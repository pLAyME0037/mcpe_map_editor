/*
 * The convert pipeline: a Bedrock store walked in full, every
 * overworld subchunk's layer zero through the translate map, a Java
 * 1.20 world written out — sections in Y order with a plains biome,
 * one region file per 32x32 of chunks, a level.dat carrying the seed
 * and the world's own name, spawn, game type and difficulty across.
 *
 * The drops are decisions.md#D12: anything not carried is counted in
 * McmeConvertReport and never fails the run, and a failed run removes
 * the output directory whole, so an output world is finished or
 * absent, never half a world.
 */

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "stb_ds.h"

#include "mcme/convert.h"
#include "mcme/bedrock_chunk.h"
#include "mcme/bedrock_keys.h"
#include "mcme/compress.h"
#include "mcme/leveldb.h"
#include "mcme/nbt.h"
#include "mcme/region.h"
#include "mcme/translate.h"
#include "mcme/world.h"

enum {
    kJavaDataVersion  = 3463,  /* Java 1.20 */
    kBlocksPerSection = 4096,
    kBitsFloor        = 4,     /* the format's minimum bits, as in
                                  java_chunk.c */
    kBitsMax          = 15
};

static const char kAir[]   = "minecraft:air";
static const char kPlains[] = "minecraft:plains";

static void refuse(McmeWriteRefusal *r, McmeWriteRefusal v)
{
    if (r != NULL) *r = v;
}

/* ========================================================================
 * Small builders
 * ======================================================================== */

static bool put_string(McmeArena *a, McmeTag *c, const char *k,
                       const char *v)
{
    McmeTag *t = mcme_nbt_new(a, MCME_TAG_String);
    return t != NULL
        && mcme_nbt_set_string(a, t, v, strlen(v))
        && mcme_nbt_compound_put(a, c, k, strlen(k), t);
}

static bool put_int(McmeArena *a, McmeTag *c, const char *k, int32_t v)
{
    McmeTag *t = mcme_nbt_new(a, MCME_TAG_Int);
    return t != NULL && mcme_nbt_set_int(t, v)
        && mcme_nbt_compound_put(a, c, k, strlen(k), t);
}

static bool put_byte(McmeArena *a, McmeTag *c, const char *k, int8_t v)
{
    McmeTag *t = mcme_nbt_new(a, MCME_TAG_Byte);
    return t != NULL && mcme_nbt_set_byte(t, v)
        && mcme_nbt_compound_put(a, c, k, strlen(k), t);
}

static bool put_long(McmeArena *a, McmeTag *c, const char *k, int64_t v)
{
    McmeTag *t = mcme_nbt_new(a, MCME_TAG_Long);
    return t != NULL && mcme_nbt_set_long(t, v)
        && mcme_nbt_compound_put(a, c, k, strlen(k), t);
}

/* Removes a path whole: a directory's contents first, then the
   directory; anything else unlinked. Missing is fine — this is the
   cleanup of a failed run, where nothing may be left to find. */
static void rm_rf(const char *path)
{
    DIR *d = opendir(path);
    if (d != NULL) {
        struct dirent *e;
        while ((e = readdir(d)) != NULL) {
            if (strcmp(e->d_name, ".") == 0
                    || strcmp(e->d_name, "..") == 0) {
                continue;
            }
            char child[4096];
            int n = snprintf(child, sizeof child, "%s/%s", path, e->d_name);
            if (n > 0 && (size_t)n < sizeof child) rm_rf(child);
        }
        closedir(d);
        rmdir(path);
    } else {
        unlink(path);
    }
}

/* ========================================================================
 * The scan: every subchunk key, in one array
 * ======================================================================== */

typedef struct {
    int32_t cx, cz;
    int8_t  cy;
    uint8_t key[14];      /* the key exactly as stored, for the get */
    uint8_t key_len;
} SubRef;

typedef struct {
    SubRef            *subs;   /* stb array */
    McmeConvertReport *rep;
} Scan;

#define MCME_BEDROCK_TAG_ENTITIES 0x32u  /* the per-chunk entity list */

static bool scan_key(void *ud, const McmeLevelEntry *e)
{
    Scan *s = (Scan *)ud;
    McmeBedrockKey k;
    if (!mcme_bedrock_key_parse(e->key, e->key_len, &k)) {
        /* Level metadata, actor records, anything else a store keeps.
           Named in the report as `other`; nothing else to do. */
        s->rep->other++;
        return true;
    }
    if (k.tag == MCME_BEDROCK_TAG_BLOCK_ENTITY) {
        s->rep->block_entities++;
        return true;
    }
    if (k.dimension != 0) {
        s->rep->other++;
        return true;
    }
    if (k.tag == MCME_BEDROCK_TAG_ENTITIES) {
        s->rep->entities++;
        return true;
    }
    bool is_sub = k.tag == MCME_BEDROCK_TAG_SUBCHUNK
        || k.tag == MCME_BEDROCK_TAG_SUBCHUNK_OLD;
    if (!is_sub) {
        /* Biome records, chunk versions, pending ticks, markers. */
        s->rep->chunk_data++;
        return true;
    }
    /* The subchunk's Y is the key's own index byte. A key without one
       is the pre-index spelling; carrying it needs the payload's own
       ordering rule, so it is counted here and named in the report. */
    if (e->key_len != 10 && e->key_len != 14) {
        s->rep->chunk_data++;
        return true;
    }
    SubRef r;
    r.cx = k.x;
    r.cz = k.z;
    r.cy = (int8_t)e->key[e->key_len - 1];
    memcpy(r.key, e->key, e->key_len);
    r.key_len = (uint8_t)e->key_len;
    arrpush(s->subs, r);
    return true;
}

static int sub_cmp(const void *a, const void *b)
{
    const SubRef *x = (const SubRef *)a;
    const SubRef *y = (const SubRef *)b;
    if (x->cx != y->cx) return (x->cx < y->cx) ? -1 : 1;
    if (x->cz != y->cz) return (x->cz < y->cz) ? -1 : 1;
    if (x->cy != y->cy) return (x->cy < y->cy) ? -1 : 1;
    return 0;
}

/* ========================================================================
 * One section: layer zero through the map, packed the Java way
 * ======================================================================== */

/* A palette slot: the Java entry, and the Bedrock source's SNBT — the
   dedup key. Two sources that spell the same translate the same, so
   one slot serves both. */
typedef struct {
    char    *snbt;    /* NULL for the seeded air slot */
    McmeTag *entry;
} Slot;

/* The Java section for one Bedrock layer. *out stays NULL when every
   position is air after the map — nothing to write, not a fault. */
static bool make_section(McmeArena *arena, int8_t cy,
                         const McmeBedrockLayer *layer,
                         McmeConvertReport *rep, McmeTag **out)
{
    *out = NULL;

    Slot *slots = NULL;
    Slot air;
    air.snbt = NULL;
    air.entry = mcme_nbt_new(arena, MCME_TAG_Compound);
    McmeTag *anm = mcme_nbt_new(arena, MCME_TAG_String);
    if (air.entry == NULL || anm == NULL
        || !mcme_nbt_compound_init(arena, air.entry, 1)
        || !mcme_nbt_set_string(arena, anm, kAir, sizeof kAir - 1)
        || !mcme_nbt_compound_put(arena, air.entry, "Name", 4, anm)) {
        arrfree(slots);
        return false;
    }
    arrpush(slots, air);   /* slot 0 is air, the drop target */

    size_t *slot_of = mcme_arena_alloc(arena,
                                       layer->palette_len * sizeof *slot_of);
    if (slot_of == NULL) {
        arrfree(slots);
        return false;
    }

    bool dropped = false;
    for (size_t i = 0; i < layer->palette_len; ++i) {
        const McmeTag *src = layer->palette[i].root;
        McmeBuffer snbt;
        mcme_buffer_init(&snbt);
        if (src == NULL
            || !mcme_nbt_write_snbt(src, &snbt, 0)
            || !mcme_buffer_append_u8(&snbt, 0)) {
            mcme_buffer_free(&snbt);
            arrfree(slots);
            return false;
        }
        size_t found = (size_t)-1;
        for (size_t j = 1; j < (size_t)arrlen(slots); ++j) {
            if (slots[j].snbt != NULL
                && strcmp(slots[j].snbt, (char *)snbt.data) == 0) {
                found = j;
                break;
            }
        }
        if (found != (size_t)-1) {
            mcme_buffer_free(&snbt);
            slot_of[i] = found;
            continue;
        }
        McmeTag *je = mcme_translate_block(arena, src);
        if (je == NULL) {
            /* A legacy entry the map refuses: its positions are air,
               and the subchunk is counted once (D12). */
            mcme_clear_error();
            dropped = true;
            mcme_buffer_free(&snbt);
            slot_of[i] = 0;
            continue;
        }
        Slot slot;
        slot.snbt = mcme_arena_strdup(arena, (char *)snbt.data);
        slot.entry = je;
        mcme_buffer_free(&snbt);
        if (slot.snbt == NULL) {
            arrfree(slots);
            return false;
        }
        arrpush(slots, slot);
        slot_of[i] = arrlen(slots) - 1;
    }
    if (dropped) rep->legacy_subchunks++;

    /* Every position, Bedrock's XZY to Java's X-then-Z-then-Y. */
    size_t jidx[kBlocksPerSection];
    bool all_air = true;
    size_t blocks = 0;
    for (int xi = 0; xi < 16; ++xi) {
        for (int zi = 0; zi < 16; ++zi) {
            for (int yi = 0; yi < 16; ++yi) {
                size_t bpos = ((size_t)xi << 8) | ((size_t)zi << 4)
                    | (size_t)yi;
                size_t jpos = (size_t)xi + (size_t)zi * 16
                    + (size_t)yi * 256;
                size_t slot = slot_of[layer->indices[bpos]];
                jidx[jpos] = slot;
                if (slot != 0) {
                    all_air = false;
                    ++blocks;
                }
            }
        }
    }
    rep->blocks += blocks;
    if (all_air) {
        arrfree(slots);
        return true;
    }

    /* Packed the way Java writes it: floor(64 / bits) entries to a
       long, none spanning two — the same rule java_chunk.c reads. */
    size_t nslots = (size_t)arrlen(slots);
    int bits = kBitsFloor;
    while (bits < kBitsMax && ((size_t)1 << bits) < nslots) ++bits;
    size_t per = 64u / (size_t)bits;
    size_t nlongs = (kBlocksPerSection + per - 1) / per;
    int64_t *longs = mcme_arena_alloc(arena,
                                      nlongs * sizeof *longs);
    if (longs == NULL) {
        arrfree(slots);
        return false;
    }
    memset(longs, 0, nlongs * sizeof *longs);
    for (size_t p = 0; p < kBlocksPerSection; ++p) {
        size_t word = p / per;
        size_t bit = (p % per) * (size_t)bits;
        longs[word] |= (int64_t)((uint64_t)jidx[p] << bit);
    }

    McmeTag *sec = mcme_nbt_new(arena, MCME_TAG_Compound);
    McmeTag *ytag = mcme_nbt_new(arena, MCME_TAG_Byte);
    if (sec == NULL || ytag == NULL
        || !mcme_nbt_compound_init(arena, sec, 3)
        || !mcme_nbt_set_byte(ytag, cy)
        || !mcme_nbt_compound_put(arena, sec, "Y", 1, ytag)) {
        arrfree(slots);
        return false;
    }

    McmeTag *pal = mcme_nbt_new(arena, MCME_TAG_List);
    if (pal == NULL
        || !mcme_nbt_list_alloc(arena, pal, MCME_TAG_Compound, nslots)) {
        arrfree(slots);
        return false;
    }
    for (size_t i = 0; i < nslots; ++i) {
        pal->list.items[i] = *slots[i].entry;
    }
    McmeTag *data = mcme_nbt_new(arena, MCME_TAG_Long_Array);
    McmeTag *bs = mcme_nbt_new(arena, MCME_TAG_Compound);
    if (data == NULL || bs == NULL
        || !mcme_nbt_set_longs(arena, data, longs, nlongs)
        || !mcme_nbt_compound_init(arena, bs, 2)
        || !mcme_nbt_compound_put(arena, bs, "palette", 7, pal)
        || !mcme_nbt_compound_put(arena, bs, "data", 4, data)
        || !mcme_nbt_compound_put(arena, sec, "block_states", 12, bs)) {
        arrfree(slots);
        return false;
    }

    /* Biomes: the one-entry palette the format takes as shorthand.
       A plains default until the biome map is a later entry. */
    McmeTag *biomes = mcme_nbt_new(arena, MCME_TAG_Compound);
    McmeTag *bpal = mcme_nbt_new(arena, MCME_TAG_List);
    McmeTag *bp = mcme_nbt_new(arena, MCME_TAG_String);
    if (biomes == NULL || bpal == NULL || bp == NULL
        || !mcme_nbt_compound_init(arena, biomes, 1)
        || !mcme_nbt_list_alloc(arena, bpal, MCME_TAG_String, 1)
        || !mcme_nbt_set_string(arena, bp, kPlains, sizeof kPlains - 1)) {
        arrfree(slots);
        return false;
    }
    bpal->list.items[0] = *bp;
    if (!mcme_nbt_compound_put(arena, biomes, "palette", 7, bpal)
        || !mcme_nbt_compound_put(arena, sec, "biomes", 6, biomes)) {
        arrfree(slots);
        return false;
    }

    arrfree(slots);
    *out = sec;
    return true;
}

/* ========================================================================
 * One chunk: every subchunk, one region-file write
 * ======================================================================== */

static bool convert_chunk(McmeLevelDb *db, const char *out,
                          const SubRef *subs, size_t n,
                          McmeConvertReport *rep,
                          McmeWriteRefusal *refusal)
{
    McmeArena *arena = mcme_arena_new();
    if (arena == NULL) {
        mcme_set_error("out of memory");
        refuse(refusal, MCME_WRITE_REFUSAL_INTERNAL);
        return false;
    }

    McmeTag *root = mcme_nbt_new(arena, MCME_TAG_Compound);
    McmeTag *sections = mcme_nbt_new(arena, MCME_TAG_List);
    if (root == NULL || sections == NULL
        || !mcme_nbt_compound_init(arena, root, 8)) {
        mcme_set_error("out of memory");
        refuse(refusal, MCME_WRITE_REFUSAL_INTERNAL);
        mcme_arena_free(arena);
        return false;
    }

    McmeTag **built = NULL;
    for (size_t i = 0; i < n; ++i) {
        McmeBuffer val;
        mcme_buffer_init(&val);
        bool deleted = false;
        if (!mcme_leveldb_get(db, subs[i].key, subs[i].key_len, &val,
                              &deleted)) {
            mcme_buffer_free(&val);
            mcme_set_error("the subchunk at (%d, %d, %d) is "
                           "gone from the store", subs[i].cx,
                           (int)subs[i].cy, subs[i].cz);
            refuse(refusal, MCME_WRITE_REFUSAL_STORE);
            arrfree(built);
            mcme_arena_free(arena);
            return false;
        }
        if (deleted) {
            mcme_buffer_free(&val);
            rep->legacy_subchunks++;
            continue;
        }
        McmeBedrockSubchunk sc;
        if (!mcme_bedrock_subchunk_decode(&sc, arena, val.data,
                                          val.len)) {
            /* A payload this build does not decode — the pre-palette
               versions — drops the subchunk as air, counted (D12). */
            mcme_buffer_free(&val);
            mcme_clear_error();
            rep->legacy_subchunks++;
            continue;
        }
        mcme_buffer_free(&val);
        rep->subchunks++;
        if (sc.layer_count == 0) continue;   /* the all-air marker */
        if (sc.layer_count > 1) rep->water_layers++;
        McmeTag *sec = NULL;
        if (!make_section(arena, subs[i].cy, &sc.layers[0], rep,
                          &sec)) {
            mcme_set_error("out of memory building the "
                           "section at (%d, %d, %d)", subs[i].cx,
                           (int)subs[i].cy, subs[i].cz);
            refuse(refusal, MCME_WRITE_REFUSAL_INTERNAL);
            arrfree(built);
            mcme_arena_free(arena);
            return false;
        }
        if (sec != NULL) arrpush(built, sec);
    }

    size_t nsec = (size_t)arrlen(built);
    if (!mcme_nbt_list_alloc(arena, sections, MCME_TAG_Compound,
                             nsec)) {
        mcme_set_error("out of memory");
        refuse(refusal, MCME_WRITE_REFUSAL_INTERNAL);
        arrfree(built);
        mcme_arena_free(arena);
        return false;
    }
    for (size_t i = 0; i < nsec; ++i) {
        sections->list.items[i] = *built[i];
    }

    int32_t cx = subs[0].cx;
    int32_t cz = subs[0].cz;
    bool ok = put_int(arena, root, "DataVersion", kJavaDataVersion)
        && put_string(arena, root, "Status", "minecraft:full")
        && put_int(arena, root, "xPos", cx)
        && put_int(arena, root, "zPos", cz)
        && put_int(arena, root, "yPos", -4)
        && put_long(arena, root, "LastUpdate", 0)
        && put_long(arena, root, "InhabitedTime", 0)
        && mcme_nbt_compound_put(arena, root, "sections", 8, sections);
    McmeTag *entities = mcme_nbt_new(arena, MCME_TAG_List);
    ok = ok && entities != NULL
        && mcme_nbt_list_alloc(arena, entities, MCME_TAG_Byte, 0)
        && mcme_nbt_compound_put(arena, root, "block_entities", 14,
                                 entities);
    if (!ok) {
        mcme_set_error("out of memory building the chunk at "
                       "(%d, %d)", cx, cz);
        refuse(refusal, MCME_WRITE_REFUSAL_INTERNAL);
        arrfree(built);
        mcme_arena_free(arena);
        return false;
    }

    char file[4096];
    int w = snprintf(file, sizeof file, "%s/region/r.%d.%d.mca", out,
                     cx >> 5, cz >> 5);
    if (w <= 0 || (size_t)w >= sizeof file) {
        mcme_set_error("the region path for chunk (%d, %d) "
                       "does not fit", cx, cz);
        refuse(refusal, MCME_WRITE_REFUSAL_INTERNAL);
        arrfree(built);
        mcme_arena_free(arena);
        return false;
    }
    bool written = mcme_region_set_chunk(file, cx, cz, root, arena, 0,
                                         refusal);
    if (written) rep->chunks++;
    arrfree(built);
    mcme_arena_free(arena);
    return written;
}

/* ========================================================================
 * The Java level.dat
 * ======================================================================== */

/* One dimension of the vanilla 1.20 generation settings, so terrain
   the game has not generated yet comes from the same seed the Bedrock
   world recorded. */
static bool dimension(McmeArena *a, McmeTag *dims, const char *key,
                      const char *type, const char *preset,
                      const char *settings, bool fixed)
{
    McmeTag *dim = mcme_nbt_new(a, MCME_TAG_Compound);
    McmeTag *gen = mcme_nbt_new(a, MCME_TAG_Compound);
    McmeTag *bs = mcme_nbt_new(a, MCME_TAG_Compound);
    if (dim == NULL || gen == NULL || bs == NULL
        || !mcme_nbt_compound_init(a, dim, 2)
        || !mcme_nbt_compound_init(a, gen, 3)
        || !mcme_nbt_compound_init(a, bs, 2)
        || !put_string(a, dim, "type", type)
        || !put_string(a, gen, "type", "minecraft:noise")
        || !put_string(a, bs, "type",
                       fixed ? "minecraft:fixed"
                             : "minecraft:multi_noise")
        || !put_string(a, bs, fixed ? "biome" : "preset", preset)
        || !mcme_nbt_compound_put(a, gen, "biome_source", 12, bs)
        || !put_string(a, gen, "settings", settings)
        || !mcme_nbt_compound_put(a, dim, "generator", 9, gen)
        || !mcme_nbt_compound_put(a, dims, key, strlen(key), dim)) {
        return false;
    }
    return true;
}

/* The output level.dat: gzip, big-endian, the fields a Java world
   needs to open and to keep generating from the same seed. */
static bool java_level_dat(const McmeTag *scope, McmeArena *arena,
                           McmeBuffer *gz)
{
    McmeTag *data = mcme_nbt_new(arena, MCME_TAG_Compound);
    if (data == NULL || !mcme_nbt_compound_init(arena, data, 12)) {
        return false;
    }

    const McmeTag *name = mcme_nbt_get(scope, "LevelName");
    if (name != NULL && name->type == MCME_TAG_String) {
        McmeTag *t = mcme_nbt_new(arena, MCME_TAG_String);
        if (t == NULL
            || !mcme_nbt_set_string(arena, t, name->string.data,
                                    name->string.len)
            || !mcme_nbt_compound_put(arena, data, "LevelName", 9, t)) {
            return false;
        }
    }

    int64_t seed     = mcme_nbt_long_or(mcme_nbt_get(scope, "RandomSeed"), 0);
    int32_t gametype = mcme_nbt_int_or(mcme_nbt_get(scope, "GameType"), 0);
    int32_t spawn_x  = mcme_nbt_int_or(mcme_nbt_get(scope, "SpawnX"), 0);
    int32_t spawn_y  = mcme_nbt_int_or(mcme_nbt_get(scope, "SpawnY"), 64);
    int32_t spawn_z  = mcme_nbt_int_or(mcme_nbt_get(scope, "SpawnZ"), 0);
    int64_t tick     = mcme_nbt_long_or(mcme_nbt_get(scope, "CurrentTick"), 0);

    McmeTag *version = mcme_nbt_new(arena, MCME_TAG_Compound);
    McmeTag *wgs     = mcme_nbt_new(arena, MCME_TAG_Compound);
    McmeTag *dims    = mcme_nbt_new(arena, MCME_TAG_Compound);
    if (version == NULL || wgs == NULL || dims == NULL
        || !mcme_nbt_compound_init(arena, version, 3)
        || !mcme_nbt_compound_init(arena, wgs, 4)
        || !mcme_nbt_compound_init(arena, dims, 3)
        || !put_string(arena, version, "Name", "1.20")
        || !put_int(arena, version, "Id", kJavaDataVersion)
        || !put_byte(arena, version, "Snapshot", 0)
        || !dimension(arena, dims, "minecraft:overworld",
                      "minecraft:overworld", "minecraft:overworld",
                      "minecraft:overworld", false)
        || !dimension(arena, dims, "minecraft:the_nether",
                      "minecraft:the_nether", "minecraft:nether",
                      "minecraft:nether", false)
        || !dimension(arena, dims, "minecraft:the_end",
                      "minecraft:the_end", "minecraft:the_end",
                      "minecraft:the_end", true)
        || !put_long(arena, wgs, "seed", seed)
        || !put_byte(arena, wgs, "bonus_chest", 0)
        || !put_byte(arena, wgs, "generate_features", 1)
        || !mcme_nbt_compound_put(arena, wgs, "dimensions", 10, dims)) {
        return false;
    }

    bool ok = put_int(arena, data, "DataVersion", kJavaDataVersion)
        && mcme_nbt_compound_put(arena, data, "version", 7, version)
        && put_int(arena, data, "GameType", gametype)
        && put_byte(arena, data, "hardcore", 0)
        && put_byte(arena, data, "initialized", 1)
        && put_int(arena, data, "SpawnX", spawn_x)
        && put_int(arena, data, "SpawnY", spawn_y)
        && put_int(arena, data, "SpawnZ", spawn_z)
        && put_long(arena, data, "Time", tick)
        && put_long(arena, data, "DayTime", tick)
        && mcme_nbt_compound_put(arena, data, "WorldGenSettings", 16, wgs);
    if (!ok) return false;

    /* Bedrock keeps its own difficulty as an int on the same 0-3
       scale; carry it when the world recorded one. */
    const McmeTag *diff = mcme_nbt_get(scope, "Difficulty");
    if (diff != NULL
            && !put_int(arena, data, "Difficulty",
                        mcme_nbt_int_or(diff, 1))) {
        return false;
    }

    McmeTag *root = mcme_nbt_new(arena, MCME_TAG_Compound);
    if (root == NULL || !mcme_nbt_compound_init(arena, root, 1)
        || !mcme_nbt_compound_put(arena, root, "Data", 4, data)) {
        return false;
    }

    McmeNbtDoc doc;
    doc.arena = arena;
    doc.root = root;
    doc.little_endian = false;
    if (!mcme_nbt_str_set(arena, &doc.root_name, "", 0)) {
        return false;
    }
    McmeBuffer raw;
    mcme_buffer_init(&raw);
    if (!mcme_nbt_write_binary(&doc, &raw)) {
        mcme_buffer_free(&raw);
        return false;
    }
    bool gz_ok = mcme_compress(MCME_COMPRESS_GZIP, raw.data, raw.len, gz);
    mcme_buffer_free(&raw);
    return gz_ok;
}

/* ========================================================================
 * The run
 * ======================================================================== */

bool mcme_convert_bedrock_java(const char *in, const char *out,
                               McmeConvertReport *report,
                               McmeWriteRefusal *refusal)
{
    McmeConvertReport zeros;
    if (report == NULL) report = &zeros;
    memset(report, 0, sizeof *report);
    refuse(refusal, MCME_WRITE_REFUSAL_NONE);

    if (in == NULL || out == NULL) {
        mcme_set_error("null argument");
        refuse(refusal, MCME_WRITE_REFUSAL_USAGE);
        return false;
    }
    if (mcme_fs_exists(out)) {
        mcme_set_error("'%s' exists; a conversion writes a "
                       "new world", out);
        refuse(refusal, MCME_WRITE_REFUSAL_USAGE);
        return false;
    }

    McmeWorldInfo info;
    if (!mcme_world_probe(&info, in)) {
        refuse(refusal, MCME_WRITE_REFUSAL_NOT_THERE);
        return false;
    }
    if (info.kind != MCME_WORLD_BEDROCK) {
        mcme_set_error("'%s' is not a Bedrock world", in);
        mcme_world_info_free(&info);
        refuse(refusal, MCME_WRITE_REFUSAL_NOT_READABLE);
        return false;
    }

    /* The level.dat comes first: the seed is the one thing a
       converted world cannot be whole without, and reading it here
       means a world without one has not yet created an output. */
    char lvl[4096];
    int w = snprintf(lvl, sizeof lvl, "%s/level.dat", in);
    if (w <= 0 || (size_t)w >= sizeof lvl) {
        mcme_set_error("the input's level.dat path does not "
                       "fit");
        mcme_world_info_free(&info);
        refuse(refusal, MCME_WRITE_REFUSAL_INTERNAL);
        return false;
    }
    if (!mcme_fs_exists(lvl)) {
        mcme_set_error("'%s' has no level.dat to carry the "
                       "seed from", in);
        mcme_world_info_free(&info);
        refuse(refusal, MCME_WRITE_REFUSAL_NOT_READABLE);
        return false;
    }
    McmeArena *arena = mcme_arena_new();
    McmeNbtDoc doc;
    McmeLevelDatFrame frame;
    if (arena == NULL) {
        mcme_set_error("out of memory");
        mcme_world_info_free(&info);
        refuse(refusal, MCME_WRITE_REFUSAL_INTERNAL);
        return false;
    }
    if (!mcme_level_dat_parse(&doc, arena, lvl, &frame)) {
        mcme_arena_free(arena);
        mcme_world_info_free(&info);
        refuse(refusal, MCME_WRITE_REFUSAL_NOT_READABLE);
        return false;
    }

    /* Bedrock keeps its fields at the root or under Data; world.c
       looks in both, and so does this. */
    const McmeTag *data = mcme_nbt_get(doc.root, "Data");
    const McmeTag *scope = (data != NULL
                            && data->type == MCME_TAG_Compound)
        ? data : doc.root;

    McmeBuffer gz;
    mcme_buffer_init(&gz);
    if (!java_level_dat(scope, arena, &gz)) {
        mcme_buffer_free(&gz);
        mcme_arena_free(arena);
        mcme_world_info_free(&info);
        refuse(refusal, MCME_WRITE_REFUSAL_INTERNAL);
        return false;
    }
    mcme_arena_free(arena);   /* the built tree only fed the buffer */

    /* From here the output exists, and every failure removes it. */
    if (!mcme_fs_mkdir_p(out)) {
        mcme_buffer_free(&gz);
        mcme_world_info_free(&info);
        refuse(refusal, MCME_WRITE_REFUSAL_STORE);
        return false;
    }
    char region_dir[4096];
    w = snprintf(region_dir, sizeof region_dir, "%s/region", out);
    bool made = w > 0 && (size_t)w < sizeof region_dir
        && mcme_fs_mkdir_p(region_dir);
    char leveldat[4096];
    w = snprintf(leveldat, sizeof leveldat, "%s/level.dat", out);
    bool wrote_level = made && w > 0 && (size_t)w < sizeof leveldat
        && mcme_fs_write(leveldat, gz.data, gz.len);
    mcme_buffer_free(&gz);
    if (!wrote_level) {
        if (mcme_last_error()[0] == '\0') {
            mcme_set_error("cannot write the output world "
                           "under '%s'", out);
        }
        rm_rf(out);
        mcme_world_info_free(&info);
        refuse(refusal, MCME_WRITE_REFUSAL_STORE);
        return false;
    }

    McmeLevelDb *db = NULL;
    if (!mcme_leveldb_open(&db, info.db_path)) {
        rm_rf(out);
        mcme_world_info_free(&info);
        refuse(refusal, MCME_WRITE_REFUSAL_STORE);
        return false;
    }

    Scan scan;
    scan.subs = NULL;
    scan.rep = report;
    mcme_leveldb_iterate(db, scan_key, &scan);
    size_t nsubs = (size_t)arrlen(scan.subs);
    qsort(scan.subs, nsubs, sizeof (SubRef), sub_cmp);

    bool ok = true;
    size_t i = 0;
    while (ok && i < nsubs) {
        size_t j = i;
        while (j < nsubs && scan.subs[j].cx == scan.subs[i].cx
               && scan.subs[j].cz == scan.subs[i].cz) {
            ++j;
        }
        ok = convert_chunk(db, out, scan.subs + i, j - i, report, refusal);
        i = j;
    }

    arrfree(scan.subs);
    mcme_leveldb_close(db);
    mcme_world_info_free(&info);
    if (!ok) rm_rf(out);
    return ok;
}
