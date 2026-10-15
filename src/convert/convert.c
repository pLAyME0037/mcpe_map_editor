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
#include "mcme/bedrock_entity.h"
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

/* The same, for a name the map hands back with its own length — a
   parsed NBT string is not NUL-terminated, so strlen would read past
   it. */
static bool put_stringn(McmeArena *a, McmeTag *c, const char *k,
                        const char *v, size_t vlen)
{
    McmeTag *t = mcme_nbt_new(a, MCME_TAG_String);
    return t != NULL
        && mcme_nbt_set_string(a, t, v, vlen)
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
    int32_t cx, cz;
} BeRef;                     /* a chunk's block-entity record       */

typedef struct {
    int32_t cx, cz;
    uint8_t tag;             /* 0x33 or 0x3a: which record this is  */
} TickRef;                   /* a chunk's tick record               */

typedef struct {
    SubRef            *subs;   /* stb array */
    BeRef             *bes;    /* stb array */
    TickRef           *ticks;  /* stb array */
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
    if (k.dimension != 0) {
        /* Every key of another dimension is `other` — block-entity
           records included. One rule for every tag: the convert walks
           the overworld, and a nether key is a nether key. */
        s->rep->other++;
        return true;
    }
    if (k.tag == MCME_BEDROCK_TAG_BLOCK_ENTITY) {
        /* Collected, not counted here. A record whose chunk converts
           is carried or dropped document by document where the chunk
           is written; a record whose chunk holds no subchunks is
           counted after the chunk loop. Counting a record twice, or
           never, would both be lies. */
        BeRef b;
        b.cx = k.x;
        b.cz = k.z;
        arrpush(s->bes, b);
        return true;
    }
    if (k.tag == MCME_BEDROCK_TAG_PENDING_TICKS
            || k.tag == MCME_BEDROCK_TAG_RANDOM_TICKS) {
        /* Collected like the block-entity records: a record whose
           chunk converts carries its entries, a record whose chunk
           holds no subchunks counts them after the chunk loop, and
           neither count is guessed here (D18). */
        TickRef t;
        t.cx = k.x;
        t.cz = k.z;
        t.tag = k.tag;
        arrpush(s->ticks, t);
        return true;
    }
    if (k.tag == MCME_BEDROCK_TAG_ENTITIES) {
        s->rep->entities++;
        return true;
    }
    bool is_sub = k.tag == MCME_BEDROCK_TAG_SUBCHUNK
        || k.tag == MCME_BEDROCK_TAG_SUBCHUNK_OLD;
    if (!is_sub) {
        /* Biome records, chunk versions, markers. */
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

static int be_cmp(const void *a, const void *b)
{
    const BeRef *x = (const BeRef *)a;
    const BeRef *y = (const BeRef *)b;
    if (x->cx != y->cx) return (x->cx < y->cx) ? -1 : 1;
    if (x->cz != y->cz) return (x->cz < y->cz) ? -1 : 1;
    return 0;
}

/* One chunk may hold both tick records; the tag tiebreak keeps the
   0x33 entries ahead of the 0x3a ones, so a carried list's order is
   the scan's order, not the sort's mood. */
static int tick_cmp(const void *a, const void *b)
{
    const TickRef *x = (const TickRef *)a;
    const TickRef *y = (const TickRef *)b;
    if (x->cx != y->cx) return (x->cx < y->cx) ? -1 : 1;
    if (x->cz != y->cz) return (x->cz < y->cz) ? -1 : 1;
    if (x->tag != y->tag) return (x->tag < y->tag) ? -1 : 1;
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

/* ========================================================================
 * Waterlogging: Bedrock's second layer becomes Java's property
 *
 * Bedrock keeps waterlogged water beside block zero as a second layer
 * of the subchunk; Java spells the same fact `waterlogged: "true"` on
 * the block itself. Carrying it therefore needs two answers: is this
 * Java block one that has the property at all, and is the layer cell
 * water (a layer cell of anything else must not become water — a lie
 * the reference would catch).
 *
 * The set of property-bearing blocks is the reference's own:
 * PyMCTranslate's is_waterloggable over java/1.20 answers for all
 * 1003 blocks — 310 yes, 693 no — and ref_convert pins this table to
 * that answer position by position. The table is names, never a
 * pattern over names: every suffix family below was enumerated
 * against the reference until the two sets matched name for name.
 * ======================================================================== */

static const char * const kWaterlogExact[] = {
    "minecraft:amethyst_cluster",
    "minecraft:calibrated_sculk_sensor",
    "minecraft:candle",
    "minecraft:chain",
    "minecraft:chest",
    "minecraft:conduit",
    "minecraft:campfire",
    "minecraft:decorated_pot",
    "minecraft:glow_lichen",
    "minecraft:hanging_roots",
    "minecraft:iron_bars",
    "minecraft:ladder",
    "minecraft:lantern",
    "minecraft:light",
    "minecraft:lightning_rod",
    "minecraft:mangrove_propagule",
    "minecraft:mangrove_roots",
    "minecraft:pointed_dripstone",
    "minecraft:rail",
    "minecraft:scaffolding",
    "minecraft:sculk_sensor",
    "minecraft:sculk_shrieker",
    "minecraft:sculk_vein",
    "minecraft:sea_pickle",
    "minecraft:soul_campfire",
    "minecraft:soul_lantern",
};

static const char * const kWaterlogSuffixes[] = {
    "_bud", "_candle", "_chest", "_coral", "_coral_fan",
    "_coral_wall_fan", "_dripleaf", "_dripleaf_stem", "_fence",
    "_leaves", "_pane", "_rail", "_sign", "_slab", "_stairs",
    "_trapdoor", "_wall",
};

static bool ends_with(const char *s, size_t len, const char *suffix)
{
    size_t n = strlen(suffix);
    return len >= n && memcmp(s + len - n, suffix, n) == 0;
}

/* Whether the Java entry `name` carries the `waterlogged` property. */
static bool waterloggable(const char *name, size_t len)
{
    for (size_t i = 0; i < sizeof kWaterlogExact / sizeof kWaterlogExact[0];
         ++i) {
        if (strlen(kWaterlogExact[i]) == len
                && memcmp(name, kWaterlogExact[i], len) == 0) {
            return true;
        }
    }
    for (size_t i = 0;
         i < sizeof kWaterlogSuffixes / sizeof kWaterlogSuffixes[0]; ++i) {
        if (ends_with(name, len, kWaterlogSuffixes[i])) return true;
    }
    return false;
}

/* The waterlogged twin of a palette slot: the same Java entry with
   `waterlogged: "true"` — properties the entry already carries are
   copied, because the second layer says nothing about them, and the
   slot's source-SNBT key is dropped: only source entries dedup, and
   this one is derived. */
static bool make_waterlogged_variant(McmeArena *arena, const Slot *base,
                                     Slot *out)
{
    out->snbt = NULL;
    out->entry = mcme_nbt_new(arena, MCME_TAG_Compound);
    const McmeTag *bname = mcme_nbt_get(base->entry, "Name");
    const McmeTag *bprops = mcme_nbt_get(base->entry, "Properties");
    if (out->entry == NULL || bname == NULL
            || !mcme_nbt_compound_init(arena, out->entry, 2)
            || !mcme_nbt_compound_put(arena, out->entry, "Name", 4,
                                      (McmeTag *)bname)) {
        return false;
    }

    McmeTag *props = mcme_nbt_new(arena, MCME_TAG_Compound);
    if (props == NULL || !mcme_nbt_compound_init(arena, props, 4)) {
        return false;
    }
    if (bprops != NULL && bprops->type == MCME_TAG_Compound) {
        for (size_t i = 0; i < bprops->compound.len; ++i) {
            if (!mcme_nbt_compound_put(arena, props,
                                       bprops->compound.entries[i].name.data,
                                       bprops->compound.entries[i].name.len,
                                       bprops->compound.entries[i].value)) {
                return false;
            }
        }
    }

    McmeTag *wl = mcme_nbt_new(arena, MCME_TAG_String);
    if (wl == NULL
            || !mcme_nbt_set_string(arena, wl, "true", 4)
            || !mcme_nbt_compound_put(arena, props, "waterlogged", 11, wl)
            || !mcme_nbt_compound_put(arena, out->entry, "Properties", 10,
                                      props)) {
        return false;
    }
    return true;
}

/* What a second-layer palette entry is: air (nothing there), water
   (the waterlogging layer proper), or something mcme must not
   translate into a property it only resembles. */
enum { kExtraAir = 0, kExtraWater = 1, kExtraOther = 2 };

static uint8_t extra_cell_kind(const McmeBedrockLayer *extra, size_t idx)
{
    const McmeTag *src = extra->palette[idx].root;
    const McmeTag *nm = src != NULL ? mcme_nbt_get(src, "name") : NULL;
    if (nm == NULL || nm->type != MCME_TAG_String) return kExtraOther;
    if (nm->string.len == sizeof kAir - 1
            && memcmp(nm->string.data, kAir, sizeof kAir - 1) == 0) {
        return kExtraAir;
    }
    if (nm->string.len == 15
            && memcmp(nm->string.data, "minecraft:water", 15) == 0) {
        return kExtraWater;
    }
    return kExtraOther;
}

/* The Java section for one Bedrock layer. `extra`, when not NULL, is
   the subchunk's second layer — the waterlogging half, folded into
   the Java blocks' `waterlogged` property where the block has one and
   counted dropped where it does not. *out stays NULL when every
   position is air after the map — nothing to write, not a fault. */
static bool make_section(McmeArena *arena, int8_t cy,
                         const McmeBedrockLayer *layer,
                         const McmeBedrockLayer *extra,
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

    /* Every position, Bedrock's XZY to Java's X-then-Z-then-Y. The
       second layer is folded in per position: water where the Java
       block takes the property, a counted drop where it does not. */
    uint8_t *extra_kind = NULL;
    size_t *wl_of = NULL;    /* base slot -> waterlogged variant */
    if (extra != NULL) {
        for (size_t i = 0; i < extra->palette_len; ++i) {
            arrpush(extra_kind, extra_cell_kind(extra, i));
        }
    }

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
                if (extra != NULL) {
                    uint8_t kind = extra_kind[extra->indices[bpos]];
                    if (kind != kExtraAir) {
                        const McmeTag *nm = mcme_nbt_get(
                            slots[slot].entry, "Name");
                        bool carried = kind == kExtraWater
                            && nm != NULL && nm->type == MCME_TAG_String
                            && waterloggable(nm->string.data,
                                             nm->string.len);
                        if (carried) {
                            while ((size_t)arrlen(wl_of) <= slot) {
                                arrpush(wl_of, (size_t)-1);
                            }
                            if (wl_of[slot] == (size_t)-1) {
                                Slot v;
                                if (!make_waterlogged_variant(arena,
                                                              &slots[slot],
                                                              &v)) {
                                    arrfree(slots);
                                    arrfree(extra_kind);
                                    arrfree(wl_of);
                                    return false;
                                }
                                arrpush(slots, v);
                                wl_of[slot] = arrlen(slots) - 1;
                            }
                            slot = wl_of[slot];
                        } else {
                            /* Water beside a block with no such
                               property, or a layer cell that is not
                               water: dropped and counted — turning it
                               into a property the block does not have
                               would be an edit no reader agrees on. */
                            rep->water_positions++;
                        }
                    }
                }
                jidx[jpos] = slot;
                if (slot != 0) {
                    all_air = false;
                    ++blocks;
                }
            }
        }
    }
    arrfree(extra_kind);
    arrfree(wl_of);
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

/* ========================================================================
 * Block entities: the families whose Java spelling is known
 *
 * A document is carried when two things hold: its id is one Java
 * spells exactly as Bedrock does — so the mapping is identity and
 * there is no name mcme could invent — and its own x/y/z name a
 * block, because a Java chunk's block_entities list places entries by
 * those coordinates. Everything else drops and is counted (D12).
 *
 * The game writes its savegame ids short ("Chest"); the frozen golden
 * fixture and mcme's own writes spell them "minecraft:chest". Both
 * spellings carry; the answer is always Java's namespaced id.
 *
 * What travels inside a carried document: id, x, y, z; Items (below);
 * a nonempty CustomName; Lock; the smelter timers as Java's ints.
 * What does not: fields with no Java spelling to carry to —
 * keepPacked, the pair coordinates double chests record, anything
 * else the game adds. A document the world has not filled yet — one
 * with a LootTable — drops entirely (see carry_block_entity). The
 * report counts whole documents and whole stacks, never individual
 * fields.
 * ======================================================================== */

/*
 * The containers the carry maps, under the game's own savegame ids —
 * "Chest", "BlastFurnace" — and Java's namespaced id beside each.
 *
 * The game writes the short form into its records (measured: every
 * one of the ten, thousands of documents, in a real 2026 world); the
 * frozen golden fixture and mcme's own writes spell the id
 * "minecraft:chest". Both spellings carry; the answer is always
 * Java's.
 */
static const struct {
    const char *game;    /* the savegame id, as the game spells it */
    const char *java;    /* Java's namespaced id, the output       */
} kCarriedBlockEntities[] = {
    {"Chest",        "minecraft:chest"},
    {"TrappedChest", "minecraft:trapped_chest"},
    {"EnderChest",   "minecraft:ender_chest"},
    {"Barrel",       "minecraft:barrel"},
    {"Hopper",       "minecraft:hopper"},
    {"Furnace",      "minecraft:furnace"},
    {"BlastFurnace", "minecraft:blast_furnace"},
    {"Smoker",       "minecraft:smoker"},
    {"Dispenser",    "minecraft:dispenser"},
    {"Dropper",      "minecraft:dropper"},
};

/* Java's id for this document's, or NULL: the id names no container
   the mapping knows. The namespaced spelling is the short one with
   the prefix stripped, compared without case — "minecraft:chest" and
   "Chest" are the same container in two writers' hands. */
static const char *carried_java_id(const char *id, size_t len)
{
    static const char kNs[] = "minecraft:";
    if (len > sizeof kNs - 1
            && memcmp(id, kNs, sizeof kNs - 1) == 0) {
        id += sizeof kNs - 1;
        len -= sizeof kNs - 1;
    }
    for (size_t i = 0;
         i < sizeof kCarriedBlockEntities
             / sizeof kCarriedBlockEntities[0];
         ++i) {
        const char *game = kCarriedBlockEntities[i].game;
        if (strlen(game) != len) continue;
        bool same = true;
        for (size_t j = 0; j < len; ++j) {
            char a = id[j], b = game[j];
            if (a >= 'A' && a <= 'Z') a += 32;
            if (b >= 'A' && b <= 'Z') b += 32;
            if (a != b) { same = false; break; }
        }
        if (same) return kCarriedBlockEntities[i].java;
    }
    return NULL;
}

/* The verdict for one document: carried into the list, dropped and
   counted, or an allocation failure — which fails the run, because a
   conversion that cannot finish its arithmetic is not a conversion
   that guesses. */
typedef enum { BE_CARRY, BE_DROP, BE_FAIL } BeVerdict;

/* The integer at `key`, from the numeric types a store actually
   holds. False for a field that is absent or spelled another way —
   the caller then treats it as absent, never as zero. */
static bool int_at(const McmeTag *root, const char *key, int32_t *out)
{
    const McmeTag *t = mcme_nbt_get(root, key);
    if (t == NULL) return false;
    switch (t->type) {
    case MCME_TAG_Byte:  *out = t->as_byte;  return true;
    case MCME_TAG_Short: *out = t->as_short; return true;
    case MCME_TAG_Int:   *out = t->as_int;   return true;
    default: return false;
    }
}

/*
 * A stack's `tag` holding exactly {Damage: 0} is the game's
 * undamaged marker: no damage, nothing else — the Java answer is no
 * tag at all, so the stack carries and the marker stays behind.
 */
static bool tag_is_undamaged_marker(const McmeTag *tag)
{
    return tag != NULL && tag->type == MCME_TAG_Compound
        && tag->compound.len == 1
        && tag->compound.entries[0].name.len == 6
        && memcmp(tag->compound.entries[0].name.data, "Damage", 6) == 0
        && tag->compound.entries[0].value != NULL
        && tag->compound.entries[0].value->type == MCME_TAG_Int
        && tag->compound.entries[0].value->as_int == 0;
}

/* A block item's `Block` compound is the game's own record of the
   block the item is — name, states, version, nothing item-specific
   (measured: all of them, in a real world, hold exactly these
   three). The item's id already says the same thing in Java's
   spelling, so the stack carries and the compound stays behind. Any
   other content — a shulker box's stored contents, for instance —
   is data mcme cannot translate, and the stack drops. */
static bool block_is_identity(const McmeTag *b)
{
    if (b == NULL || b->type != MCME_TAG_Compound
            || b->compound.len != 3) {
        return false;
    }
    bool name = false, states = false, version = false;
    for (size_t i = 0; i < b->compound.len; ++i) {
        const McmeStr *k = &b->compound.entries[i].name;
        const McmeTag *v = b->compound.entries[i].value;
        if (k->len == 4 && memcmp(k->data, "name", 4) == 0) {
            name = v != NULL && v->type == MCME_TAG_String;
        } else if (k->len == 6
                && memcmp(k->data, "states", 6) == 0) {
            states = v != NULL && v->type == MCME_TAG_Compound;
        } else if (k->len == 7
                && memcmp(k->data, "version", 7) == 0) {
            version = v != NULL && v->type == MCME_TAG_Int;
        } else {
            return false;
        }
    }
    return name && states && version;
}

/*
 * One item stack: the game's spelling is Slot, Name, Count, Damage —
 * Name is the item id, Count the amount, Damage the metadata short
 * (the wiki's item format, and every stack in a real world). The
 * frozen golden fixture spells the same data id/count/damage, so
 * that spelling carries too. Out: Slot, id, Count under Java's
 * names, a nonzero damage as Java's tag.Damage.
 *
 * Three fields the game puts on stacks hold no item data Java
 * needs: WasPickedUp's pickup bookkeeping, the block identity a
 * block item repeats, and the {Damage: 0} undamaged marker. They
 * drop with the stack's answer intact. Every other field — an
 * enchantment list, a display name, stored contents — is item data
 * mcme cannot translate, and a stack mcme cannot translate is not a
 * stack mcme half-writes: the stack drops and the caller counts
 * it. */
static McmeTag *carry_stack(McmeArena *arena, const McmeTag *src,
                            bool *dropped)
{
    *dropped = false;
    if (src == NULL || src->type != MCME_TAG_Compound) {
        *dropped = true;
        return NULL;
    }
    const McmeTag *id = mcme_nbt_get(src, "Name");
    if (id == NULL || id->type != MCME_TAG_String) {
        id = mcme_nbt_get(src, "id");
    }
    int32_t slot = 0, count = 0;
    if (id == NULL || id->type != MCME_TAG_String
            || !int_at(src, "Slot", &slot)
            || !(int_at(src, "Count", &count)
                || int_at(src, "count", &count))) {
        *dropped = true;
        return NULL;
    }
    for (size_t i = 0; i < src->compound.len; ++i) {
        const char *k = src->compound.entries[i].name.data;
        size_t kl = src->compound.entries[i].name.len;
        const McmeTag *v = src->compound.entries[i].value;
        if ((kl == 4 && memcmp(k, "Slot", 4) == 0)
                || (kl == 4 && memcmp(k, "Name", 4) == 0)
                || (kl == 2 && memcmp(k, "id", 2) == 0)
                || (kl == 5 && memcmp(k, "Count", 5) == 0)
                || (kl == 5 && memcmp(k, "count", 5) == 0)
                || (kl == 6 && memcmp(k, "Damage", 6) == 0)
                || (kl == 6 && memcmp(k, "damage", 6) == 0)) {
            continue;
        }
        if ((kl == 11 && memcmp(k, "WasPickedUp", 11) == 0)
                || (kl == 5 && memcmp(k, "Block", 5) == 0
                    && block_is_identity(v))
                || (kl == 3 && memcmp(k, "tag", 3) == 0
                    && tag_is_undamaged_marker(v))) {
            continue;
        }
        *dropped = true;
        return NULL;
    }

    McmeTag *out = mcme_nbt_new(arena, MCME_TAG_Compound);
    McmeTag *jt = mcme_nbt_new(arena, MCME_TAG_Byte);
    McmeTag *jc = mcme_nbt_new(arena, MCME_TAG_Byte);
    if (out == NULL || jt == NULL || jc == NULL
            || !mcme_nbt_compound_init(arena, out, 4)
            || !mcme_nbt_set_byte(jt, (int8_t)slot)
            || !mcme_nbt_set_byte(jc, (int8_t)count)
            || !mcme_nbt_compound_put(arena, out, "Slot", 4, jt)
            || !mcme_nbt_compound_put(arena, out, "id", 2,
                                      (McmeTag *)id)
            || !mcme_nbt_compound_put(arena, out, "Count", 5, jc)) {
        return NULL;   /* *dropped stays false: the caller fails */
    }

    int32_t damage = 0;
    if ((int_at(src, "Damage", &damage)
            || int_at(src, "damage", &damage)) && damage != 0) {
        McmeTag *tag = mcme_nbt_new(arena, MCME_TAG_Compound);
        McmeTag *dmg = mcme_nbt_new(arena, MCME_TAG_Int);
        if (tag == NULL || dmg == NULL
                || !mcme_nbt_compound_init(arena, tag, 1)
                || !mcme_nbt_set_int(dmg, damage)
                || !mcme_nbt_compound_put(arena, tag, "Damage", 6, dmg)
                || !mcme_nbt_compound_put(arena, out, "tag", 3, tag)) {
            return NULL;
        }
    }
    return out;
}

/* The Java block_entities entry for one Bedrock document, or the
   verdict that it does not become one. */
static BeVerdict carry_block_entity(McmeArena *arena,
                                    const McmeTag *src,
                                    McmeConvertReport *rep,
                                    McmeTag **out)
{
    *out = NULL;
    if (src == NULL || src->type != MCME_TAG_Compound) return BE_DROP;

    /* Coordinates first, the way the per-position reader reads them:
       a document no block address is not a block entity a Java list
       can hold, whatever its id says. */
    const McmeTag *id = mcme_nbt_get(src, "id");
    int32_t x = 0, y = 0, z = 0;
    if (id == NULL || id->type != MCME_TAG_String
            || !int_at(src, "x", &x) || !int_at(src, "y", &y)
            || !int_at(src, "z", &z)) {
        return BE_DROP;
    }
    const char *java_id =
        carried_java_id(id->string.data, id->string.len);
    if (java_id == NULL) return BE_DROP;

    /* A container the world has not filled yet. The loot table IS
       the items — mcme has no map from Bedrock's loot paths to
       Java's — so carrying the document would deliver an empty
       container where a full one belonged. The document drops and
       the count says so. */
    if (mcme_nbt_get(src, "LootTable") != NULL) return BE_DROP;

    McmeTag *jid = mcme_nbt_new(arena, MCME_TAG_String);
    McmeTag *j = mcme_nbt_new(arena, MCME_TAG_Compound);
    McmeTag *jx = mcme_nbt_new(arena, MCME_TAG_Int);
    McmeTag *jy = mcme_nbt_new(arena, MCME_TAG_Int);
    McmeTag *jz = mcme_nbt_new(arena, MCME_TAG_Int);
    if (jid == NULL || j == NULL || jx == NULL || jy == NULL
            || jz == NULL
            || !mcme_nbt_compound_init(arena, j, 6)
            || !mcme_nbt_set_string(arena, jid, java_id,
                                    strlen(java_id))
            || !mcme_nbt_set_int(jx, x) || !mcme_nbt_set_int(jy, y)
            || !mcme_nbt_set_int(jz, z)
            || !mcme_nbt_compound_put(arena, j, "id", 2, jid)
            || !mcme_nbt_compound_put(arena, j, "x", 1, jx)
            || !mcme_nbt_compound_put(arena, j, "y", 1, jy)
            || !mcme_nbt_compound_put(arena, j, "z", 1, jz)) {
        return BE_FAIL;
    }

    const McmeTag *items = mcme_nbt_get(src, "Items");
    if (items != NULL && items->type == MCME_TAG_List) {
        McmeTag **stacks = NULL;
        for (size_t i = 0; i < items->list.len; ++i) {
            bool dropped = false;
            McmeTag *s = carry_stack(arena, &items->list.items[i],
                                     &dropped);
            if (s == NULL && !dropped) {
                arrfree(stacks);
                return BE_FAIL;
            }
            if (s != NULL) arrpush(stacks, s);
            else rep->items++;
        }
        McmeTag *jl = mcme_nbt_new(arena, MCME_TAG_List);
        bool jl_ok = jl != NULL
            && mcme_nbt_list_alloc(arena, jl, MCME_TAG_Compound,
                                   (size_t)arrlen(stacks));
        for (size_t i = 0; jl_ok && i < (size_t)arrlen(stacks); ++i) {
            jl->list.items[i] = *stacks[i];
        }
        arrfree(stacks);
        if (!jl_ok
                || !mcme_nbt_compound_put(arena, j, "Items", 5, jl)) {
            return BE_FAIL;
        }
    }

    const McmeTag *cn = mcme_nbt_get(src, "CustomName");
    if (cn != NULL && cn->type == MCME_TAG_String
            && cn->string.len > 0
            && !mcme_nbt_compound_put(arena, j, "CustomName", 10,
                                      (McmeTag *)cn)) {
        return BE_FAIL;
    }
    const McmeTag *lock = mcme_nbt_get(src, "Lock");
    if (lock != NULL && lock->type == MCME_TAG_String
            && !mcme_nbt_compound_put(arena, j, "Lock", 4,
                                      (McmeTag *)lock)) {
        return BE_FAIL;
    }

    /* The smelter timers, under Java's names as Java's ints. */
    static const char * const kTimers[] = {
        "BurnTime", "CookTime", "CookTimeTotal"
    };
    for (size_t i = 0; i < sizeof kTimers / sizeof kTimers[0]; ++i) {
        int32_t v = 0;
        if (int_at(src, kTimers[i], &v)
                && !put_int(arena, j, kTimers[i], v)) {
            return BE_FAIL;
        }
    }

    *out = j;
    return BE_CARRY;
}

/* ========================================================================
 * One chunk: every subchunk, one region-file write
 * ======================================================================== */

/* Is this tag one of the four integer types the tick fields spell?
   long_or's fallback is not a type check — a compound field would
   read back as 0 and carry a lie. */
static bool tick_int(const McmeTag *t)
{
    if (t == NULL) return false;
    switch (t->type) {
    case MCME_TAG_Byte: case MCME_TAG_Short:
    case MCME_TAG_Int:  case MCME_TAG_Long:
        return true;
    default:
        return false;
    }
}

/* One tick record's entries, carried into the Java lists. The
   blockState is shaped exactly like a subchunk palette entry — name,
   states, version — so the same map answers it (D17); Java spells the
   two lists apart by content: water and lava under `fluid_ticks`,
   every other block under `block_ticks`. An entry either carries
   whole or counts as dropped — no half-translated entry reaches a
   Java list (D12, D18). */
static bool carry_tick_record(McmeLevelDb *db, const TickRef *ref,
                              McmeArena *arena,
                              McmeTag ***fluid, McmeTag ***block,
                              McmeConvertReport *rep,
                              McmeWriteRefusal *refusal)
{
    uint8_t key[9];
    size_t klen = 0;
    if (!mcme_bedrock_key_build(key, sizeof key, &klen,
                                ref->cx, ref->cz, 0, ref->tag,
                                false, 0)) {
        mcme_set_error("the tick record key at (%d, %d) does not fit",
                       ref->cx, ref->cz);
        refuse(refusal, MCME_WRITE_REFUSAL_INTERNAL);
        return false;
    }
    McmeBuffer val;
    mcme_buffer_init(&val);
    bool deleted = false;
    if (!mcme_leveldb_get(db, key, klen, &val, &deleted)) {
        mcme_buffer_free(&val);
        mcme_set_error("the tick record at (%d, %d) is gone from "
                       "the store", ref->cx, ref->cz);
        refuse(refusal, MCME_WRITE_REFUSAL_STORE);
        return false;
    }
    if (deleted) {
        mcme_buffer_free(&val);
        return true;           /* a tombstone holds no entries */
    }
    McmeNbtDoc doc;
    if (!mcme_nbt_parse(&doc, arena, val.data, val.len, true)) {
        mcme_buffer_free(&val);
        mcme_clear_error();
        rep->ticks += 1;       /* the record counts once */
        return true;
    }
    mcme_buffer_free(&val);

    const McmeTag *ct = NULL;
    const McmeTag *list = NULL;
    if (doc.root == NULL || doc.root->type != MCME_TAG_Compound
            || (ct = mcme_nbt_get(doc.root, "currentTick")) == NULL
            || !tick_int(ct)
            || (list = mcme_nbt_get(doc.root, "tickList")) == NULL
            || list->type != MCME_TAG_List) {
        rep->ticks += 1;       /* not a record mcme can read: once */
        return true;
    }
    int64_t now = mcme_nbt_long_or(ct, 0);

    for (size_t i = 0; i < list->list.len; ++i) {
        const McmeTag *e = &list->list.items[i];
        const McmeTag *bs = NULL, *tm = NULL, *xt = NULL, *yt = NULL;
        const McmeTag *zt = NULL;
        if (e->type != MCME_TAG_Compound
                || (bs = mcme_nbt_get(e, "blockState")) == NULL
                || bs->type != MCME_TAG_Compound
                || (tm = mcme_nbt_get(e, "time")) == NULL
                || (xt = mcme_nbt_get(e, "x")) == NULL
                || (yt = mcme_nbt_get(e, "y")) == NULL
                || (zt = mcme_nbt_get(e, "z")) == NULL
                || !tick_int(tm) || !tick_int(xt)
                || !tick_int(yt) || !tick_int(zt)) {
            rep->ticks++;      /* an entry without the fields: dropped */
            continue;
        }
        /* Bounded before the subtraction: two full-range int64s
           would overflow it, and a time nowhere near the clock is
           not a tick Java can spell as its Int. */
        int64_t time_val = mcme_nbt_long_or(tm, 0);
        if (time_val < INT64_MIN / 2 || time_val > INT64_MAX / 2
                || now < INT64_MIN / 2 || now > INT64_MAX / 2) {
            rep->ticks++;
            continue;
        }
        int64_t t = time_val - now;
        if (t < INT32_MIN || t > INT32_MAX) {
            rep->ticks++;
            continue;
        }
        McmeTag *jn = mcme_translate_block(arena, bs);
        const McmeTag *nm = jn != NULL ? mcme_nbt_get(jn, "Name") : NULL;
        if (jn == NULL || nm == NULL || nm->type != MCME_TAG_String) {
            mcme_clear_error();
            rep->ticks++;      /* legacy or nameless: dropped, named */
            continue;
        }
        McmeTag *ent = mcme_nbt_new(arena, MCME_TAG_Compound);
        if (ent == NULL || !mcme_nbt_compound_init(arena, ent, 6)
                || !put_stringn(arena, ent, "i",
                                nm->string.data, nm->string.len)
                || !put_int(arena, ent, "p", 0)
                || !put_int(arena, ent, "t", (int32_t)t)
                || !put_int(arena, ent, "x", mcme_nbt_int_or(xt, 0))
                || !put_int(arena, ent, "y", mcme_nbt_int_or(yt, 0))
                || !put_int(arena, ent, "z", mcme_nbt_int_or(zt, 0))) {
            refuse(refusal, MCME_WRITE_REFUSAL_INTERNAL);
            return false;
        }
        bool fluid_name = (nm->string.len == sizeof "minecraft:water" - 1
                           && memcmp(nm->string.data, "minecraft:water",
                                     nm->string.len) == 0)
            || (nm->string.len == sizeof "minecraft:lava" - 1
                && memcmp(nm->string.data, "minecraft:lava",
                          nm->string.len) == 0);
        if (fluid_name) {
            arrpush(*fluid, ent);
        } else {
            arrpush(*block, ent);
        }
        rep->ticks_carried++;
    }
    return true;
}

static bool convert_chunk(McmeLevelDb *db, const char *out,
                          const SubRef *subs, size_t n,
                          const BeRef *bes, size_t nb,
                          const TickRef *ticks, size_t ntk,
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
    McmeTag **carried = NULL;    /* the block_entities entries */
    McmeTag **fluid = NULL;      /* the fluid_ticks entries    */
    McmeTag **block = NULL;      /* the block_ticks entries    */
    int32_t cx = subs[0].cx;
    int32_t cz = subs[0].cz;
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
        McmeTag *sec = NULL;
        if (!make_section(arena, subs[i].cy, &sc.layers[0],
                          sc.layer_count > 1 ? &sc.layers[1] : NULL,
                          rep, &sec)) {
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

    /* The chunk's block-entity record: every document either carried
       with its fields mapped, or counted (D15). The whole record must
       decode — a record that will not is counted once, never
       half-read. */
    if (nb > 0) {
        McmeNbtDoc *bdocs = NULL;
        size_t ndocs = 0;
        McmeEntityStatus st = mcme_bedrock_chunk_block_entities(
            db, cx, cz, 0, arena, &bdocs, &ndocs);
        if (st == MCME_ENTITY_NOT_READABLE) {
            mcme_clear_error();
            rep->block_entities++;    /* the record counts once */
        } else if (st == MCME_ENTITY_STORE) {
            refuse(refusal, MCME_WRITE_REFUSAL_STORE);
            arrfree(built);
            arrfree(carried);
            arrfree(fluid);
            arrfree(block);
            mcme_arena_free(arena);
            return false;
        } else if (st != MCME_ENTITY_OK && st != MCME_ENTITY_ABSENT) {
            refuse(refusal, MCME_WRITE_REFUSAL_INTERNAL);
            arrfree(built);
            arrfree(carried);
            arrfree(fluid);
            arrfree(block);
            mcme_arena_free(arena);
            return false;
        } else {
            for (size_t i = 0; i < ndocs; ++i) {
                McmeTag *j = NULL;
                BeVerdict v = carry_block_entity(arena, bdocs[i].root,
                                                 rep, &j);
                if (v == BE_FAIL) {
                    mcme_set_error("out of memory carrying the block "
                                   "entity at chunk (%d, %d)", cx, cz);
                    refuse(refusal, MCME_WRITE_REFUSAL_INTERNAL);
                    arrfree(built);
                    arrfree(carried);
                    arrfree(fluid);
                    arrfree(block);
                    mcme_arena_free(arena);
                    return false;
                }
                if (v == BE_CARRY) {
                    arrpush(carried, j);
                    rep->block_entities_carried++;
                } else {
                    rep->block_entities++;
                }
            }
        }
    }

    /* The chunk's tick records: every entry through the same map the
       palettes use, split the way Java spells its two lists. Both
       lists stand in every chunk, empty when the records hold none —
       the shape the reference writes (D18). */
    for (size_t i = 0; i < ntk; ++i) {
        if (!carry_tick_record(db, &ticks[i], arena, &fluid, &block,
                               rep, refusal)) {
            arrfree(built);
            arrfree(carried);
            arrfree(fluid);
            arrfree(block);
            mcme_arena_free(arena);
            return false;
        }
    }

    bool ok = put_int(arena, root, "DataVersion", kJavaDataVersion)
        && put_string(arena, root, "Status", "minecraft:full")
        && put_int(arena, root, "xPos", cx)
        && put_int(arena, root, "zPos", cz)
        && put_int(arena, root, "yPos", -4)
        && put_long(arena, root, "LastUpdate", 0)
        && put_long(arena, root, "InhabitedTime", 0)
        && mcme_nbt_compound_put(arena, root, "sections", 8, sections);
    /* The list itself: the documents the map carried, in the
       record's own order. An empty list is what the reference writes
       when a chunk holds none. */
    McmeTag *entities = mcme_nbt_new(arena, MCME_TAG_List);
    size_t nbe = (size_t)arrlen(carried);
    bool bel_ok = entities != NULL
        && (nbe > 0
            ? mcme_nbt_list_alloc(arena, entities, MCME_TAG_Compound,
                                  nbe)
            : mcme_nbt_list_alloc(arena, entities, MCME_TAG_Byte, 0));
    for (size_t i = 0; bel_ok && i < nbe; ++i) {
        entities->list.items[i] = *carried[i];
    }
    ok = ok && bel_ok
        && mcme_nbt_compound_put(arena, root, "block_entities", 14,
                                 entities);
    /* fluid_ticks and block_ticks: both keys in every chunk, empty
       when the records carried nothing — never half a list. Java
       reads a missing list and an empty one the same, but the shape
       a chunk is written in is a fact, not a knob. */
    McmeTag *fluid_list = mcme_nbt_new(arena, MCME_TAG_List);
    McmeTag *block_list = mcme_nbt_new(arena, MCME_TAG_List);
    size_t nfl = (size_t)arrlen(fluid);
    size_t nbl = (size_t)arrlen(block);
    bool ft_ok = fluid_list != NULL
        && (nfl > 0
            ? mcme_nbt_list_alloc(arena, fluid_list, MCME_TAG_Compound,
                                  nfl)
            : mcme_nbt_list_alloc(arena, fluid_list, MCME_TAG_Byte, 0));
    bool bt_ok = block_list != NULL
        && (nbl > 0
            ? mcme_nbt_list_alloc(arena, block_list, MCME_TAG_Compound,
                                  nbl)
            : mcme_nbt_list_alloc(arena, block_list, MCME_TAG_Byte, 0));
    for (size_t i = 0; ft_ok && i < nfl; ++i) {
        fluid_list->list.items[i] = *fluid[i];
    }
    for (size_t i = 0; bt_ok && i < nbl; ++i) {
        block_list->list.items[i] = *block[i];
    }
    ok = ok && ft_ok && bt_ok
        && mcme_nbt_compound_put(arena, root, "fluid_ticks", 11,
                                 fluid_list)
        && mcme_nbt_compound_put(arena, root, "block_ticks", 11,
                                 block_list);
    if (!ok) {
        mcme_set_error("out of memory building the chunk at "
                       "(%d, %d)", cx, cz);
        refuse(refusal, MCME_WRITE_REFUSAL_INTERNAL);
        arrfree(built);
        arrfree(carried);
        arrfree(fluid);
        arrfree(block);
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
        arrfree(carried);
        arrfree(fluid);
        arrfree(block);
        mcme_arena_free(arena);
        return false;
    }
    bool written = mcme_region_set_chunk(file, cx, cz, root, arena, 0,
                                         refusal);
    if (written) rep->chunks++;
    arrfree(built);
    arrfree(carried);
    arrfree(fluid);
    arrfree(block);
    mcme_arena_free(arena);
    return written;
}

/* Records whose chunk holds no subchunks: no Java chunk is written
   for that chunk, so every document counts as dropped. The read is
   the same whole-record read the carried path uses — the count is
   documents, and an unreadable record counts once. */
static bool count_unconverted_records(McmeLevelDb *db,
                                      const BeRef *bes, size_t n,
                                      McmeArena *arena,
                                      McmeConvertReport *rep,
                                      McmeWriteRefusal *refusal)
{
    for (size_t i = 0; i < n; ++i) {
        McmeNbtDoc *docs = NULL;
        size_t nd = 0;
        McmeEntityStatus st = mcme_bedrock_chunk_block_entities(
            db, bes[i].cx, bes[i].cz, 0, arena, &docs, &nd);
        if (st == MCME_ENTITY_OK) {
            rep->block_entities += nd;
        } else if (st == MCME_ENTITY_NOT_READABLE) {
            mcme_clear_error();
            rep->block_entities += 1;
        } else if (st != MCME_ENTITY_ABSENT) {
            /* A store that fails mid-walk is not a count mcme can
               finish — the run fails, the output is removed. */
            refuse(refusal, st == MCME_ENTITY_STORE
                   ? MCME_WRITE_REFUSAL_STORE
                   : MCME_WRITE_REFUSAL_INTERNAL);
            return false;
        }
    }
    return true;
}

/* Tick records whose chunk holds no subchunks: no Java chunk is
   written for that chunk, so every entry counts as dropped. The read
   is the same whole-record read the carried path uses — the count is
   entries, and an unreadable record counts once (D18). */
static bool count_unconverted_ticks(McmeLevelDb *db,
                                    const TickRef *refs, size_t n,
                                    McmeArena *arena,
                                    McmeConvertReport *rep,
                                    McmeWriteRefusal *refusal)
{
    for (size_t i = 0; i < n; ++i) {
        uint8_t key[9];
        size_t klen = 0;
        if (!mcme_bedrock_key_build(key, sizeof key, &klen,
                                    refs[i].cx, refs[i].cz, 0,
                                    refs[i].tag, false, 0)) {
            mcme_set_error("the tick record key at (%d, %d) does not fit",
                           refs[i].cx, refs[i].cz);
            refuse(refusal, MCME_WRITE_REFUSAL_INTERNAL);
            return false;
        }
        McmeBuffer val;
        mcme_buffer_init(&val);
        bool deleted = false;
        if (!mcme_leveldb_get(db, key, klen, &val, &deleted)) {
            mcme_buffer_free(&val);
            mcme_set_error("the tick record at (%d, %d) is gone from "
                           "the store", refs[i].cx, refs[i].cz);
            refuse(refusal, MCME_WRITE_REFUSAL_STORE);
            return false;
        }
        if (deleted) {
            mcme_buffer_free(&val);
            continue;          /* a tombstone holds no entries */
        }
        McmeNbtDoc doc;
        if (!mcme_nbt_parse(&doc, arena, val.data, val.len, true)) {
            mcme_buffer_free(&val);
            mcme_clear_error();
            rep->ticks += 1;   /* the record counts once */
            continue;
        }
        mcme_buffer_free(&val);
        const McmeTag *list = (doc.root != NULL
                               && doc.root->type == MCME_TAG_Compound)
            ? mcme_nbt_get(doc.root, "tickList") : NULL;
        if (list == NULL || list->type != MCME_TAG_List) {
            rep->ticks += 1;   /* not a record mcme can count: once */
            continue;
        }
        rep->ticks += list->list.len;
    }
    return true;
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
    scan.bes = NULL;
    scan.ticks = NULL;
    scan.rep = report;
    mcme_leveldb_iterate(db, scan_key, &scan);
    size_t nsubs = (size_t)arrlen(scan.subs);
    size_t nbes = (size_t)arrlen(scan.bes);
    size_t ntk = (size_t)arrlen(scan.ticks);
    if (nsubs > 1) qsort(scan.subs, nsubs, sizeof (SubRef), sub_cmp);
    if (nbes > 1) qsort(scan.bes, nbes, sizeof (BeRef), be_cmp);
    if (ntk > 1) qsort(scan.ticks, ntk, sizeof (TickRef), tick_cmp);

    /* Every list is ordered by chunk, so one walk pairs records with
       the chunks that convert. A record whose chunk never appears has
       no Java chunk to live in; its documents and entries are
       counted, not carried — a drop the report names like any
       other. */
    bool ok = true;
    McmeArena *rec_arena = mcme_arena_new();
    if (rec_arena == NULL) {
        mcme_set_error("out of memory");
        refuse(refusal, MCME_WRITE_REFUSAL_INTERNAL);
        ok = false;
    }
    size_t i = 0, bi = 0, ki = 0;
    while (ok && i < nsubs) {
        size_t j = i;
        while (j < nsubs && scan.subs[j].cx == scan.subs[i].cx
               && scan.subs[j].cz == scan.subs[i].cz) {
            ++j;
        }
        int32_t cx = scan.subs[i].cx;
        int32_t cz = scan.subs[i].cz;
        while (ok && bi < nbes
               && (scan.bes[bi].cx < cx
                   || (scan.bes[bi].cx == cx && scan.bes[bi].cz < cz))) {
            ok = count_unconverted_records(db, scan.bes + bi, 1,
                                           rec_arena, report, refusal);
            ++bi;
        }
        size_t bj = bi;
        while (bj < nbes && scan.bes[bj].cx == cx
               && scan.bes[bj].cz == cz) {
            ++bj;
        }
        while (ok && ki < ntk
               && (scan.ticks[ki].cx < cx
                   || (scan.ticks[ki].cx == cx
                       && scan.ticks[ki].cz < cz))) {
            ok = count_unconverted_ticks(db, scan.ticks + ki, 1,
                                         rec_arena, report, refusal);
            ++ki;
        }
        size_t kj = ki;
        while (kj < ntk && scan.ticks[kj].cx == cx
               && scan.ticks[kj].cz == cz) {
            ++kj;
        }
        if (ok) {
            ok = convert_chunk(db, out, scan.subs + i, j - i,
                               scan.bes + bi, bj - bi,
                               scan.ticks + ki, kj - ki, report,
                               refusal);
        }
        bi = bj;
        ki = kj;
        i = j;
    }
    while (ok && bi < nbes) {
        ok = count_unconverted_records(db, scan.bes + bi, 1,
                                       rec_arena, report, refusal);
        ++bi;
    }
    while (ok && ki < ntk) {
        ok = count_unconverted_ticks(db, scan.ticks + ki, 1,
                                     rec_arena, report, refusal);
        ++ki;
    }
    mcme_arena_free(rec_arena);

    arrfree(scan.subs);
    arrfree(scan.bes);
    arrfree(scan.ticks);
    mcme_leveldb_close(db);
    mcme_world_info_free(&info);
    if (!ok) rm_rf(out);
    return ok;
}
