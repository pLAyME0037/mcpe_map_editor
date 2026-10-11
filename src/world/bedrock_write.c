#include "mcme/bedrock_write.h"

#include "mcme/bedrock_chunk.h"
#include "mcme/bedrock_keys.h"
#include "mcme/leveldb.h"

#include <stdio.h>
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

bool mcme_bedrock_set_block(const char *db_dir,
                                  int32_t x, int32_t y, int32_t z,
                                  int32_t dimension,
                                  const McmeNbtDoc *block)
{
    if (db_dir == NULL || block == NULL) {
        mcme_set_error("set block: null argument");
        return false;
    }

    int32_t cx = floor_div16(x), cz = floor_div16(z);
    int32_t cy = floor_div16(y);
    if (cy < INT8_MIN || cy > INT8_MAX) {
        mcme_set_error("set block: subchunk Y %d does not "
                       "fit the key's signed byte", cy);
        return false;
    }

    uint8_t key[14];
    size_t key_len = 0;
    if (!mcme_bedrock_key_build(key, sizeof key, &key_len,
                                      cx, cz, dimension,
                                      MCME_BEDROCK_TAG_SUBCHUNK,
                                      true, (int8_t)cy)) {
        return false;
    }

    McmeLevelDb *db = NULL;
    if (!mcme_leveldb_open_rw(&db, db_dir)) return false;

    /* Every path from here frees the same four things. A
       refusal after the store is opened leaves the store
       exactly as it was. */
    bool ok = false;
    McmeBuffer payload;
    mcme_buffer_init(&payload);
    McmeBuffer encoded;
    mcme_buffer_init(&encoded);
    McmeArena *arena = NULL;

    if (!mcme_leveldb_has(db, key, key_len)) {
        mcme_set_error("set block: subchunk (%d, %d, %d) is "
                       "not stored in this world; creating "
                       "chunks is not supported",
                       cx, cz, (int)cy);
        goto done;
    }
    bool deleted = false;
    if (!mcme_leveldb_get(db, key, key_len, &payload, &deleted)) {
        goto done;       /* the store's own error stands */
    }
    if (deleted) {
        mcme_set_error("set block: subchunk (%d, %d, %d) is "
                       "deleted in this world", cx, cz, (int)cy);
        goto done;
    }

    arena = mcme_arena_new();
    if (arena == NULL) {
        mcme_set_error("set block: out of memory");
        goto done;
    }

    McmeBedrockSubchunk sc;
    if (!mcme_bedrock_subchunk_decode(&sc, arena,
                                           payload.data, payload.len)) {
        char why[256];
        snprintf(why, sizeof why, "%s", mcme_last_error());
        mcme_set_error("set block: subchunk (%d, %d, %d) does "
                       "not decode: %s", cx, cz, (int)cy, why);
        goto done;
    }

    if (!mcme_bedrock_subchunk_set(&sc, arena,
                                        mod16(x), mod16(y), mod16(z),
                                        block)) {
        char why[256];
        snprintf(why, sizeof why, "%s", mcme_last_error());
        mcme_set_error("set block: block (%d, %d, %d): %s",
                       x, y, z, why);
        goto done;
    }

    if (!mcme_bedrock_subchunk_encode(&sc, &encoded)) {
        char why[256];
        snprintf(why, sizeof why, "%s", mcme_last_error());
        mcme_set_error("set block: subchunk (%d, %d, %d) does "
                       "not re-encode: %s", cx, cz, (int)cy, why);
        goto done;
    }

    if (!mcme_leveldb_put(db, key, key_len,
                               encoded.data, encoded.len)) {
        goto done;       /* the store's own error stands */
    }
    ok = true;

done:
    mcme_buffer_free(&payload);
    mcme_buffer_free(&encoded);
    if (arena != NULL) mcme_arena_free(arena);
    mcme_leveldb_close(db);
    return ok;
}
