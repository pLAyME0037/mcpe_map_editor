/*
 * The convert pipeline: a Bedrock store walked in full, every
 * subchunk's layer zero through the translate map, a Java 1.20
 * world written out — the game's three dimensions, each in the
 * directory Java reads it from (region/, DIM-1/region/,
 * DIM1/region/), sections in Y order with a plains biome, one
 * region file per 32x32 of chunks, a level.dat carrying the seed
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

/* The overworld's floor is y -64: subchunks run from -4 up, and the
   Data3D record's biome arrays count from there. The record carries
   no floor of its own — the dimension gives it one (D21). */
static const int8_t kBedrockFloorCy = -4;   /* the overworld        */

/* The floor a dimension's records and chunks stand on, and the
   height it spells, after the Java dimension types the game itself
   ships: the overworld runs y -64..319 (floor section -4, 384
   blocks), the nether and the end y 0..255 (floor section 0, 256).
   So their records count strata and columns from 0, their chunks
   write yPos 0, and a heightmap column answers inside [0, height]. */
static int8_t dim_floor_cy(int32_t dim)
{
    return dim == 0 ? kBedrockFloorCy : (int8_t)0;
}

static int32_t dim_height(int32_t dim)
{
    return dim == 0 ? 384 : 256;
}

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
    int32_t dim;          /* 0 overworld, 1 nether, 2 end (D21)     */
    int32_t cx, cz;
    int8_t  cy;
    uint8_t key[14];      /* the key exactly as stored, for the get */
    uint8_t key_len;
} SubRef;

typedef struct {
    int32_t dim;             /* the record's own dimension (D21)    */
    int32_t cx, cz;
} BeRef;                     /* a chunk's block-entity record       */

typedef struct {
    int32_t dim;
    int32_t cx, cz;
    uint8_t tag;             /* 0x33 or 0x3a: which record this is  */
} TickRef;                   /* a chunk's tick record               */

typedef struct {
    int32_t dim;
    int32_t cx, cz;
    uint8_t key[14];      /* the key exactly as stored, for the get */
    uint8_t key_len;
} BiomeRef;                 /* a chunk's Data3D record               */

typedef struct {
    int32_t dim;             /* the record's own dimension (D21)    */
    int32_t cx, cz;
} EntRef;                     /* a chunk's entity-list record (0x32) */

typedef struct {
    SubRef            *subs;   /* stb array */
    BeRef             *bes;    /* stb array */
    EntRef            *ents;   /* stb array */
    TickRef           *ticks;  /* stb array */
    BiomeRef          *biomes; /* stb array */
    bool               has_subs[3];
                               /* the store holds chunks in this
                                  dimension (D21)                   */
    McmeConvertReport *rep;
} Scan;

static bool scan_key(void *ud, const McmeLevelEntry *e)
{
    Scan *s = (Scan *)ud;
    McmeBedrockKey k;
    if (!mcme_bedrock_key_parse(e->key, e->key_len, &k)) {
        /* Level metadata, portallists, anything else a store keeps.
           The two entity keys are named since D23: an actor record
           is a document the store holds and counts with them; the
           digp index counts nowhere, its documents being the actor
           records themselves — counting both would count one entity
           twice. Anything else is named in the report as `other`;
           nothing else to do. */
        if (mcme_bedrock_actor_key(e->key, e->key_len)) {
            s->rep->actors++;
            return true;
        }
        if (mcme_bedrock_digp_key(e->key, e->key_len)) {
            return true;
        }
        s->rep->other++;
        return true;
    }
    int32_t dim = k.dimension;
    if (dim != 0 && dim != 1 && dim != 2) {
        /* The game's three dimensions carry every chunk key the
           same way now (D21), each landing in the directory Java
           reads it from. A number beyond them lands nowhere:
           counted `other`, like any key with nowhere to go. */
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
        b.dim = dim;
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
        t.dim = dim;
        t.cx = k.x;
        t.cz = k.z;
        t.tag = k.tag;
        arrpush(s->ticks, t);
        return true;
    }
    if (k.tag == MCME_BEDROCK_TAG_BIOME_3D) {
        /* Collected the same way (D19): the record whose chunk
           converts carries its biome cells, the record whose chunk
           holds no subchunks counts them after the chunk loop. The
           whole key is kept for the get — the record has no reader
           keyed by coordinates the way entities have, and rebuilding
           the key would drop a second spelling store's might hold. */
        BiomeRef b;
        b.dim = dim;
        b.cx = k.x;
        b.cz = k.z;
        memcpy(b.key, e->key, e->key_len);
        b.key_len = (uint8_t)e->key_len;
        arrpush(s->biomes, b);
        return true;
    }
    if (k.tag == MCME_BEDROCK_TAG_ENTITIES) {
        /* Collected, not counted here — but with no pairing to
           respect, unlike the block-entity records: no entity
           document crosses into Java (the reference answers
           nothing for entities, D23), so every record's documents
           count once after the chunk loop, whatever its own chunk
           does. The key is rebuilt for the get the block-entity
           records rebuild theirs with — measured one spelling on
           the real store (D23). */
        EntRef r;
        r.dim = dim;
        r.cx = k.x;
        r.cz = k.z;
        arrpush(s->ents, r);
        return true;
    }
    bool is_sub = k.tag == MCME_BEDROCK_TAG_SUBCHUNK
        || k.tag == MCME_BEDROCK_TAG_SUBCHUNK_OLD;
    if (!is_sub) {
        /* Chunk versions, the older 0x2d biome spelling, markers. */
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
    r.dim = dim;
    r.cx = k.x;
    r.cz = k.z;
    r.cy = (int8_t)e->key[e->key_len - 1];
    memcpy(r.key, e->key, e->key_len);
    r.key_len = (uint8_t)e->key_len;
    arrpush(s->subs, r);
    s->has_subs[dim] = true;
    return true;
}

static int sub_cmp(const void *a, const void *b)
{
    const SubRef *x = (const SubRef *)a;
    const SubRef *y = (const SubRef *)b;
    if (x->dim != y->dim) return (x->dim < y->dim) ? -1 : 1;
    if (x->cx != y->cx) return (x->cx < y->cx) ? -1 : 1;
    if (x->cz != y->cz) return (x->cz < y->cz) ? -1 : 1;
    if (x->cy != y->cy) return (x->cy < y->cy) ? -1 : 1;
    return 0;
}

static int be_cmp(const void *a, const void *b)
{
    const BeRef *x = (const BeRef *)a;
    const BeRef *y = (const BeRef *)b;
    if (x->dim != y->dim) return (x->dim < y->dim) ? -1 : 1;
    if (x->cx != y->cx) return (x->cx < y->cx) ? -1 : 1;
    if (x->cz != y->cz) return (x->cz < y->cz) ? -1 : 1;
    return 0;
}

static int ent_cmp(const void *a, const void *b)
{
    const EntRef *x = (const EntRef *)a;
    const EntRef *y = (const EntRef *)b;
    if (x->dim != y->dim) return (x->dim < y->dim) ? -1 : 1;
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
    if (x->dim != y->dim) return (x->dim < y->dim) ? -1 : 1;
    if (x->cx != y->cx) return (x->cx < y->cx) ? -1 : 1;
    if (x->cz != y->cz) return (x->cz < y->cz) ? -1 : 1;
    if (x->tag != y->tag) return (x->tag < y->tag) ? -1 : 1;
    return 0;
}

static int biome_cmp(const void *a, const void *b)
{
    const BiomeRef *x = (const BiomeRef *)a;
    const BiomeRef *y = (const BiomeRef *)b;
    if (x->dim != y->dim) return (x->dim < y->dim) ? -1 : 1;
    if (x->cx != y->cx) return (x->cx < y->cx) ? -1 : 1;
    if (x->cz != y->cz) return (x->cz < y->cz) ? -1 : 1;
    return 0;
}

/* Is a record's chunk before the (dim, cx, cz) the walk stands on?
   Every list is sorted this way, so the walk pairs each record with
   its own dimension's chunks and nothing else (D21). */
static bool pos_before(int32_t rdim, int32_t rcx, int32_t rcz,
                       int32_t dim, int32_t cx, int32_t cz)
{
    if (rdim != dim) return rdim < dim;
    if (rcx != cx) return rcx < cx;
    return rcz < cz;
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
/* One section's `biomes` container, from the chunk record's own 64
   cells (already in Java's container order, x + z*4 + y*16).

   Every cell goes through the biome map; a cell the map does not name
   becomes the reference's own fallback — plains — and counts, so the
   report says which cells arrived as their fallback rather than their
   biome (D19). The palette is the distinct names sorted by name: the
   container's internal order is mcme's own (ref_convert pins cell
   content, not byte order), and a sorted palette is the one order
   nothing downstream can argue with. One name is the format's
   shorthand — no data array; more pack the way the block indices do,
   floor(64 / bits) values to a long, none spanning two. */
/* Two names, bytewise: the lengths belong to the comparison too,
   though every name here is a literal that ends where it says. */
static bool str_eq(const McmeStr *a, const McmeStr *b)
{
    return a->len == b->len && memcmp(a->data, b->data, a->len) == 0;
}

/* One cell's name for the palette sort, with the position it came
   from so the sort hands back the mapping, not just the order. */
struct BiomeCellName {
    const McmeStr *name;
    uint8_t        cell;
};

static int biome_cell_cmp(const void *a, const void *b)
{
    const struct BiomeCellName *x = (const struct BiomeCellName *)a;
    const struct BiomeCellName *y = (const struct BiomeCellName *)b;
    size_t n = x->name->len < y->name->len ? x->name->len
                                           : y->name->len;
    int c = n == 0 ? 0 : memcmp(x->name->data, y->name->data, n);
    if (c != 0) return c;
    if (x->name->len != y->name->len) {
        return x->name->len < y->name->len ? -1 : 1;
    }
    return 0;
}

static bool make_biomes(McmeArena *arena, const uint32_t *cells,
                        McmeConvertReport *rep, McmeTag *sec)
{
    McmeStr names[MCME_BEDROCK_BIOME_CELLS];
    for (size_t i = 0; i < MCME_BEDROCK_BIOME_CELLS; ++i) {
        if (mcme_translate_biome((int32_t)cells[i], &names[i])) {
            rep->biomes_carried++;
        } else {
            names[i].data = (char *)kPlains;
            names[i].len = sizeof kPlains - 1;
            rep->biomes++;
        }
    }

    /* The palette and the mapping, from one sort of the cells
       themselves: a run of equal names is one palette entry, and
       every cell in the run takes that entry's index. The sort
       carries names by pointer — they are literals, none is going
       anywhere — so what moves is a name's address and a byte. */
    struct BiomeCellName order[MCME_BEDROCK_BIOME_CELLS];
    for (size_t i = 0; i < MCME_BEDROCK_BIOME_CELLS; ++i) {
        order[i].name = &names[i];
        order[i].cell = (uint8_t)i;
    }
    qsort(order, MCME_BEDROCK_BIOME_CELLS, sizeof order[0],
          biome_cell_cmp);

    McmeStr pal[MCME_BEDROCK_BIOME_CELLS];
    uint8_t slot[MCME_BEDROCK_BIOME_CELLS];
    size_t np = 0;
    for (size_t i = 0; i < MCME_BEDROCK_BIOME_CELLS; ) {
        size_t j = i + 1;
        while (j < MCME_BEDROCK_BIOME_CELLS
               && str_eq(order[j].name, order[i].name)) {
            ++j;
        }
        for (size_t k = i; k < j; ++k) {
            slot[order[k].cell] = (uint8_t)np;
        }
        pal[np] = *order[i].name;
        ++np;
        i = j;
    }

    McmeTag *bpal = mcme_nbt_new(arena, MCME_TAG_List);
    if (bpal == NULL
            || !mcme_nbt_list_alloc(arena, bpal, MCME_TAG_String, np)) {
        return false;
    }
    for (size_t k = 0; k < np; ++k) {
        McmeTag *s = mcme_nbt_new(arena, MCME_TAG_String);
        if (s == NULL
                || !mcme_nbt_set_string(arena, s, pal[k].data,
                                        pal[k].len)) {
            return false;
        }
        bpal->list.items[k] = *s;
    }

    McmeTag *biomes = mcme_nbt_new(arena, MCME_TAG_Compound);
    if (biomes == NULL
            || !mcme_nbt_compound_init(arena, biomes,
                                       np > 1 ? 2 : 1)
            || !mcme_nbt_compound_put(arena, biomes, "palette", 7,
                                      bpal)) {
        return false;
    }

    if (np > 1) {
        int bits = 1;
        while (((size_t)1 << bits) < np) ++bits;
        size_t per = MCME_BEDROCK_BIOME_CELLS / (size_t)bits;
        size_t nlongs = (MCME_BEDROCK_BIOME_CELLS + per - 1) / per;
        int64_t *longs = mcme_arena_alloc(arena,
                                          nlongs * sizeof *longs);
        if (longs == NULL) return false;
        memset(longs, 0, nlongs * sizeof *longs);
        for (size_t p = 0; p < MCME_BEDROCK_BIOME_CELLS; ++p) {
            size_t word = p / per;
            size_t bit = (p % per) * (size_t)bits;
            longs[word] |= (int64_t)((uint64_t)slot[p] << bit);
        }
        McmeTag *data = mcme_nbt_new(arena, MCME_TAG_Long_Array);
        if (data == NULL
                || !mcme_nbt_set_longs(arena, data, longs, nlongs)
                || !mcme_nbt_compound_put(arena, biomes, "data", 4,
                                          data)) {
            return false;
        }
    }

    return mcme_nbt_compound_put(arena, sec, "biomes", 6, biomes);
}

static bool make_section(McmeArena *arena, int8_t cy,
                         const McmeBedrockLayer *layer,
                         const McmeBedrockLayer *extra,
                         const uint32_t *biome_cells,
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
            /* An entry the map refuses — nameless, or a `val` that
               is not a number: its positions are air, and the
               subchunk is counted once (D12). */
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

    /* Biomes: the section's own cells from the chunk's Data3D record.
       The record's absence at this Y leaves no key at all — the old
       plains placeholder went with the map it was waiting for: an
       absent container reads back as the default that placeholder
       spelled, and Bedrock held nothing here to say otherwise (D19). */
    if (biome_cells != NULL
            && !make_biomes(arena, biome_cells, rep, sec)) {
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
 * A document is carried when two things hold: its id is one of the
 * families the mapping knows — the word both editions spell the same
 * way ("Chest" is Java's "chest", "Sign" is "sign"; one id spells it
 * out: "EnchantTable" is "enchanting_table") so there is no name mcme
 * could invent — and its own x/y/z name a block, because a Java
 * chunk's block_entities list places entries by those coordinates.
 * Everything else drops and is counted (D12).
 *
 * The game writes its savegame ids short ("Chest"); the frozen golden
 * fixture and mcme's own writes spell them "minecraft:chest". Both
 * spellings carry; the answer is always Java's namespaced id.
 *
 * What travels inside a carried document: id, x, y, z, and beside
 * them what each family's Java spelling has — Items (below), a
 * nonempty CustomName, Lock, the smelter timers as Java's ints for
 * the containers; the fields the table below names each family for
 * the rest (D22). What does not: fields with no Java spelling to
 * carry to — keepPacked, the pair coordinates double chests record,
 * the sculk listener's selector, anything else the game adds. A
 * document the world has not filled yet — one with a LootTable —
 * drops entirely (see carry_block_entity). The report counts whole
 * documents and whole stacks, never individual fields.
 * ======================================================================== */

/* The families within the carry: what travels beside id/x/y/z.
   The containers and the smelter timers are D15's; the rest arrive
   with the fields each type's Java spelling has (D22), measured
   through the reference on a real world — the spawner's ranges
   under the same names, the sign's text split into Java's four
   messages, the brewing stand's progress under BrewTime. A type
   whose Java block entity holds nothing a Bedrock document spells
   still carries: the id and the position are the document (the
   sculk trio, the lectern, the skull). */
enum {
    BE_CONTAINER,   /* Items, CustomName, Lock, the smelter timers */
    BE_BREWING,     /* Items, CustomName, Lock, BrewTime and Fuel  */
    BE_SPAWNER,     /* the seven ranges, SpawnData from the id     */
    BE_SIGN,        /* front_text, back_text, is_waxed             */
    BE_COMPARATOR,  /* OutputSignal                                */
    BE_BEACON,      /* Primary, Secondary                          */
    BE_GATEWAY,     /* Age, ExitPortal                             */
    BE_CAMPFIRE,    /* CookingTimes                                */
    BE_JUKEBOX,     /* RecordItem                                  */
    BE_SHELF,       /* Items, last_interacted_slot                 */
    BE_EMPTY        /* nothing: id and position are all            */
};

/*
 * The documents the carry maps, under the game's own savegame ids —
 * "Chest", "BlastFurnace" — and Java's namespaced id beside each.
 *
 * The game writes the short form into its records (measured: every
 * one of them, tens of thousands of documents, in a real 2026
 * world); the frozen golden fixture and mcme's own writes spell the
 * id "minecraft:chest". Both spellings carry; the answer is always
 * Java's.
 */
static const struct {
    const char *game;    /* the savegame id, as the game spells it */
    const char *java;    /* Java's namespaced id, the output       */
    int kind;            /* what travels beside id and position    */
} kCarriedBlockEntities[] = {
    {"Chest",            "minecraft:chest",            BE_CONTAINER},
    {"TrappedChest",     "minecraft:trapped_chest",    BE_CONTAINER},
    {"EnderChest",       "minecraft:ender_chest",      BE_CONTAINER},
    {"Barrel",           "minecraft:barrel",           BE_CONTAINER},
    {"Hopper",           "minecraft:hopper",           BE_CONTAINER},
    {"Furnace",          "minecraft:furnace",          BE_CONTAINER},
    {"BlastFurnace",     "minecraft:blast_furnace",    BE_CONTAINER},
    {"Smoker",           "minecraft:smoker",           BE_CONTAINER},
    {"Dispenser",        "minecraft:dispenser",        BE_CONTAINER},
    {"Dropper",          "minecraft:dropper",          BE_CONTAINER},
    {"ShulkerBox",       "minecraft:shulker_box",      BE_CONTAINER},
    {"BrewingStand",     "minecraft:brewing_stand",    BE_BREWING},
    {"MobSpawner",       "minecraft:mob_spawner",      BE_SPAWNER},
    {"Sign",             "minecraft:sign",             BE_SIGN},
    {"HangingSign",      "minecraft:hanging_sign",     BE_SIGN},
    {"Comparator",       "minecraft:comparator",       BE_COMPARATOR},
    {"Beacon",           "minecraft:beacon",           BE_BEACON},
    {"EndGateway",       "minecraft:end_gateway",      BE_GATEWAY},
    {"Campfire",         "minecraft:campfire",         BE_CAMPFIRE},
    {"SculkSensor",      "minecraft:sculk_sensor",     BE_EMPTY},
    {"SculkCatalyst",    "minecraft:sculk_catalyst",   BE_EMPTY},
    {"SculkShrieker",    "minecraft:sculk_shrieker",   BE_EMPTY},
    {"Jukebox",          "minecraft:jukebox",          BE_JUKEBOX},
    {"ChiseledBookshelf","minecraft:chiseled_bookshelf",
                                                       BE_SHELF},
    {"Lectern",          "minecraft:lectern",          BE_EMPTY},
    {"Skull",            "minecraft:skull",            BE_EMPTY},
    {"Bell",             "minecraft:bell",             BE_EMPTY},
    {"EnchantTable",     "minecraft:enchanting_table", BE_EMPTY},
    {"DaylightDetector", "minecraft:daylight_detector",
                                                       BE_EMPTY},
    {"EndPortal",        "minecraft:end_portal",       BE_EMPTY},
};

/* Java's id for this document's, or NULL: the id names no family
   the mapping knows. The namespaced spelling is the short one with
   the prefix stripped, compared without case — "minecraft:chest" and
   "Chest" are the same document in two writers' hands. `kind` says
   what travels beside id and position. */
static const char *carried_java_id(const char *id, size_t len,
                                   int *kind)
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
        if (same) {
            *kind = kCarriedBlockEntities[i].kind;
            return kCarriedBlockEntities[i].java;
        }
    }
    return NULL;
}

/* The verdict for one document: carried into the list, dropped and
   counted, or an allocation failure — which fails the run, because a
   conversion that cannot finish its arithmetic is not a conversion
   that guesses. */
typedef enum { BE_CARRY, BE_DROP, BE_FAIL } BeVerdict;

/* The integer this tag spells, from the numeric types a store
   actually holds. False for a field that is absent or spelled
   another way — the caller then treats it as absent, never as
   zero. Long stays out: the tick fields are the only longs a
   document holds, and they ask for it by name. */
static bool tag_int(const McmeTag *t, int32_t *out)
{
    if (t == NULL) return false;
    switch (t->type) {
    case MCME_TAG_Byte:  *out = t->as_byte;  return true;
    case MCME_TAG_Short: *out = t->as_short; return true;
    case MCME_TAG_Int:   *out = t->as_int;   return true;
    default: return false;
    }
}

/* The integer at `key`, as tag_int sees it. */
static bool int_at(const McmeTag *root, const char *key, int32_t *out)
{
    return tag_int(mcme_nbt_get(root, key), out);
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
 * `with_slot` is how the stack sits in a list. A record in a
 * jukebox stands alone — no Slot key in Bedrock's own spelling and
 * none in Java's — so the caller passes false: no Slot is required
 * and none is written (D22).
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
                            bool *dropped, bool with_slot)
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
            || (with_slot && !int_at(src, "Slot", &slot))
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
    McmeTag *jt = with_slot ? mcme_nbt_new(arena, MCME_TAG_Byte)
                            : NULL;
    McmeTag *jc = mcme_nbt_new(arena, MCME_TAG_Byte);
    if (out == NULL || (with_slot && jt == NULL) || jc == NULL
            || !mcme_nbt_compound_init(arena, out,
                                       with_slot ? 4 : 3)
            || (with_slot
                && (!mcme_nbt_set_byte(jt, (int8_t)slot)
                    || !mcme_nbt_compound_put(arena, out, "Slot", 4,
                                              jt)))
            || !mcme_nbt_set_byte(jc, (int8_t)count)
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

/* The document's Items list under Java's stacks: every element
   through the stack rule, a stack that drops counted whole (D15).
   Absent Items writes nothing — Java then loads the empty list
   the absent Bedrock field meant. */
static bool carry_items(McmeArena *arena, const McmeTag *src,
                        McmeTag *j, McmeConvertReport *rep)
{
    const McmeTag *items = mcme_nbt_get(src, "Items");
    if (items == NULL || items->type != MCME_TAG_List) {
        return true;
    }
    McmeTag **stacks = NULL;
    for (size_t i = 0; i < items->list.len; ++i) {
        bool dropped = false;
        McmeTag *s = carry_stack(arena, &items->list.items[i],
                                 &dropped, true);
        if (s == NULL && !dropped) {
            arrfree(stacks);
            return false;
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
    return jl_ok
        && mcme_nbt_compound_put(arena, j, "Items", 5, jl);
}

/* A nonempty CustomName, and a Lock the world has set: Java spells
   both the same way, so both travel as they stand. */
static bool carry_name_lock(McmeArena *arena, const McmeTag *src,
                            McmeTag *j)
{
    const McmeTag *cn = mcme_nbt_get(src, "CustomName");
    if (cn != NULL && cn->type == MCME_TAG_String
            && cn->string.len > 0
            && !mcme_nbt_compound_put(arena, j, "CustomName", 10,
                                      (McmeTag *)cn)) {
        return false;
    }
    const McmeTag *lock = mcme_nbt_get(src, "Lock");
    if (lock != NULL && lock->type == MCME_TAG_String
            && !mcme_nbt_compound_put(arena, j, "Lock", 4,
                                      (McmeTag *)lock)) {
        return false;
    }
    return true;
}

/* One line of a sign as Java's JSON message string: quoted, with
   the spellings JSON escapes doubled through a backslash — quote,
   backslash, the five named control characters, any other control
   as \u00XX — and every other byte, the UTF-8 the world already
   wrote, passing as itself (measured: the reference's answer for a
   plain line is the line in JSON quotes; an empty line is the
   empty pair). */
static bool json_line(McmeArena *arena, const char *s, size_t len,
                      char **out, size_t *outlen)
{
    size_t n = 2;
    for (size_t i = 0; i < len; ++i) {
        unsigned char c = (unsigned char)s[i];
        if (c == '"' || c == '\\'
                || c == '\b' || c == '\f' || c == '\n'
                || c == '\r' || c == '\t') n += 2;
        else if (c < 0x20) n += 6;
        else n += 1;
    }
    char *buf = mcme_arena_alloc(arena, n + 1);
    if (buf == NULL) return false;
    size_t o = 0;
    buf[o++] = '"';
    for (size_t i = 0; i < len; ++i) {
        unsigned char c = (unsigned char)s[i];
        switch (c) {
        case '"':  buf[o++] = '\\'; buf[o++] = '"';  break;
        case '\\': buf[o++] = '\\'; buf[o++] = '\\'; break;
        case '\b': buf[o++] = '\\'; buf[o++] = 'b';  break;
        case '\f': buf[o++] = '\\'; buf[o++] = 'f';  break;
        case '\n': buf[o++] = '\\'; buf[o++] = 'n';  break;
        case '\r': buf[o++] = '\\'; buf[o++] = 'r';  break;
        case '\t': buf[o++] = '\\'; buf[o++] = 't';  break;
        default:
            if (c < 0x20) {
                static const char kHex[] = "0123456789abcdef";
                buf[o++] = '\\'; buf[o++] = 'u';
                buf[o++] = '0';  buf[o++] = '0';
                buf[o++] = kHex[(c >> 4) & 15];
                buf[o++] = kHex[c & 15];
            } else {
                buf[o++] = (char)c;
            }
            break;
        }
    }
    buf[o++] = '"';
    buf[o] = '\0';
    *out = buf;
    *outlen = o;
    return true;
}

/* One side of a sign: Java's front_text (or back_text) compound
   from Bedrock's FrontText (or BackText), measured through the
   reference on a real world.
 *
 * The four messages are the text's lines: the trailing newline
   ends the last line rather than starting an empty one, each line
   arrives as its JSON string, and a sign with fewer than four
   lines pads the rest with the escaped emptiness Java's own
   translator writes for a missing line. The color crosses as
   black with no glowing outline: the int the game stores for the
   color takes no path through the universal format the Java
   mapping reads, so every sign's answer — the reference's and
   mcme's — is black (D22). A document from before the two-sided
   format has neither FrontText nor BackText: it carries with
   both sides empty, its old top-level text having no measured
   Java spelling. */
static bool carry_sign_side(McmeArena *arena, const McmeTag *src,
                            const char *bedside,
                            const char *javaside, McmeTag *j)
{
    const McmeTag *side = mcme_nbt_get(src, bedside);
    const McmeTag *text = (side != NULL
                           && side->type == MCME_TAG_Compound)
        ? mcme_nbt_get(side, "Text") : NULL;
    const char *p = "";
    size_t left = 0;
    if (text != NULL && text->type == MCME_TAG_String) {
        p = text->string.data;
        left = text->string.len;
    }
    if (left > 0 && p[left - 1] == '\n') --left;

    McmeTag *msgs = mcme_nbt_new(arena, MCME_TAG_List);
    McmeTag *comp = mcme_nbt_new(arena, MCME_TAG_Compound);
    if (msgs == NULL || comp == NULL
            || !mcme_nbt_list_alloc(arena, msgs, MCME_TAG_String, 4)
            || !mcme_nbt_compound_init(arena, comp, 3)) {
        return false;
    }
    size_t line = 0;
    while (line < 4 && left > 0) {
        const char *nl = memchr(p, '\n', left);
        size_t ll = nl != NULL ? (size_t)(nl - p) : left;
        char *q = NULL;
        size_t ql = 0;
        McmeTag *t = mcme_nbt_new(arena, MCME_TAG_String);
        if (!json_line(arena, p, ll, &q, &ql) || t == NULL
                || !mcme_nbt_set_string(arena, t, q, ql)) {
            return false;
        }
        msgs->list.items[line] = *t;
        ++line;
        if (nl == NULL) break;
        p = nl + 1;
        left -= ll + 1;
    }
    static const char kPad[] = "\\\"\\\"";
    for (; line < 4; ++line) {
        McmeTag *t = mcme_nbt_new(arena, MCME_TAG_String);
        if (t == NULL || !mcme_nbt_set_string(arena, t, kPad, 4)) {
            return false;
        }
        msgs->list.items[line] = *t;
    }
    if (!put_string(arena, comp, "color", "black")
            || !put_byte(arena, comp, "has_glowing_text", 0)
            || !mcme_nbt_compound_put(arena, comp, "messages", 8,
                                      msgs)) {
        return false;
    }
    return mcme_nbt_compound_put(arena, j, javaside,
                                 strlen(javaside), comp);
}

/* The spawner's seven ranges, under the same names — both editions
   spell them alike, measured on every spawner in a real world —
   and the one spawn choice the document names: its
   EntityIdentifier becomes Java's SpawnData's id, a mob name every
   value a real world holds spells the same in both editions.
   The display fields the game adds describe a rendering Java
   computes itself, and SpawnPotentials is Java's own default;
   neither travels (D22). */
static bool carry_spawner(McmeArena *arena, const McmeTag *src,
                          McmeTag *j)
{
    static const char * const kRanges[] = {
        "Delay", "MaxNearbyEntities", "MaxSpawnDelay",
        "MinSpawnDelay", "RequiredPlayerRange", "SpawnCount",
        "SpawnRange"
    };
    for (size_t i = 0;
         i < sizeof kRanges / sizeof kRanges[0]; ++i) {
        int32_t v = 0;
        if (int_at(src, kRanges[i], &v)
                && !put_int(arena, j, kRanges[i], v)) {
            return false;
        }
    }
    const McmeTag *eid = mcme_nbt_get(src, "EntityIdentifier");
    if (eid != NULL && eid->type == MCME_TAG_String) {
        McmeTag *sd = mcme_nbt_new(arena, MCME_TAG_Compound);
        if (sd == NULL
                || !mcme_nbt_compound_init(arena, sd, 1)
                || !mcme_nbt_compound_put(arena, sd, "id", 2,
                                          (McmeTag *)eid)
                || !mcme_nbt_compound_put(arena, j, "SpawnData", 9,
                                          sd)) {
            return false;
        }
    }
    return true;
}

/* Age as Java's int. The exit portal: the three coordinates the
   game writes as a list in X/Y/Z order are Java's X/Y/Z compound
   (measured through the reference). The position check the game
   has already made has no Java spelling and stays behind (D15's
   field rule); a portal list spelled any other way writes no
   portal rather than a guessed one. */
static bool carry_gateway(McmeArena *arena, const McmeTag *src,
                          McmeTag *j)
{
    int32_t v = 0;
    if (int_at(src, "Age", &v) && !put_int(arena, j, "Age", v)) {
        return false;
    }
    const McmeTag *ep = mcme_nbt_get(src, "ExitPortal");
    int32_t xyz[3];
    if (ep != NULL && ep->type == MCME_TAG_List
            && ep->list.len == 3
            && tag_int(&ep->list.items[0], &xyz[0])
            && tag_int(&ep->list.items[1], &xyz[1])
            && tag_int(&ep->list.items[2], &xyz[2])) {
        McmeTag *c = mcme_nbt_new(arena, MCME_TAG_Compound);
        if (c == NULL
                || !mcme_nbt_compound_init(arena, c, 3)
                || !put_int(arena, c, "X", xyz[0])
                || !put_int(arena, c, "Y", xyz[1])
                || !put_int(arena, c, "Z", xyz[2])
                || !mcme_nbt_compound_put(arena, j, "ExitPortal", 10,
                                          c)) {
            return false;
        }
    }
    return true;
}

/* The four cooks' progress as Java's one int array: ItemTime1
   through ItemTime4 are Java's four slots in order (measured: the
   reference answers every campfire with all four). A document
   missing one writes no array — Java's own default then says what
   the absent fields said. This world's campfires hold no items to
   cook (measured: none ever did), so no Items travels (D22). */
static bool carry_campfire(McmeArena *arena, const McmeTag *src,
                           McmeTag *j)
{
    static const char * const kTimes[] = {
        "ItemTime1", "ItemTime2", "ItemTime3", "ItemTime4"
    };
    int32_t t[4];
    bool all = true;
    for (size_t i = 0; i < 4; ++i) {
        all = all && int_at(src, kTimes[i], &t[i]);
    }
    if (all) {
        McmeTag *arr = mcme_nbt_new(arena, MCME_TAG_Int_Array);
        if (arr == NULL
                || !mcme_nbt_set_ints(arena, arr, t, 4)
                || !mcme_nbt_compound_put(arena, j, "CookingTimes",
                                          12, arr)) {
            return false;
        }
    }
    return true;
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
    int kind = BE_EMPTY;
    const char *java_id =
        carried_java_id(id->string.data, id->string.len, &kind);
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

    /* What travels beside id and position, per family. */
    switch (kind) {
    case BE_CONTAINER:
        if (!carry_items(arena, src, j, rep)
                || !carry_name_lock(arena, src, j)) {
            return BE_FAIL;
        }
        /* The smelter timers, under Java's names as Java's ints. */
        {
            static const char * const kTimers[] = {
                "BurnTime", "CookTime", "CookTimeTotal"
            };
            for (size_t i = 0;
                 i < sizeof kTimers / sizeof kTimers[0]; ++i) {
                int32_t v = 0;
                if (int_at(src, kTimers[i], &v)
                        && !put_int(arena, j, kTimers[i], v)) {
                    return BE_FAIL;
                }
            }
        }
        break;

    case BE_BREWING:
        /* The brewing stand is a container whose progress the game
           spells CookTime — under Java's BrewTime, not the
           furnace's CookTime — and whose fuel gauge Java spells
           Fuel. FuelTotal is the game's bookkeeping of what a
           bottle of fuel started with; Java keeps only what is
           left (D22, measured through the reference). */
        if (!carry_items(arena, src, j, rep)
                || !carry_name_lock(arena, src, j)) {
            return BE_FAIL;
        }
        {
            int32_t v = 0;
            if (int_at(src, "CookTime", &v)
                    && !put_int(arena, j, "BrewTime", v)) {
                return BE_FAIL;
            }
            if (int_at(src, "FuelAmount", &v)
                    && !put_int(arena, j, "Fuel", v)) {
                return BE_FAIL;
            }
        }
        break;

    case BE_SPAWNER:
        if (!carry_spawner(arena, src, j)) return BE_FAIL;
        break;

    case BE_SIGN:
        if (!carry_sign_side(arena, src, "FrontText", "front_text",
                             j)
                || !carry_sign_side(arena, src, "BackText",
                                    "back_text", j)) {
            return BE_FAIL;
        }
        {
            int32_t waxed = 0;
            if (int_at(src, "IsWaxed", &waxed)
                    && !put_byte(arena, j, "is_waxed",
                                 (int8_t)waxed)) {
                return BE_FAIL;
            }
        }
        break;

    case BE_COMPARATOR:
        {
            int32_t v = 0;
            if (int_at(src, "OutputSignal", &v)
                    && !put_int(arena, j, "OutputSignal", v)) {
                return BE_FAIL;
            }
        }
        break;

    case BE_BEACON:
        /* The chosen primary and secondary effects: the game spells
           them lowercase, Java capitalized, same ints — the one
           pair where both names are visibly the same word (D22,
           measured: java's defaults fill the rest). */
        {
            int32_t v = 0;
            if (int_at(src, "primary", &v)
                    && !put_int(arena, j, "Primary", v)) {
                return BE_FAIL;
            }
            if (int_at(src, "secondary", &v)
                    && !put_int(arena, j, "Secondary", v)) {
                return BE_FAIL;
            }
        }
        break;

    case BE_GATEWAY:
        if (!carry_gateway(arena, src, j)) return BE_FAIL;
        break;

    case BE_CAMPFIRE:
        if (!carry_campfire(arena, src, j)) return BE_FAIL;
        break;

    case BE_JUKEBOX:
        /* One record, standing alone: no Slot in the stack (the
           rule with `with_slot` off). An item the rule cannot read
           drops as a whole — counted like any stack — and the
           document still carries its position. */
        {
            const McmeTag *ri = mcme_nbt_get(src, "RecordItem");
            if (ri != NULL) {
                bool dropped = false;
                McmeTag *s =
                    carry_stack(arena, ri, &dropped, false);
                if (s == NULL && !dropped) return BE_FAIL;
                if (s != NULL) {
                    if (!mcme_nbt_compound_put(arena, j,
                                               "RecordItem", 10,
                                               s)) {
                        return BE_FAIL;
                    }
                } else {
                    rep->items++;
                }
            }
        }
        break;

    case BE_SHELF:
        if (!carry_items(arena, src, j, rep)) return BE_FAIL;
        {
            int32_t v = 0;
            if (int_at(src, "LastInteractedSlot", &v)
                    && !put_byte(arena, j, "last_interacted_slot",
                                 (int8_t)v)) {
                return BE_FAIL;
            }
        }
        break;

    case BE_EMPTY:
    default:
        /* Nothing: this world's documents hold no field Java's
           block entity for the type spells (measured per type —
           the sculk listener's selector, the skull's animation
           and rotation, the lectern's absent book). */
        break;
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
    uint8_t key[13];      /* the game's longest chunk key (D21)     */
    size_t klen = 0;
    if (!mcme_bedrock_key_build(key, sizeof key, &klen,
                                ref->cx, ref->cz, ref->dim, ref->tag,
                                false, 0)) {
        mcme_set_error("the tick record key of dimension %d at "
                       "(%d, %d) does not fit", (int)ref->dim,
                       ref->cx, ref->cz);
        refuse(refusal, MCME_WRITE_REFUSAL_INTERNAL);
        return false;
    }
    McmeBuffer val;
    mcme_buffer_init(&val);
    bool deleted = false;
    if (!mcme_leveldb_get(db, key, klen, &val, &deleted)) {
        mcme_buffer_free(&val);
        mcme_set_error("the tick record of dimension %d at "
                       "(%d, %d) is gone from the store",
                       (int)ref->dim, ref->cx, ref->cz);
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
            rep->ticks++;      /* a blockState the map refuses: dropped */
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

static bool count_unconverted_biomes(McmeLevelDb *db,
                                     const BiomeRef *refs, size_t n,
                                     McmeArena *arena,
                                     McmeConvertReport *rep,
                                     McmeWriteRefusal *refusal);

/* The record's heightmap, every column inside the world Java
   spells for this dimension — [0, dim_height(dim)], the range the
   measured store's overworld columns sat in (10..309, over 36,848
   records). One column outside is not a map mcme writes: that half
   is refused and counted, the cells beside it still cross (D20). */
static bool heightmap_fits(const int16_t *hm, int32_t height)
{
    for (size_t i = 0; i < MCME_BEDROCK_HEIGHTMAP_COLS; ++i) {
        if (hm[i] < 0 || hm[i] > height) return false;
    }
    return true;
}

/* One Java heightmap: the record's 256 columns packed the way 1.16+
   spells them — 9 bits to a value, 7 values to a long, none
   spanning two, 37 longs — under the one name the record can
   honestly answer. The columns are already Java's values (first air
   above the floor, collidables and fluids counted) and already
   Java's order (x + 16z): the record and the map speak the same
   numbers, and only the packing changes. */
static bool make_heightmap(McmeArena *arena, const int16_t *hm,
                           McmeTag **out)
{
    enum { kBits = 9, kPerLong = 7, kLongs = 37 };
    *out = NULL;
    int64_t *longs = mcme_arena_alloc(arena, kLongs * sizeof *longs);
    if (longs == NULL) return false;
    memset(longs, 0, kLongs * sizeof *longs);
    for (size_t p = 0; p < MCME_BEDROCK_HEIGHTMAP_COLS; ++p) {
        size_t word = p / kPerLong;
        size_t bit = (p % kPerLong) * kBits;
        longs[word] |= (int64_t)((uint64_t)(uint16_t)hm[p] << bit);
    }
    McmeTag *data = mcme_nbt_new(arena, MCME_TAG_Long_Array);
    McmeTag *maps = mcme_nbt_new(arena, MCME_TAG_Compound);
    if (data == NULL || maps == NULL
        || !mcme_nbt_set_longs(arena, data, longs, kLongs)
        || !mcme_nbt_compound_init(arena, maps, 1)
        || !mcme_nbt_compound_put(arena, maps, "MOTION_BLOCKING", 15,
                                  data)) {
        return false;
    }
    *out = maps;
    return true;
}

static bool convert_chunk(McmeLevelDb *db, const char *out,
                          int32_t dim,
                          const SubRef *subs, size_t n,
                          const BeRef *bes, size_t nb,
                          const TickRef *ticks, size_t ntk,
                          const BiomeRef *biomes, size_t nbi,
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

    /* The chunk's Data3D record, decoded once before the sections it
       decorates are built. One record is the shape — one key per
       chunk — so the span's first entry is the chunk's own; a second
       spelling of the same record is not merged by guessing which the
       game means, and its cells count as not carried like any other
       record the chunk gets no use from. A record that will not
       decode drops every cell it would have held, counted once; a
       tombstone held nothing to drop. The record's other half — its
       heightmap — rides the same verdicts and counts for itself:
       it crosses when the record decodes and every column spells
       inside the world's height (D20). */
    McmeBedrockBiomes bis;
    bool has_biomes = false;
    bool has_heightmap = false;
    memset(&bis, 0, sizeof bis);
    if (nbi > 1
            && !count_unconverted_biomes(db, biomes + 1, nbi - 1, arena,
                                         rep, refusal)) {
        mcme_arena_free(arena);
        return false;
    }
    if (nbi > 0) {
        McmeBuffer val;
        mcme_buffer_init(&val);
        bool deleted = false;
        if (!mcme_leveldb_get(db, biomes[0].key, biomes[0].key_len,
                              &val, &deleted)) {
            mcme_buffer_free(&val);
            mcme_set_error("the biome record of dimension %d at "
                           "(%d, %d) is gone from the store",
                           (int)dim, cx, cz);
            refuse(refusal, MCME_WRITE_REFUSAL_STORE);
            mcme_arena_free(arena);
            return false;
        }
        if (deleted) {
            mcme_buffer_free(&val);
            /* A tombstone held no map either — nothing to drop. */
        } else if (!mcme_bedrock_biome_decode(&bis, arena, val.data,
                                              val.len,
                                              dim_floor_cy(dim))) {
            mcme_buffer_free(&val);
            mcme_clear_error();
            rep->biomes += 1;      /* the record counts once    */
            rep->heightmaps += 1;  /* its map is lost with it   */
        } else {
            mcme_buffer_free(&val);
            has_biomes = true;
            /* The map crosses when every column is one the world's
               height spells; the cells cross regardless (D20). */
            has_heightmap = heightmap_fits(bis.heightmap,
                                           dim_height(dim));
            if (has_heightmap) rep->heightmaps_carried++;
            else rep->heightmaps += 1;
        }
    } else {
        /* A converted chunk whose store held no record: nothing for
           the cells to come from, nothing for the map either — and
           the map counts, the drop it is (D20). */
        rep->heightmaps += 1;
    }
    uint8_t taken[MCME_BEDROCK_BIOME_MAX_ARRAYS] = {0};

    for (size_t i = 0; i < n; ++i) {
        McmeBuffer val;
        mcme_buffer_init(&val);
        bool deleted = false;
        if (!mcme_leveldb_get(db, subs[i].key, subs[i].key_len, &val,
                              &deleted)) {
            mcme_buffer_free(&val);
            mcme_set_error("the subchunk of dimension %d at "
                           "(%d, %d, %d) is gone from the store",
                           (int)dim, subs[i].cx, (int)subs[i].cy,
                           subs[i].cz);
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
            /* A payload this build does not decode — a version
               nothing decodes, a payload cut short or with a tail
               (the pre-palette versions decode, D25) — drops the
               subchunk as air, counted (D12). */
            mcme_buffer_free(&val);
            mcme_clear_error();
            rep->legacy_subchunks++;
            continue;
        }
        mcme_buffer_free(&val);
        rep->subchunks++;
        if (sc.layer_count == 0) continue;   /* the all-air marker */
        const uint32_t *cells = NULL;
        size_t arr_i = 0;
        if (has_biomes) {
            for (size_t b = 0; b < bis.count; ++b) {
                if (bis.arrays[b].cy == subs[i].cy) {
                    cells = bis.arrays[b].cells;
                    arr_i = b;
                    break;
                }
            }
        }
        McmeTag *sec = NULL;
        if (!make_section(arena, subs[i].cy, &sc.layers[0],
                          sc.layer_count > 1 ? &sc.layers[1] : NULL,
                          cells, rep, &sec)) {
            mcme_set_error("out of memory building the "
                           "section at (%d, %d, %d)", subs[i].cx,
                           (int)subs[i].cy, subs[i].cz);
            refuse(refusal, MCME_WRITE_REFUSAL_INTERNAL);
            arrfree(built);
            mcme_arena_free(arena);
            return false;
        }
        if (sec != NULL) {
            arrpush(built, sec);
            if (cells != NULL) taken[arr_i] = 1;
        }
    }

    /* An array whose Y got no section — a stratum with no subchunk
       key beneath it, a legacy payload, an all-air marker, a record
       reaching past the chunk's own top — carried nowhere: its cells
       count (D19). Measured on one real store every such cell was the
       first kind: 0.13% of all cells, in 343 chunks, all pure-air
       strata beside the terrain the sections do hold. */
    if (has_biomes) {
        for (size_t b = 0; b < bis.count; ++b) {
            if (!taken[b]) rep->biomes += MCME_BEDROCK_BIOME_CELLS;
        }
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
            db, cx, cz, dim, arena, &bdocs, &ndocs);
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
        && put_int(arena, root, "yPos", dim_floor_cy(dim))
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
    /* MOTION_BLOCKING: the chunk's own heightmap, the other half of
       the same Data3D record the biomes came from, its columns
       already Java's numbers (D20). A chunk whose record held no
       writable map gets no key at all — the shape the store speaks,
       and absent is the game's own answer to compute. */
    McmeTag *hmaps = NULL;
    if (has_heightmap
            && !make_heightmap(arena, bis.heightmap, &hmaps)) {
        ok = false;
    }
    ok = ok && (hmaps == NULL
                || mcme_nbt_compound_put(arena, root, "Heightmaps",
                                         10, hmaps));
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
    char dimpath[16] = "";
    if (dim != 0) {
        const char *dd = NULL;
        if (!mcme_java_dimension_dir(dim, &dd)) {
            /* scan_key admits only the game's three dimensions, so
               this is a wrong number reaching here, not world data. */
            refuse(refusal, MCME_WRITE_REFUSAL_INTERNAL);
            arrfree(built);
            arrfree(carried);
            arrfree(fluid);
            arrfree(block);
            mcme_arena_free(arena);
            return false;
        }
        snprintf(dimpath, sizeof dimpath, "%s/", dd);
    }
    int w = snprintf(file, sizeof file, "%s/%sregion/r.%d.%d.mca",
                     out, dimpath, cx >> 5, cz >> 5);
    if (w <= 0 || (size_t)w >= sizeof file) {
        mcme_set_error("the region path for chunk (%d, %d) of "
                       "dimension %d does not fit", cx, cz, (int)dim);
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
            db, bes[i].cx, bes[i].cz, bes[i].dim, arena, &docs, &nd);
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

/* Entity-list records: the whole record is the drop. Unlike every
   other record kind there is no pairing to respect — no entity
   document crosses into Java (the reference answers nothing a Java
   entity can load, decisions.md#D23), so a record's documents all
   count here, once, whether its chunk converts or never appears.
   The read is the block-entity reader's own: documents counted, a
   record that will not decode counted once, an empty record
   counting nothing — never a partial answer. */
static bool count_unconverted_entities(McmeLevelDb *db,
                                       const EntRef *ents, size_t n,
                                       McmeArena *arena,
                                       McmeConvertReport *rep,
                                       McmeWriteRefusal *refusal)
{
    for (size_t i = 0; i < n; ++i) {
        McmeNbtDoc *docs = NULL;
        size_t nd = 0;
        McmeEntityStatus st = mcme_bedrock_chunk_entities(
            db, ents[i].cx, ents[i].cz, ents[i].dim, arena, &docs, &nd);
        if (st == MCME_ENTITY_OK) {
            rep->entities += nd;
        } else if (st == MCME_ENTITY_NOT_READABLE) {
            mcme_clear_error();
            rep->entities += 1;   /* the record counts once */
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
        uint8_t key[13];  /* the game's longest chunk key (D21)     */
        size_t klen = 0;
        if (!mcme_bedrock_key_build(key, sizeof key, &klen,
                                    refs[i].cx, refs[i].cz, refs[i].dim,
                                    refs[i].tag, false, 0)) {
            mcme_set_error("the tick record key of dimension %d at "
                           "(%d, %d) does not fit", (int)refs[i].dim,
                           refs[i].cx, refs[i].cz);
            refuse(refusal, MCME_WRITE_REFUSAL_INTERNAL);
            return false;
        }
        McmeBuffer val;
        mcme_buffer_init(&val);
        bool deleted = false;
        if (!mcme_leveldb_get(db, key, klen, &val, &deleted)) {
            mcme_buffer_free(&val);
            mcme_set_error("the tick record of dimension %d at "
                           "(%d, %d) is gone from the store",
                           (int)refs[i].dim, refs[i].cx, refs[i].cz);
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

/* A biome record no chunk gets use from — its span's chunk never
   appears in the subchunk scan. The whole record is the drop: its
   cells count (decoded first, since the count is the payload's own
   shape), and a record that will not decode counts once like any
   other, never half-read. */
static bool count_unconverted_biomes(McmeLevelDb *db,
                                     const BiomeRef *refs, size_t n,
                                     McmeArena *arena,
                                     McmeConvertReport *rep,
                                     McmeWriteRefusal *refusal)
{
    for (size_t i = 0; i < n; ++i) {
        McmeBuffer val;
        mcme_buffer_init(&val);
        bool deleted = false;
        if (!mcme_leveldb_get(db, refs[i].key, refs[i].key_len, &val,
                              &deleted)) {
            mcme_buffer_free(&val);
            mcme_set_error("the biome record of dimension %d at "
                           "(%d, %d) is gone from the store",
                           (int)refs[i].dim, refs[i].cx, refs[i].cz);
            refuse(refusal, MCME_WRITE_REFUSAL_STORE);
            return false;
        }
        if (deleted) {
            mcme_buffer_free(&val);
            continue;          /* a tombstone holds no cells */
        }
        McmeBedrockBiomes biomes;
        if (!mcme_bedrock_biome_decode(&biomes, arena, val.data, val.len,
                                       dim_floor_cy(refs[i].dim))) {
            mcme_buffer_free(&val);
            mcme_clear_error();
            rep->biomes += 1;      /* the record counts once   */
            rep->heightmaps += 1;  /* its map is lost with it  */
            continue;
        }
        mcme_buffer_free(&val);
        rep->biomes += biomes.count * MCME_BEDROCK_BIOME_CELLS;
        /* The map of a record no chunk gets: counted, never carried
           (D20) — the same rule its cells already follow. */
        rep->heightmaps += 1;
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
    scan.ents = NULL;
    scan.ticks = NULL;
    scan.biomes = NULL;
    memset(scan.has_subs, 0, sizeof scan.has_subs);
    scan.rep = report;
    mcme_leveldb_iterate(db, scan_key, &scan);
    size_t nsubs = (size_t)arrlen(scan.subs);
    size_t nbes = (size_t)arrlen(scan.bes);
    size_t nent = (size_t)arrlen(scan.ents);
    size_t ntk = (size_t)arrlen(scan.ticks);
    size_t nbi = (size_t)arrlen(scan.biomes);
    if (nsubs > 1) qsort(scan.subs, nsubs, sizeof (SubRef), sub_cmp);
    if (nbes > 1) qsort(scan.bes, nbes, sizeof (BeRef), be_cmp);
    if (nent > 1) qsort(scan.ents, nent, sizeof (EntRef), ent_cmp);
    if (ntk > 1) qsort(scan.ticks, ntk, sizeof (TickRef), tick_cmp);
    if (nbi > 1) qsort(scan.biomes, nbi, sizeof (BiomeRef), biome_cmp);

    /* The nether and the end arrive where Java reads them: their own
       directories, made only when the store holds chunks there — an
       output tree spells the dimensions its input had, the same
       emptiness the reference-shaped outputs keep (D21). */
    for (int32_t d = 1; d <= 2; ++d) {
        if (!scan.has_subs[d]) continue;
        const char *dd = NULL;
        char dimdir[4096];
        w = mcme_java_dimension_dir(d, &dd)
            ? snprintf(dimdir, sizeof dimdir, "%s/%s/region", out, dd)
            : -1;
        if (w > 0 && (size_t)w < sizeof dimdir
                && mcme_fs_mkdir_p(dimdir)) {
            continue;
        }
        if (mcme_last_error()[0] == '\0') {
            mcme_set_error("cannot write the dimension %d region "
                           "directory under '%s'", (int)d, out);
        }
        arrfree(scan.subs);
        arrfree(scan.bes);
        arrfree(scan.ents);
        arrfree(scan.ticks);
        arrfree(scan.biomes);
        mcme_leveldb_close(db);
        mcme_world_info_free(&info);
        rm_rf(out);
        refuse(refusal, MCME_WRITE_REFUSAL_STORE);
        return false;
    }

    /* Every list is ordered by (dimension, chunk), so one walk pairs
       each record with the chunks that convert beside it — the
       nether's records with the nether's chunks, never the
       overworld's at the same coordinates (D21). A record whose
       chunk never appears has no Java chunk to live in; its
       documents and entries are counted, not carried — a drop the
       report names like any other. */
    bool ok = true;
    McmeArena *rec_arena = mcme_arena_new();
    if (rec_arena == NULL) {
        mcme_set_error("out of memory");
        refuse(refusal, MCME_WRITE_REFUSAL_INTERNAL);
        ok = false;
    }
    size_t i = 0, bi = 0, ki = 0, bmi = 0;
    while (ok && i < nsubs) {
        size_t j = i;
        while (j < nsubs && scan.subs[j].dim == scan.subs[i].dim
               && scan.subs[j].cx == scan.subs[i].cx
               && scan.subs[j].cz == scan.subs[i].cz) {
            ++j;
        }
        int32_t dim = scan.subs[i].dim;
        int32_t cx = scan.subs[i].cx;
        int32_t cz = scan.subs[i].cz;
        while (ok && bi < nbes
               && pos_before(scan.bes[bi].dim, scan.bes[bi].cx,
                             scan.bes[bi].cz, dim, cx, cz)) {
            ok = count_unconverted_records(db, scan.bes + bi, 1,
                                           rec_arena, report, refusal);
            ++bi;
        }
        size_t bj = bi;
        while (bj < nbes && scan.bes[bj].dim == dim
               && scan.bes[bj].cx == cx
               && scan.bes[bj].cz == cz) {
            ++bj;
        }
        while (ok && ki < ntk
               && pos_before(scan.ticks[ki].dim, scan.ticks[ki].cx,
                             scan.ticks[ki].cz, dim, cx, cz)) {
            ok = count_unconverted_ticks(db, scan.ticks + ki, 1,
                                         rec_arena, report, refusal);
            ++ki;
        }
        size_t kj = ki;
        while (kj < ntk && scan.ticks[kj].dim == dim
               && scan.ticks[kj].cx == cx
               && scan.ticks[kj].cz == cz) {
            ++kj;
        }
        while (ok && bmi < nbi
               && pos_before(scan.biomes[bmi].dim, scan.biomes[bmi].cx,
                             scan.biomes[bmi].cz, dim, cx, cz)) {
            ok = count_unconverted_biomes(db, scan.biomes + bmi, 1,
                                          rec_arena, report, refusal);
            ++bmi;
        }
        size_t bmj = bmi;
        while (bmj < nbi && scan.biomes[bmj].dim == dim
               && scan.biomes[bmj].cx == cx
               && scan.biomes[bmj].cz == cz) {
            ++bmj;
        }
        if (ok) {
            ok = convert_chunk(db, out, dim, scan.subs + i, j - i,
                               scan.bes + bi, bj - bi,
                               scan.ticks + ki, kj - ki,
                               scan.biomes + bmi, bmj - bmi, report,
                               refusal);
        }
        bi = bj;
        ki = kj;
        bmi = bmj;
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
    while (ok && bmi < nbi) {
        ok = count_unconverted_biomes(db, scan.biomes + bmi, 1,
                                      rec_arena, report, refusal);
        ++bmi;
    }
    /* The entity-list records last, together: nothing about them
       depends on which chunks converted (D23), so they need no
       place in the pairing walk above — every record is read once
       here and its documents counted. */
    size_t ei = 0;
    while (ok && ei < nent) {
        ok = count_unconverted_entities(db, scan.ents + ei, 1,
                                        rec_arena, report, refusal);
        ++ei;
    }
    mcme_arena_free(rec_arena);

    arrfree(scan.subs);
    arrfree(scan.bes);
    arrfree(scan.ents);
    arrfree(scan.ticks);
    arrfree(scan.biomes);
    mcme_leveldb_close(db);
    mcme_world_info_free(&info);
    if (!ok) rm_rf(out);
    return ok;
}
