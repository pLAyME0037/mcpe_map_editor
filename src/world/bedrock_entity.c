#include "mcme/bedrock_entity.h"

#include "mcme/bedrock_keys.h"

#include <stdio.h>
#include <string.h>

/* The integer at `key` of a compound, or "not this document": a
   block entity's coordinates are ints, and a document that gives
   the field another type is not one mcme can place. */
static bool coord_at(const McmeTag *root, const char *key,
                     int32_t *out)
{
    const McmeTag *t = mcme_nbt_path(root, key);
    if (t == NULL || t->type != MCME_TAG_Int) return false;
    *out = t->as_int;
    return true;
}

/* Whether a document is the entity of this block: the one whose own
   x, y and z say so. A document that carries no coordinates is not
   addressable by position — the reference's decoder drops it — so
   it is skipped, not matched and not refused. */
static bool at_position(const McmeTag *root, int32_t x, int32_t y,
                        int32_t z)
{
    if (root == NULL || root->type != MCME_TAG_Compound) return false;
    int32_t ex = 0, ey = 0, ez = 0;
    return coord_at(root, "x", &ex) && coord_at(root, "y", &ey)
        && coord_at(root, "z", &ez) && ex == x && ey == y
        && ez == z;
}

McmeEntityStatus mcme_bedrock_block_entity(McmeLevelDb *db,
                                           int32_t x, int32_t y,
                                           int32_t z,
                                           int32_t dimension,
                                           McmeArena *arena,
                                           McmeNbtDoc *out)
{
    if (db == NULL || arena == NULL || out == NULL) {
        mcme_set_error("block entity: null argument");
        return MCME_ENTITY_USAGE;
    }
    memset(out, 0, sizeof *out);

    /* The record is per chunk: one lookup names the whole list of
       entities the chunk holds, whatever block was asked for. */
    int32_t cx = mcme_bedrock_chunk_of(x);
    int32_t cz = mcme_bedrock_chunk_of(z);
    uint8_t key[14];
    size_t key_len = 0;
    if (!mcme_bedrock_key_build(key, sizeof key, &key_len, cx, cz,
                                dimension,
                                MCME_BEDROCK_TAG_BLOCK_ENTITY,
                                false, 0)) {
        return MCME_ENTITY_USAGE;
    }

    if (!mcme_leveldb_has(db, key, key_len)) {
        return MCME_ENTITY_ABSENT;
    }

    McmeBuffer payload;
    mcme_buffer_init(&payload);
    bool deleted = false;
    if (!mcme_leveldb_get(db, key, key_len, &payload, &deleted)) {
        mcme_buffer_free(&payload);
        return MCME_ENTITY_STORE;   /* the store's own error stands */
    }
    if (deleted) {
        mcme_buffer_free(&payload);
        return MCME_ENTITY_ABSENT;  /* removed, so nothing is there */
    }

    /* One document per entity, concatenated with no framing: each
       ends where the next begins, which is what the palette reader
       already parses this way. */
    McmeEntityStatus status = MCME_ENTITY_ABSENT;
    size_t at = 0;
    while (at < payload.len) {
        McmeNbtDoc doc;
        size_t consumed = 0;
        if (!mcme_nbt_parse_prefix(&doc, arena,
                                   payload.data + at,
                                   payload.len - at, true,
                                   &consumed)
                || consumed == 0) {
            /* No progress would loop forever; a document that will
               not parse is world data mcme cannot read. */
            char why[256];
            snprintf(why, sizeof why, "%s", mcme_last_error());
            mcme_set_error("block entity: the record of chunk "
                           "(%d, %d) does not decode: %s", (int)cx,
                           (int)cz, why);
            status = MCME_ENTITY_NOT_READABLE;
            break;
        }
        at += consumed;
        if (!at_position(doc.root, x, y, z)) continue;
        *out = doc;
        status = MCME_ENTITY_OK;
        break;
    }

    mcme_buffer_free(&payload);
    return status;
}
