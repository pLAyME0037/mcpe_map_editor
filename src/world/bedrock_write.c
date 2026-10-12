#include "mcme/bedrock_write.h"

#include "mcme/bedrock_chunk.h"
#include "mcme/bedrock_keys.h"
#include "mcme/leveldb.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The version the reference assigns a block that
   arrives without one — amulet's
   base_leveldb_interface: "1, 14, 0, 0". What a
   freshly placed block writes, to match. */
#define BLOCK_VERSION_DEFAULT 17694720

/* Floor division by 16, for coordinates that may be
   negative: C truncates towards zero, and the chunk a
   negative block falls in is the one below it. */
static int32_t floor_div16(int32_t v)
{
    int32_t q = v / 16;
    if (v % 16 != 0 && v < 0) q -= 1;
    return q;
}

/* The remainder mod 16, always 0 to 15: the block's
   position inside its chunk or subchunk. */
static int32_t mod16(int32_t v)
{
    int32_t r = v % 16;
    return r < 0 ? r + 16 : r;
}

bool mcme_bedrock_subchunk_key(int32_t x, int32_t y,
                               int32_t z, int32_t dimension,
                               uint8_t *out, size_t cap,
                               size_t *len)
{
    if (out == NULL || len == NULL) {
        mcme_set_error("subchunk key: null argument");
        return false;
    }

    int32_t cy = floor_div16(y);
    if (cy < INT8_MIN || cy > INT8_MAX) {
        mcme_set_error("subchunk key: subchunk Y %d does "
                       "not fit the key's signed byte", cy);
        return false;
    }

    return mcme_bedrock_key_build(out, cap, len,
                                  floor_div16(x),
                                  floor_div16(z),
                                  dimension,
                                  MCME_BEDROCK_TAG_SUBCHUNK,
                                  true, (int8_t)cy);
}

bool mcme_bedrock_block_entry(McmeArena *arena,
                              const char *name,
                              const McmeTag *states,
                              McmeNbtDoc *out)
{
    if (arena == NULL || name == NULL || out == NULL) {
        mcme_set_error("block entry: null argument");
        return false;
    }
    if (states != NULL && states->type != MCME_TAG_Compound) {
        mcme_set_error("block entry: states is not a "
                       "compound");
        return false;
    }

    size_t name_len = strlen(name);
    if (name_len == 0) {
        mcme_set_error("block entry: empty block name");
        return false;
    }

    char *name_copy   = mcme_arena_alloc(arena, name_len);
    char *key_name    = mcme_arena_alloc(arena, 4);
    char *key_states  = mcme_arena_alloc(arena, 6);
    char *key_version = mcme_arena_alloc(arena, 7);
    McmeTag *name_tag    = mcme_arena_alloc(arena, sizeof *name_tag);
    McmeTag *states_tag  = mcme_arena_alloc(arena, sizeof *states_tag);
    McmeTag *version_tag = mcme_arena_alloc(arena, sizeof *version_tag);
    McmeCompoundEntry *entries =
        mcme_arena_alloc(arena, 3 * sizeof *entries);
    McmeTag *root = mcme_arena_alloc(arena, sizeof *root);
    if (name_copy == NULL || key_name == NULL
            || key_states == NULL || key_version == NULL
            || name_tag == NULL || states_tag == NULL
            || version_tag == NULL || entries == NULL
            || root == NULL) {
        return false;
    }

    memcpy(name_copy, name, name_len);
    memcpy(key_name, "name", 4);
    memcpy(key_states, "states", 6);
    memcpy(key_version, "version", 7);

    name_tag->type = MCME_TAG_String;
    name_tag->string.data = name_copy;
    name_tag->string.len = name_len;

    if (states != NULL) {
        *states_tag = *states;       /* borrowed, not copied */
    } else {
        states_tag->type = MCME_TAG_Compound;
        states_tag->compound.entries = NULL;
        states_tag->compound.len = 0;
    }

    version_tag->type = MCME_TAG_Int;
    version_tag->as_int = BLOCK_VERSION_DEFAULT;

    entries[0].name.data = key_name;
    entries[0].name.len = 4;
    entries[0].value = name_tag;
    entries[1].name.data = key_states;
    entries[1].name.len = 6;
    entries[1].value = states_tag;
    entries[2].name.data = key_version;
    entries[2].name.len = 7;
    entries[2].value = version_tag;

    root->type = MCME_TAG_Compound;
    root->compound.entries = entries;
    root->compound.len = 3;

    out->arena = arena;
    out->root = root;
    out->root_name.data = (char *)"";
    out->root_name.len = 0;
    out->little_endian = true;
    return true;
}

/* ========================================================================
 * Creating a subchunk
 * ======================================================================== */

bool mcme_bedrock_subchunk_air(McmeArena *arena, uint8_t version,
                               int8_t index, McmeBedrockSubchunk *sc)
{
    if (arena == NULL || sc == NULL) {
        mcme_set_error("subchunk: air: null argument");
        return false;
    }
    if (version != 8 && version != 9) {
        mcme_set_error("subchunk: air: mcme creates a version 8 or 9 "
                       "subchunk, not %u", version);
        return false;
    }

    McmeBedrockLayer *layer = mcme_arena_alloc(arena, sizeof *layer);
    uint16_t *indices =
        mcme_arena_alloc(arena, MCME_BEDROCK_SUBCHUNK_BLOCKS * sizeof *indices);
    McmeNbtDoc *palette = mcme_arena_alloc(arena, sizeof *palette);
    if (layer == NULL || indices == NULL || palette == NULL) {
        mcme_set_error("subchunk: air: out of memory");
        return false;
    }
    memset(indices, 0,
           MCME_BEDROCK_SUBCHUNK_BLOCKS * sizeof *indices);
    if (!mcme_bedrock_block_entry(arena, "minecraft:air", NULL,
                                  palette)) {
        return false;
    }

    layer->bits = 0;        /* one entry fills the layer */
    layer->indices = indices;
    layer->palette_len = 1;
    layer->palette = palette;

    sc->version = version;
    sc->has_index = (version == 9);
    sc->index = index;
    sc->layer_count = 1;
    sc->layers = layer;
    return true;
}

/* The version a stored neighbour uses, so a subchunk mcme
   creates matches the world around it: 9 when a neighbour
   carries the Y index, otherwise 8 — what the reference writes
   for a new subchunk. Only the two neighbours are asked; a world
   that mixes versions deeper than that gets 8, which every
   version of the game reads. */
static uint8_t neighbour_version(McmeLevelDb *db, int32_t cx,
                                 int32_t cz, int32_t cy,
                                 int32_t dimension)
{
    for (int d = -1; d <= 1; d += 2) {
        int32_t ncy = cy + d;
        if (ncy < INT8_MIN || ncy > INT8_MAX) continue;
        uint8_t key[14];
        size_t key_len = 0;
        if (!mcme_bedrock_key_build(key, sizeof key, &key_len,
                                    cx, cz, dimension,
                                    MCME_BEDROCK_TAG_SUBCHUNK,
                                    true, (int8_t)ncy)) {
            continue;
        }
        McmeBuffer p;
        mcme_buffer_init(&p);
        bool deleted = false;
        bool is_nine = mcme_leveldb_get(db, key, key_len, &p,
                                        &deleted)
                       && !deleted && p.len > 0 && p.data[0] == 9;
        mcme_buffer_free(&p);
        if (is_nine) return 9;
    }
    return 8;
}

bool mcme_bedrock_subchunk_get_or_create(McmeLevelDb *db,
                                         int32_t cx, int32_t cz,
                                         int32_t cy,
                                         int32_t dimension,
                                         McmeArena *arena,
                                         McmeBedrockSubchunk *sc,
                                         McmeWriteRefusal *refusal)
{
    if (refusal != NULL) *refusal = MCME_WRITE_REFUSAL_NONE;

    if (db == NULL || arena == NULL || sc == NULL) {
        if (refusal != NULL) *refusal = MCME_WRITE_REFUSAL_USAGE;
        mcme_set_error("subchunk: null argument");
        return false;
    }
    if (cy < INT8_MIN || cy > INT8_MAX) {
        if (refusal != NULL) *refusal = MCME_WRITE_REFUSAL_USAGE;
        mcme_set_error("subchunk: Y %d does not fit the key's "
                       "signed byte", (int)cy);
        return false;
    }

    uint8_t key[14];
    size_t key_len = 0;
    if (!mcme_bedrock_key_build(key, sizeof key, &key_len,
                                cx, cz, dimension,
                                MCME_BEDROCK_TAG_SUBCHUNK,
                                true, (int8_t)cy)) {
        if (refusal != NULL) *refusal = MCME_WRITE_REFUSAL_USAGE;
        return false;
    }

    if (!mcme_leveldb_has(db, key, key_len)) {
        /* Absent: the subchunk the caller is about to fill in is
           all air, in the version the neighbours use. The probe
           above called absence "key not found"; that is the
           answer, not a fault, so it does not stay behind as the
           last error of a call that is about to succeed. */
        uint8_t version = neighbour_version(db, cx, cz, cy,
                                            dimension);
        bool made = mcme_bedrock_subchunk_air(arena, version,
                                              (int8_t)cy, sc);
        if (made) {
            mcme_clear_error();
        } else if (refusal != NULL) {
            *refusal = MCME_WRITE_REFUSAL_INTERNAL; /* OOM */
        }
        return made;
    }

    McmeBuffer payload;
    mcme_buffer_init(&payload);
    bool deleted = false;
    if (!mcme_leveldb_get(db, key, key_len, &payload, &deleted)) {
        mcme_buffer_free(&payload);
        if (refusal != NULL) *refusal = MCME_WRITE_REFUSAL_STORE;
        return false;           /* the store's own error stands */
    }
    if (deleted) {
        mcme_buffer_free(&payload);
        if (refusal != NULL) *refusal = MCME_WRITE_REFUSAL_NOT_THERE;
        mcme_set_error("subchunk (%d, %d, %d) is deleted in this "
                       "world", cx, cz, (int)cy);
        return false;
    }
    bool ok = mcme_bedrock_subchunk_decode(sc, arena, payload.data,
                                           payload.len);
    if (!ok && refusal != NULL)
        *refusal = MCME_WRITE_REFUSAL_NOT_READABLE;
    mcme_buffer_free(&payload);
    return ok;
}

bool mcme_bedrock_set_block(const char *db_dir,
                            int32_t x, int32_t y, int32_t z,
                            int32_t dimension,
                            const McmeNbtDoc *block,
                            McmeWriteRefusal *refusal)
{
    if (refusal != NULL) *refusal = MCME_WRITE_REFUSAL_NONE;

    if (db_dir == NULL || block == NULL) {
        if (refusal != NULL) *refusal = MCME_WRITE_REFUSAL_USAGE;
        mcme_set_error("set block: null argument");
        return false;
    }

    int32_t cx = floor_div16(x), cz = floor_div16(z);
    int32_t cy = floor_div16(y);
    if (cy < INT8_MIN || cy > INT8_MAX) {
        if (refusal != NULL) *refusal = MCME_WRITE_REFUSAL_USAGE;
        mcme_set_error("set block: subchunk Y %d does not "
                       "fit the key's signed byte", cy);
        return false;
    }

    uint8_t key[14];
    size_t key_len = 0;
    if (!mcme_bedrock_subchunk_key(x, y, z, dimension,
                                   key, sizeof key,
                                   &key_len)) {
        if (refusal != NULL) *refusal = MCME_WRITE_REFUSAL_USAGE;
        return false;
    }

    McmeLevelDb *db = NULL;
    if (!mcme_leveldb_open_rw(&db, db_dir)) {
        if (refusal != NULL) *refusal = MCME_WRITE_REFUSAL_STORE;
        return false;
    }

    /* Every path from here frees the same two things. A
       refusal after the store is opened leaves the store
       exactly as it was. */
    bool ok = false;
    McmeBuffer encoded;
    mcme_buffer_init(&encoded);
    McmeArena *arena = NULL;
    McmeWriteRefusal why = MCME_WRITE_REFUSAL_INTERNAL;

    arena = mcme_arena_new();
    if (arena == NULL) {
        mcme_set_error("set block: out of memory");
        goto done;
    }

    /* A subchunk the world has not stored comes back all air, so
       this edit reaches into ground that was never saved rather
       than refusing to create it. */
    McmeBedrockSubchunk sc;
    if (!mcme_bedrock_subchunk_get_or_create(db, cx, cz, cy,
                                             dimension, arena,
                                             &sc, &why)) {
        char why_text[256];
        snprintf(why_text, sizeof why_text, "%s", mcme_last_error());
        mcme_set_error("set block: subchunk (%d, %d, %d): %s",
                       cx, cz, (int)cy, why_text);
        goto done;
    }

    if (!mcme_bedrock_subchunk_set(&sc, arena,
                                   mod16(x), mod16(y),
                                   mod16(z), block)) {
        char why_text[256];
        snprintf(why_text, sizeof why_text, "%s", mcme_last_error());
        mcme_set_error("set block: block (%d, %d, %d): %s",
                       x, y, z, why_text);
        why = MCME_WRITE_REFUSAL_INTERNAL;  /* mcme's own document */
        goto done;
    }

    if (!mcme_bedrock_subchunk_encode(&sc, &encoded)) {
        char why_text[256];
        snprintf(why_text, sizeof why_text, "%s", mcme_last_error());
        mcme_set_error("set block: subchunk (%d, %d, %d) does "
                       "not re-encode: %s", cx, cz, (int)cy,
                       why_text);
        why = MCME_WRITE_REFUSAL_NOT_READABLE;
        goto done;
    }

    if (!mcme_leveldb_put(db, key, key_len,
                          encoded.data, encoded.len)) {
        why = MCME_WRITE_REFUSAL_STORE;
        goto done;       /* the store's own error stands */
    }
    why = MCME_WRITE_REFUSAL_NONE;
    ok = true;

done:
    if (refusal != NULL) *refusal = why;
    mcme_buffer_free(&encoded);
    if (arena != NULL) mcme_arena_free(arena);
    mcme_leveldb_close(db);
    return ok;
}

static int32_t i32_min(int32_t a, int32_t b) { return a < b ? a : b; }
static int32_t i32_max(int32_t a, int32_t b) { return a > b ? a : b; }

/* The subchunk range a box touches, first to last inclusive.
   The corners are normalised here, so every caller sees one
   shape of range. */
static void box_subchunk_range(const McmeBox *box,
                               int32_t *cx1, int32_t *cy1, int32_t *cz1,
                               int32_t *cx2, int32_t *cy2, int32_t *cz2)
{
    *cx1 = floor_div16(i32_min(box->x1, box->x2));
    *cx2 = floor_div16(i32_max(box->x1, box->x2));
    *cy1 = floor_div16(i32_min(box->y1, box->y2));
    *cy2 = floor_div16(i32_max(box->y1, box->y2));
    *cz1 = floor_div16(i32_min(box->z1, box->z2));
    *cz2 = floor_div16(i32_max(box->z1, box->z2));
}

bool mcme_bedrock_fill_check(McmeLevelDb *db, const McmeBox *box,
                             int32_t dimension,
                             McmeWriteRefusal *refusal)
{
    if (refusal != NULL) *refusal = MCME_WRITE_REFUSAL_NONE;

    if (db == NULL || box == NULL) {
        if (refusal != NULL) *refusal = MCME_WRITE_REFUSAL_USAGE;
        mcme_set_error("fill check: null argument");
        return false;
    }

    int32_t cx1, cy1, cz1, cx2, cy2, cz2;
    box_subchunk_range(box, &cx1, &cy1, &cz1, &cx2, &cy2, &cz2);
    if (cy1 < INT8_MIN || cy2 > INT8_MAX) {
        if (refusal != NULL) *refusal = MCME_WRITE_REFUSAL_USAGE;
        mcme_set_error("fill check: the box spans subchunk Y %d "
                       "to %d, beyond the key's signed byte",
                       (int)cy1, (int)cy2);
        return false;
    }

    for (int32_t cy = cy1; cy <= cy2; ++cy) {
        for (int32_t cz = cz1; cz <= cz2; ++cz) {
            for (int32_t cx = cx1; cx <= cx2; ++cx) {
                uint8_t key[14];
                size_t key_len = 0;
                if (!mcme_bedrock_key_build(key, sizeof key, &key_len,
                                            cx, cz, dimension,
                                            MCME_BEDROCK_TAG_SUBCHUNK,
                                            true, (int8_t)cy)) {
                    if (refusal != NULL)
                        *refusal = MCME_WRITE_REFUSAL_USAGE;
                    return false;
                }
                if (!mcme_leveldb_has(db, key, key_len)) {
                    /* Absent: fill creates it, so "key not
                       found" is the answer rather than a
                       fault to remember. */
                    mcme_clear_error();
                    continue;
                }

                McmeBuffer payload;
                mcme_buffer_init(&payload);
                bool deleted = false;
                if (!mcme_leveldb_get(db, key, key_len, &payload,
                                      &deleted)) {
                    mcme_buffer_free(&payload);
                    if (refusal != NULL)
                        *refusal = MCME_WRITE_REFUSAL_STORE;
                    return false;   /* the store's own error */
                }
                if (deleted) {
                    mcme_buffer_free(&payload);
                    if (refusal != NULL)
                        *refusal = MCME_WRITE_REFUSAL_NOT_THERE;
                    mcme_set_error("fill check: the subchunk (%d, "
                                   "%d, %d) is deleted in this "
                                   "world", (int)cx, (int)cz,
                                   (int)cy);
                    return false;
                }

                McmeArena *arena = mcme_arena_new();
                if (arena == NULL) {
                    mcme_buffer_free(&payload);
                    if (refusal != NULL)
                        *refusal = MCME_WRITE_REFUSAL_INTERNAL;
                    mcme_set_error("fill check: out of memory");
                    return false;
                }
                McmeBedrockSubchunk sc;
                bool decodes =
                    mcme_bedrock_subchunk_decode(&sc, arena,
                                                 payload.data,
                                                 payload.len);
                if (decodes && sc.layer_count == 0) {
                    mcme_set_error("fill check: the subchunk (%d, "
                                   "%d, %d) is the all-air marker, "
                                   "which has no layer to fill",
                                   (int)cx, (int)cz, (int)cy);
                    decodes = false;
                }
                if (!decodes) {
                    char why[256];
                    snprintf(why, sizeof why, "%s",
                             mcme_last_error());
                    mcme_set_error("fill check: the subchunk (%d, "
                                   "%d, %d): %s", (int)cx, (int)cz,
                                   (int)cy, why);
                    if (refusal != NULL)
                        *refusal = MCME_WRITE_REFUSAL_NOT_READABLE;
                }
                mcme_arena_free(arena);
                mcme_buffer_free(&payload);
                if (!decodes) return false;
            }
        }
    }
    return true;
}

/* Sets every position of the box inside one subchunk — the box
   intersected with the subchunk, in the local coordinates
   subchunk_set takes. */
static bool fill_subchunk(McmeBedrockSubchunk *sc, McmeArena *arena,
                          const McmeBox *box, int32_t cx, int32_t cy,
                          int32_t cz, const McmeNbtDoc *entry)
{
    int32_t x1 = i32_min(box->x1, box->x2);
    int32_t x2 = i32_max(box->x1, box->x2);
    int32_t y1 = i32_min(box->y1, box->y2);
    int32_t y2 = i32_max(box->y1, box->y2);
    int32_t z1 = i32_min(box->z1, box->z2);
    int32_t z2 = i32_max(box->z1, box->z2);

    int32_t lx1 = i32_max(0, x1 - cx * 16);
    int32_t lx2 = i32_min(15, x2 - cx * 16);
    int32_t ly1 = i32_max(0, y1 - cy * 16);
    int32_t ly2 = i32_min(15, y2 - cy * 16);
    int32_t lz1 = i32_max(0, z1 - cz * 16);
    int32_t lz2 = i32_min(15, z2 - cz * 16);

    for (int32_t ly = ly1; ly <= ly2; ++ly) {
        for (int32_t lz = lz1; lz <= lz2; ++lz) {
            for (int32_t lx = lx1; lx <= lx2; ++lx) {
                if (!mcme_bedrock_subchunk_set(sc, arena, lx, ly, lz,
                                               entry)) {
                    return false;
                }
            }
        }
    }
    return true;
}

/*
 * Writes one subchunk of a fill: the one the world has stored,
 * or the all-air one it does not, filled across the box and put
 * back. `refusal` says which class of failure it was, and
 * mcme_last_error() names the subchunk.
 */
static bool fill_one(McmeLevelDb *db, const McmeBox *box,
                     int32_t cx, int32_t cy, int32_t cz,
                     int32_t dimension, const McmeNbtDoc *entry,
                     McmeWriteRefusal *refusal)
{
    uint8_t key[14];
    size_t key_len = 0;
    if (!mcme_bedrock_key_build(key, sizeof key, &key_len,
                                cx, cz, dimension,
                                MCME_BEDROCK_TAG_SUBCHUNK,
                                true, (int8_t)cy)) {
        if (refusal != NULL) *refusal = MCME_WRITE_REFUSAL_USAGE;
        return false;
    }

    /* A subchunk the world has not stored comes back all air, so
       the box may reach into ground that was never saved. */
    McmeArena *arena = mcme_arena_new();
    if (arena == NULL) {
        if (refusal != NULL) *refusal = MCME_WRITE_REFUSAL_INTERNAL;
        mcme_set_error("fill: out of memory");
        return false;
    }

    McmeBedrockSubchunk sc;
    McmeWriteRefusal why = MCME_WRITE_REFUSAL_INTERNAL;
    bool ok = mcme_bedrock_subchunk_get_or_create(db, cx, cz, cy,
                                                  dimension, arena,
                                                  &sc, &why);
    if (ok) {
        ok = fill_subchunk(&sc, arena, box, cx, cy, cz, entry);
        if (!ok) {
            /* The check accepted this subchunk a moment ago; the
               block mcme was handed, not the world, is what went
               wrong. */
            why = MCME_WRITE_REFUSAL_INTERNAL;
        }
    }

    McmeBuffer encoded;
    mcme_buffer_init(&encoded);
    if (ok) {
        ok = mcme_bedrock_subchunk_encode(&sc, &encoded);
        if (!ok) why = MCME_WRITE_REFUSAL_NOT_READABLE;
    }
    if (ok) {
        ok = mcme_leveldb_put(db, key, key_len,
                              encoded.data, encoded.len);
        if (!ok) why = MCME_WRITE_REFUSAL_STORE;
    }
    if (!ok) {
        char text[256];
        snprintf(text, sizeof text, "%s", mcme_last_error());
        mcme_set_error("fill: subchunk (%d, %d, %d): %s",
                       (int)cx, (int)cz, (int)cy, text);
    }

    mcme_buffer_free(&encoded);
    mcme_arena_free(arena);
    if (refusal != NULL) *refusal = why;
    return ok;
}

bool mcme_bedrock_fill(McmeLevelDb *db, const McmeBox *box,
                       int32_t dimension,
                       const McmeNbtDoc *entry,
                       McmeWriteRefusal *refusal)
{
    if (refusal != NULL) *refusal = MCME_WRITE_REFUSAL_NONE;

    McmeWriteRefusal why = MCME_WRITE_REFUSAL_INTERNAL;

    if (db == NULL || box == NULL || entry == NULL) {
        if (refusal != NULL) *refusal = MCME_WRITE_REFUSAL_USAGE;
        mcme_set_error("fill: null argument");
        return false;
    }
    if (!mcme_bedrock_fill_check(db, box, dimension, &why)) {
        if (refusal != NULL) *refusal = why;
        return false;       /* the check's own error stands */
    }

    int32_t cx1, cy1, cz1, cx2, cy2, cz2;
    box_subchunk_range(box, &cx1, &cy1, &cz1, &cx2, &cy2, &cz2);

    for (int32_t cy = cy1; cy <= cy2; ++cy) {
        for (int32_t cz = cz1; cz <= cz2; ++cz) {
            for (int32_t cx = cx1; cx <= cx2; ++cx) {
                if (!fill_one(db, box, cx, cy, cz, dimension, entry,
                              refusal)) {
                    return false;
                }
            }
        }
    }
    return true;
}

bool mcme_bedrock_block_spec(const McmeTag *entry, McmeBuffer *out)
{
    if (entry == NULL || out == NULL) {
        mcme_set_error("block spec: null argument");
        return false;
    }
    const McmeTag *name = mcme_nbt_path(entry, "name");
    if (name == NULL || name->type != MCME_TAG_String) {
        mcme_set_error("block spec: the palette entry has no name");
        return false;
    }
    if (!mcme_buffer_append(out, name->string.data, name->string.len)) {
        return false;
    }
    const McmeTag *states = mcme_nbt_path(entry, "states");
    if (states != NULL && states->type == MCME_TAG_Compound
            && states->compound.len > 0) {
        return mcme_nbt_write_snbt(states, out, 0);
    }
    return true;
}

/* Adds `count` to the tally row for `block`, appending a row when
   the block is new. The array roots in `arena` and grows only by
   copying: the number of distinct blocks a world holds is small
   compared to the number of positions. */
static bool tally_add(McmeArena *arena, McmeBlockCount **tally,
                      size_t *len, size_t *cap,
                      const char *block, int64_t count)
{
    for (size_t i = 0; i < *len; ++i) {
        if (strcmp((*tally)[i].block, block) == 0) {
            (*tally)[i].count += count;
            return true;
        }
    }
    if (*len == *cap) {
        size_t new_cap = *cap == 0 ? 16 : *cap * 2;
        McmeBlockCount *grown =
            mcme_arena_alloc(arena, new_cap * sizeof *grown);
        if (grown == NULL) return false;
        if (*tally != NULL) {
            memcpy(grown, *tally, *len * sizeof *grown);
        }
        *tally = grown;
        *cap = new_cap;
    }
    (*tally)[*len].block = block;
    (*tally)[*len].count = count;
    *len += 1;
    return true;
}

/* Most frequent first; ties by spec, so the order is total and
   two runs over one world agree. */
static int count_cmp(const void *pa, const void *pb)
{
    const McmeBlockCount *a = pa;
    const McmeBlockCount *b = pb;
    if (a->count != b->count) {
        return a->count > b->count ? -1 : 1;
    }
    return strcmp(a->block, b->block);
}

bool mcme_bedrock_count_box(McmeLevelDb *db, const McmeBox *box,
                            int32_t dimension, McmeArena *arena,
                            McmeBlockCount **out, size_t *out_len,
                            int64_t *missing)
{
    if (db == NULL || box == NULL || arena == NULL || out == NULL
            || out_len == NULL || missing == NULL) {
        mcme_set_error("count box: null argument");
        return false;
    }
    *out = NULL;
    *out_len = 0;
    *missing = 0;

    int32_t x1 = i32_min(box->x1, box->x2);
    int32_t x2 = i32_max(box->x1, box->x2);
    int32_t y1 = i32_min(box->y1, box->y2);
    int32_t y2 = i32_max(box->y1, box->y2);
    int32_t z1 = i32_min(box->z1, box->z2);
    int32_t z2 = i32_max(box->z1, box->z2);
    int32_t cx1, cy1, cz1, cx2, cy2, cz2;
    box_subchunk_range(box, &cx1, &cy1, &cz1, &cx2, &cy2, &cz2);
    if (cy1 < INT8_MIN || cy2 > INT8_MAX) {
        mcme_set_error("count box: the box spans subchunk Y %d to "
                       "%d, beyond the key's signed byte",
                       (int)cy1, (int)cy2);
        return false;
    }

    size_t tally_len = 0, tally_cap = 0;
    McmeBlockCount *tally = NULL;   /* arena-rooted */
    bool ok = true;

    for (int32_t cy = cy1; ok && cy <= cy2; ++cy) {
        for (int32_t cz = cz1; ok && cz <= cz2; ++cz) {
            for (int32_t cx = cx1; ok && cx <= cx2; ++cx) {
                /* The box intersected with this subchunk. */
                int32_t lx1 = i32_max(0, x1 - cx * 16);
                int32_t lx2 = i32_min(15, x2 - cx * 16);
                int32_t ly1 = i32_max(0, y1 - cy * 16);
                int32_t ly2 = i32_min(15, y2 - cy * 16);
                int32_t lz1 = i32_max(0, z1 - cz * 16);
                int32_t lz2 = i32_min(15, z2 - cz * 16);
                int64_t positions = (int64_t)(lx2 - lx1 + 1)
                                  * (ly2 - ly1 + 1)
                                  * (lz2 - lz1 + 1);

                uint8_t key[14];
                size_t key_len = 0;
                if (!mcme_bedrock_key_build(key, sizeof key, &key_len,
                                            cx, cz, dimension,
                                            MCME_BEDROCK_TAG_SUBCHUNK,
                                            true, (int8_t)cy)) {
                    ok = false;
                    break;
                }

                McmeBuffer payload;
                mcme_buffer_init(&payload);
                bool deleted = false;
                bool got = mcme_leveldb_get(db, key, key_len, &payload,
                                            &deleted);
                if (!got || deleted) {
                    /* Absent, or a tombstone: empty, and the
                       caller is told how much of the box that
                       was. */
                    mcme_buffer_free(&payload);
                    *missing += 1;
                    ok = tally_add(arena, &tally, &tally_len,
                                   &tally_cap, "minecraft:air",
                                   positions);
                    continue;
                }

                McmeArena *sub = mcme_arena_new();
                McmeBedrockSubchunk sc;
                bool dec = sub != NULL
                        && mcme_bedrock_subchunk_decode(&sc, sub,
                                                        payload.data,
                                                        payload.len);
                mcme_buffer_free(&payload);
                if (!dec) {
                    char why[256];
                    snprintf(why, sizeof why, "%s", mcme_last_error());
                    mcme_set_error("count box: subchunk (%d, %d, %d) "
                                   "does not decode: %s", (int)cx,
                                   (int)cz, (int)cy, why);
                    ok = false;
                } else if (sc.layer_count == 0) {
                    /* The all-air marker: stored, empty. */
                    ok = tally_add(arena, &tally, &tally_len,
                                   &tally_cap, "minecraft:air",
                                   positions);
                } else {
                    const McmeBedrockLayer *layer = &sc.layers[0];
                    size_t counts_bytes =
                        layer->palette_len * sizeof(int64_t);
                    int64_t *counts =
                        mcme_arena_alloc(sub, counts_bytes);
                    ok = counts != NULL;
                    if (ok) {
                        memset(counts, 0, counts_bytes);
                        for (int32_t ly = ly1; ok && ly <= ly2; ++ly) {
                            for (int32_t lz = lz1;
                                 ok && lz <= lz2; ++lz) {
                                for (int32_t lx = lx1;
                                     ok && lx <= lx2; ++lx) {
                                    size_t u = (size_t)lx * 256
                                        + (size_t)lz * 16
                                        + (size_t)ly;
                                    uint16_t idx = layer->indices[u];
                                    if (idx >= layer->palette_len) {
                                        int bx = (int)(cx * 16 + lx);
                                        int by = (int)(cy * 16 + ly);
                                        int bz = (int)(cz * 16 + lz);
                                        mcme_set_error("count box: the "
                                                       "index at block "
                                                       "(%d, %d, %d) is "
                                                       "%u, outside its "
                                                       "palette of %zu "
                                                       "entries", bx, by,
                                                       bz, idx,
                                                       layer->palette_len);
                                        ok = false;
                                        break;
                                    }
                                    counts[idx] += 1;
                                }
                            }
                        }
                    }
                    for (size_t j = 0;
                         ok && j < layer->palette_len; ++j) {
                        if (counts[j] == 0) continue;
                        const McmeNbtDoc *pal = &layer->palette[j];
                        McmeBuffer spec;
                        mcme_buffer_init(&spec);
                        ok = mcme_bedrock_block_spec(pal->root, &spec);
                        if (ok) {
                            char *text = mcme_arena_alloc(arena,
                                                          spec.len + 1);
                            ok = text != NULL;
                            if (ok) {
                                memcpy(text, spec.data, spec.len);
                                text[spec.len] = '\0';
                                ok = tally_add(arena, &tally,
                                               &tally_len, &tally_cap,
                                               text, counts[j]);
                            }
                        }
                        mcme_buffer_free(&spec);
                    }
                }
                if (sub != NULL) mcme_arena_free(sub);
            }
        }
    }
    if (!ok) return false;

    qsort(tally, tally_len, sizeof *tally, count_cmp);
    *out = tally;
    *out_len = tally_len;
    return true;
}

/* ========================================================================
 * Column height — the read that answers "how high is the ground here"
 * ======================================================================== */

/* A string compared to a literal by length first: the parser's
   strings are arena-owned and the comparison must not depend on
   a terminator that the format never promised. */
static bool str_is(const McmeStr *s, const char *lit)
{
    size_t n = strlen(lit);
    return s->len == n && memcmp(s->data, lit, n) == 0;
}

/* The names the game writes for empty space. Anything else is
   ground for this report — the fluid blocks included, since a
   pool has a surface to stand on. */
static bool entry_is_air(const McmeNbtDoc *pal)
{
    if (pal == NULL || pal->root == NULL) return false;
    const McmeTag *name = mcme_nbt_path(pal->root, "name");
    if (name == NULL || name->type != MCME_TAG_String) return false;
    return str_is(&name->string, "minecraft:air")
        || str_is(&name->string, "minecraft:cave_air")
        || str_is(&name->string, "minecraft:void_air");
}

/* One palette entry rendered as the spec text the CLI speaks,
   rooted in `arena`. Fails with an error already saying which
   of the two things went wrong: the entry, or the memory. */
static const char *spec_in_arena(McmeArena *arena,
                                 const McmeNbtDoc *pal)
{
    McmeBuffer spec;
    mcme_buffer_init(&spec);
    if (!mcme_bedrock_block_spec(pal->root, &spec)) {
        mcme_buffer_free(&spec);
        return NULL;
    }
    char *text = mcme_arena_alloc(arena, spec.len + 1);
    if (text == NULL) {
        if (mcme_last_error()[0] == '\0') {
            mcme_set_error("column height: out of memory");
        }
    } else {
        if (spec.len > 0) memcpy(text, spec.data, spec.len);
        text[spec.len] = '\0';
    }
    mcme_buffer_free(&spec);
    return text;
}

/* One more stored subchunk in the column's list, growing the arena
   array the list lives in. */
static bool column_push(McmeArena *arena, int8_t **cys,
                        size_t *count, size_t *cap, int8_t cy)
{
    if (*count == *cap) {
        size_t grown_cap = *cap == 0 ? 24 : *cap * 2;
        int8_t *grown = mcme_arena_alloc(arena,
                                         grown_cap * sizeof *grown);
        if (grown == NULL) return false;
        if (*cys != NULL) memcpy(grown, *cys, *count);
        *cys = grown;
        *cap = grown_cap;
    }
    (*cys)[(*count)++] = cy;
    return true;
}

bool mcme_bedrock_column_height(McmeLevelDb *db, int32_t x, int32_t z,
                                int32_t dimension, McmeArena *arena,
                                McmeColumnHeight *out)
{
    if (db == NULL || arena == NULL || out == NULL) {
        mcme_set_error("column height: null argument");
        return false;
    }
    memset(out, 0, sizeof *out);

    int32_t cx = floor_div16(x);
    int32_t cz = floor_div16(z);
    int32_t lx = mod16(x);
    int32_t lz = mod16(z);

    /* Which subchunks the store holds here, and what they contain,
       in one pass. A key says everything about a subchunk but its
       Y, so the column is found by asking for every Y the key byte
       can name — point lookups, where scanning the store would
       read every key in the world. The modern prefix is asked
       first; a world that predates it answers under the old
       spelling.

       has() is the presence test and get() the content, because
       they disagree on purpose: has() answers for a key the store
       still has — including one the game deleted — and only get()
       can tell a deletion from an absence, which is the difference
       between ground the world removed and ground it never had. */
    int8_t *cys = NULL;
    size_t count = 0, cap = 0;
    uint8_t tag = MCME_BEDROCK_TAG_SUBCHUNK;

    /* The palette entries the two answers came from, borrowed from
       the arena every payload below decodes into — so the specs
       are rendered once, from the last block found. */
    const McmeNbtDoc *floor_pal = NULL;
    const McmeNbtDoc *surface_pal = NULL;

    for (int pass = 0; pass < 2 && count == 0; ++pass) {
        tag = pass == 0 ? MCME_BEDROCK_TAG_SUBCHUNK
                        : MCME_BEDROCK_TAG_SUBCHUNK_OLD;
        for (int cy = INT8_MIN; cy <= INT8_MAX; ++cy) {
            uint8_t key[14];
            size_t key_len = 0;
            if (!mcme_bedrock_key_build(key, sizeof key, &key_len,
                                        cx, cz, dimension, tag,
                                        true, (int8_t)cy)) {
                return false;  /* cannot happen: cy is the byte */
            }
            if (!mcme_leveldb_has(db, key, key_len)) continue;

            McmeBuffer payload;
            mcme_buffer_init(&payload);
            bool deleted = false;
            if (!mcme_leveldb_get(db, key, key_len, &payload,
                                  &deleted)) {
                mcme_buffer_free(&payload);
                return false;       /* the store's own error stands */
            }
            if (deleted) {
                mcme_buffer_free(&payload);
                continue;   /* a tombstone: removed, so not stored */
            }

            McmeBedrockSubchunk sc;
            bool dec = mcme_bedrock_subchunk_decode(&sc, arena,
                                                    payload.data,
                                                    payload.len);
            mcme_buffer_free(&payload);
            if (!dec) {
                mcme_set_error("column height: subchunk (%d, %d, "
                               "%d) does not decode: %s", (int)cx,
                               (int)cz, (int)cy, mcme_last_error());
                return false;
            }

            if (!column_push(arena, &cys, &count, &cap,
                             (int8_t)cy)) {
                mcme_set_error("column height: out of memory");
                return false;
            }

            if (sc.layer_count == 0) continue; /* the all-air marker */

            const McmeBedrockLayer *layer = &sc.layers[0];
            for (int32_t ly = 0; ly < 16; ++ly) {
                size_t u = (size_t)lx * 256 + (size_t)lz * 16
                         + (size_t)ly;
                uint16_t idx = layer->indices[u];
                if (idx >= layer->palette_len) {
                    mcme_set_error("column height: the index at "
                                   "block (%d, %d, %d) is %u, "
                                   "outside its palette of %zu "
                                   "entries", (int)x,
                                   (int)(cy * 16 + ly), (int)z,
                                   idx, layer->palette_len);
                    return false;
                }
                const McmeNbtDoc *pal = &layer->palette[idx];
                if (entry_is_air(pal)) continue;
                if (floor_pal == NULL) {
                    floor_pal = pal; /* lowest so far, and last */
                    out->has_floor = true;
                    out->floor_y = cy * 16 + ly;
                }
                surface_pal = pal;   /* highest wins */
                out->has_surface = true;
                out->surface_y = cy * 16 + ly;
            }
        }
    }

    out->subchunks = cys;
    out->subchunk_count = count;
    if (floor_pal != NULL) {
        out->floor_block = spec_in_arena(arena, floor_pal);
        if (out->floor_block == NULL) return false;
    }
    if (surface_pal != NULL) {
        out->surface_block = spec_in_arena(arena, surface_pal);
        if (out->surface_block == NULL) return false;
    }
    return true;
}
