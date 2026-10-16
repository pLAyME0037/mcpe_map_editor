#include "mcme/bedrock_entity.h"

#include "mcme/bedrock_keys.h"

#include "stb_ds.h"

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

/* The record's key for one chunk: no subchunk index, because the
   record holds the whole chunk's entity list at once. The tag says
   which list — 0x31 the block entities, 0x32 the entities (D23). */
static bool record_key(int32_t cx, int32_t cz, int32_t dimension,
                       uint8_t tag,
                       uint8_t *key, size_t cap, size_t *key_len)
{
    return mcme_bedrock_key_build(key, cap, key_len, cx, cz,
                                  dimension, tag, false, 0);
}

/* Where one document of a record sits, and the document itself. */
typedef struct {
    bool       found;
    size_t     at;      /* where the document starts          */
    size_t     len;     /* how many bytes it occupies          */
    McmeNbtDoc doc;     /* the document, when found            */
} McmeEntityHit;

/* One document of a record, wherever it sits: the walk's answer. */
typedef struct {
    size_t     at;      /* where the document starts          */
    size_t     len;     /* how many bytes it occupies          */
    McmeNbtDoc doc;
} McmeEntityDoc;

/*
 * Walks a record's documents, all of them.
 *
 * The record is one NBT document per entity concatenated with no
 * framing — the next starts where the last finished, the same shape
 * as a block palette — so the walk is parse-prefix until the buffer
 * is empty.
 *
 * Every document must decode, not just the ones a caller cares
 * about: half a record is not something mcme answers for, and it is
 * not something the write half rewrites either, so a read that
 * stopped early would disagree with a write that splices.
 *
 * Returns false with mcme_last_error() naming the chunk when any
 * part will not read, prefixed with `label` ("block entity", "entity")
 * so a failing record says which list of the chunk it was.
 * `*out` receives the documents as an stb array
 * the caller frees, empty when the walk fails.
 */
static bool record_walk(const uint8_t *data, size_t len,
                        int32_t cx, int32_t cz, const char *label,
                        McmeArena *arena, McmeEntityDoc **out)
{
    *out = NULL;
    size_t at = 0;
    while (at < len) {
        McmeNbtDoc doc;
        size_t consumed = 0;
        if (!mcme_nbt_parse_prefix(&doc, arena, data + at, len - at,
                                   true, &consumed)
                || consumed == 0) {
            /* No progress would loop forever, and a document that
               will not parse is world data mcme cannot read. */
            char why[256];
            snprintf(why, sizeof why, "%s", mcme_last_error());
            mcme_set_error("%s: the record of chunk (%d, "
                           "%d) does not decode: %s", label, (int)cx,
                           (int)cz, why);
            arrfree(*out);
            *out = NULL;
            return false;
        }
        McmeEntityDoc d;
        d.at = at;
        d.len = consumed;
        d.doc = doc;
        arrpush(*out, d);
        at += consumed;
    }
    return true;
}

/*
 * Walks a record, finding the document at this block.
 *
 * `hit` says where the match is; `found` stays false when the record
 * decodes but holds nothing at that block.
 */
static bool record_scan(const uint8_t *data, size_t len,
                        int32_t x, int32_t y, int32_t z,
                        int32_t cx, int32_t cz, McmeArena *arena,
                        McmeEntityHit *hit)
{
    hit->found = false;
    /* An absent document's `at` is where it would sit: the record's
       end. The splice reads `at` as the insertion point, so a put of
       a new block entity appends to the record's other documents
       instead of replacing all of them. */
    hit->at = len;
    hit->len = 0;
    memset(&hit->doc, 0, sizeof hit->doc);

    McmeEntityDoc *docs = NULL;
    bool ok = record_walk(data, len, cx, cz, "block entity", arena,
                          &docs);
    if (ok) {
        for (size_t i = 0; i < (size_t)arrlen(docs); ++i) {
            if (at_position(docs[i].doc.root, x, y, z)) {
                hit->found = true;
                hit->at = docs[i].at;
                hit->len = docs[i].len;
                hit->doc = docs[i].doc;
                break;
            }
        }
    }
    arrfree(docs);
    return ok;
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

    int32_t cx = mcme_bedrock_chunk_of(x);
    int32_t cz = mcme_bedrock_chunk_of(z);
    uint8_t key[14];
    size_t key_len = 0;
    if (!record_key(cx, cz, dimension, MCME_BEDROCK_TAG_BLOCK_ENTITY,
                    key, sizeof key, &key_len)) {
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

    McmeEntityHit hit;
    McmeEntityStatus status = MCME_ENTITY_ABSENT;
    if (!record_scan(payload.data, payload.len, x, y, z, cx, cz,
                     arena, &hit)) {
        status = MCME_ENTITY_NOT_READABLE;
    } else if (hit.found) {
        *out = hit.doc;
        status = MCME_ENTITY_OK;
    }

    mcme_buffer_free(&payload);
    return status;
}

/* The whole-record read both chunk readers share: the key of the
   record's tag, one get, every document decoded or the record
   unreadable — the D9 rule, kept whole. `label` names the list in
   every error the read can produce, so a failing record says
   which of the chunk's two lists would not read. */
static McmeEntityStatus chunk_record_read(McmeLevelDb *db,
                                          int32_t cx,
                                          int32_t cz,
                                          int32_t dimension,
                                          uint8_t tag,
                                          const char *label,
                                          McmeArena *arena,
                                          McmeNbtDoc **docs,
                                          size_t *count)
{
    if (db == NULL || arena == NULL || docs == NULL || count == NULL) {
        mcme_set_error("%s: null argument", label);
        return MCME_ENTITY_USAGE;
    }
    *docs = NULL;
    *count = 0;

    uint8_t key[14];
    size_t key_len = 0;
    if (!record_key(cx, cz, dimension, tag, key, sizeof key, &key_len)) {
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

    McmeEntityDoc *walked = NULL;
    McmeEntityStatus status = MCME_ENTITY_OK;
    if (!record_walk(payload.data, payload.len, cx, cz, label, arena,
                     &walked)) {
        status = MCME_ENTITY_NOT_READABLE;
    } else {
        size_t n = (size_t)arrlen(walked);
        McmeNbtDoc *out = n > 0
            ? (McmeNbtDoc *)mcme_arena_alloc(arena,
                                             n * sizeof *out)
            : NULL;
        if (n > 0 && out == NULL) {
            status = MCME_ENTITY_INTERNAL;
        } else {
            for (size_t i = 0; i < n; ++i) out[i] = walked[i].doc;
            *docs = out;
            *count = n;
        }
    }
    arrfree(walked);
    mcme_buffer_free(&payload);
    return status;
}

McmeEntityStatus mcme_bedrock_chunk_block_entities(McmeLevelDb *db,
                                                   int32_t cx,
                                                   int32_t cz,
                                                   int32_t dimension,
                                                   McmeArena *arena,
                                                   McmeNbtDoc **docs,
                                                   size_t *count)
{
    return chunk_record_read(db, cx, cz, dimension,
                             MCME_BEDROCK_TAG_BLOCK_ENTITY,
                             "block entity", arena, docs, count);
}

McmeEntityStatus mcme_bedrock_chunk_entities(McmeLevelDb *db,
                                             int32_t cx,
                                             int32_t cz,
                                             int32_t dimension,
                                             McmeArena *arena,
                                             McmeNbtDoc **docs,
                                             size_t *count)
{
    return chunk_record_read(db, cx, cz, dimension,
                             MCME_BEDROCK_TAG_ENTITIES,
                             "entity", arena, docs, count);
}

/* Sets one coordinate field of a document: the position is the
   argument, so the document's own numbers are made to agree with it
   rather than the other way round. A document that named another
   block would be stored where nothing finds it. */
static bool put_coord(McmeArena *arena, McmeTag *entity,
                      const char *key, int32_t v)
{
    McmeTag *t = mcme_nbt_new(arena, MCME_TAG_Int);
    if (t == NULL) return false;
    if (!mcme_nbt_set_int(t, v)) return false;
    return mcme_nbt_compound_put(arena, entity, key, 1, t);
}

/* The id field in either spelling the game has: the bare savegame
   id — "Chest", what all 43 distinct ids in one real store are —
   or the namespaced one — "minecraft:chest", what mcme's earlier
   writes carry. What is refused is neither spelling: no id, an
   empty side of the colon, a second colon, or a character no id
   in either form holds. The shape, not the list: the registry of
   real ids is the game's and versioned, and mcme does not have
   it. */
static bool put_id_is_spelled(const char *s, size_t len)
{
    if (len == 0) return false;
    size_t colons = 0;
    for (size_t i = 0; i < len; ++i) {
        char c = s[i];
        if (c == ':') {
            if (++colons > 1) return false;
            continue;
        }
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
                || (c >= '0' && c <= '9')
                || c == '_' || c == '-' || c == '.'))
            return false;
    }
    return !(colons == 1 && (s[0] == ':' || s[len - 1] == ':'));
}

bool mcme_bedrock_put_block_entity(const char *db_dir,
                                   int32_t x, int32_t y,
                                   int32_t z,
                                   int32_t dimension,
                                   McmeTag *entity,
                                   McmeArena *arena,
                                   McmeWriteRefusal *refusal)
{
    if (refusal != NULL) *refusal = MCME_WRITE_REFUSAL_NONE;

    if (db_dir == NULL || entity == NULL || arena == NULL) {
        if (refusal != NULL) *refusal = MCME_WRITE_REFUSAL_USAGE;
        mcme_set_error("put block entity: null argument");
        return false;
    }
    if (entity->type != MCME_TAG_Compound) {
        if (refusal != NULL) *refusal = MCME_WRITE_REFUSAL_USAGE;
        mcme_set_error("put block entity: an entity document must "
                       "be a compound, not a %s",
                       mcme_tag_type_name(entity->type));
        return false;
    }

    /* An id in one of the two spellings, stored as given: the
       game's own records are bare savegame ids and mcme's earlier
       writes are namespaced, and both read back — so what is
       refused is only an id neither form could be. */
    const McmeTag *id = mcme_nbt_get(entity, "id");
    if (id == NULL || id->type != MCME_TAG_String
            || !put_id_is_spelled(id->string.data, id->string.len)) {
        if (refusal != NULL) *refusal = MCME_WRITE_REFUSAL_USAGE;
        mcme_set_error("put block entity: an entity needs an \"id\" "
                       "naming a savegame id, like \"Chest\" or "
                       "\"minecraft:chest\"");
        return false;
    }

    if (!put_coord(arena, entity, "x", x)
            || !put_coord(arena, entity, "y", y)
            || !put_coord(arena, entity, "z", z)) {
        if (refusal != NULL) *refusal = MCME_WRITE_REFUSAL_INTERNAL;
        mcme_set_error("put block entity: out of memory");
        return false;
    }

    int32_t cx = mcme_bedrock_chunk_of(x);
    int32_t cz = mcme_bedrock_chunk_of(z);
    uint8_t key[14];
    size_t key_len = 0;
    if (!record_key(cx, cz, dimension, MCME_BEDROCK_TAG_BLOCK_ENTITY,
                    key, sizeof key, &key_len)) {
        if (refusal != NULL) *refusal = MCME_WRITE_REFUSAL_USAGE;
        return false;
    }

    bool ok = false;
    McmeWriteRefusal why = MCME_WRITE_REFUSAL_INTERNAL;
    McmeBuffer encoded, payload, out;
    mcme_buffer_init(&encoded);
    mcme_buffer_init(&payload);
    mcme_buffer_init(&out);
    McmeLevelDb *db = NULL;

    /* Encode before the store is touched: a document that will not
       encode has written nothing. The game gives a block entity no
       root name, so neither does this. */
    McmeNbtDoc doc;
    doc.arena = arena;
    doc.root = entity;
    doc.root_name.data = (char *)"";
    doc.root_name.len = 0;
    doc.little_endian = true;
    if (!mcme_nbt_write_binary(&doc, &encoded)) {
        why = MCME_WRITE_REFUSAL_INTERNAL;
        goto done;                  /* the encoder set the error */
    }

    if (!mcme_leveldb_open_rw(&db, db_dir)) {
        why = MCME_WRITE_REFUSAL_STORE;
        goto done;
    }

    bool deleted = false;
    if (!mcme_leveldb_get(db, key, key_len, &payload, &deleted)) {
        if (mcme_leveldb_has(db, key, key_len)) {
            /* Known to the store but unreadable: a failure, not
               an absence. */
            why = MCME_WRITE_REFUSAL_STORE;
            goto done;
        }
        /* Absent — the record is created, the way chunk set
           creates a subchunk the world never stored. A record the
           game deleted counts the same: adding an entity to a
           chunk that has none is the operation, not a
           resurrection of anything. */
    }

    /* The whole record must decode before any of it is rewritten,
       and its other documents go back as the bytes they arrived
       as: changing one chest cannot reorder the sign beside it. */
    McmeEntityHit hit;
    if (!record_scan(payload.data, payload.len, x, y, z, cx, cz,
                     arena, &hit)) {
        why = MCME_WRITE_REFUSAL_NOT_READABLE;
        goto done;
    }

    size_t tail = hit.found ? hit.at + hit.len : payload.len;
    bool grew = true;
    if (hit.at > 0) {
        grew = mcme_buffer_append(&out, payload.data, hit.at);
    }
    if (grew) {
        grew = mcme_buffer_append(&out, encoded.data, encoded.len);
    }
    if (grew && payload.len > tail) {
        grew = mcme_buffer_append(&out, payload.data + tail,
                                  payload.len - tail);
    }
    if (!grew) {
        why = MCME_WRITE_REFUSAL_INTERNAL;
        mcme_set_error("put block entity: out of memory");
        goto done;
    }

    if (!mcme_leveldb_put(db, key, key_len, out.data, out.len)) {
        why = MCME_WRITE_REFUSAL_STORE;
        goto done;
    }
    ok = true;

done:
    if (!ok && refusal != NULL) *refusal = why;
    mcme_buffer_free(&out);
    mcme_buffer_free(&payload);
    mcme_buffer_free(&encoded);
    if (db != NULL) mcme_leveldb_close(db);
    return ok;
}

bool mcme_bedrock_del_block_entity(const char *db_dir,
                                   int32_t x, int32_t y,
                                   int32_t z,
                                   int32_t dimension,
                                   McmeWriteRefusal *refusal)
{
    if (refusal != NULL) *refusal = MCME_WRITE_REFUSAL_NONE;

    if (db_dir == NULL) {
        if (refusal != NULL) *refusal = MCME_WRITE_REFUSAL_USAGE;
        mcme_set_error("del block entity: null argument");
        return false;
    }

    int32_t cx = mcme_bedrock_chunk_of(x);
    int32_t cz = mcme_bedrock_chunk_of(z);
    uint8_t key[14];
    size_t key_len = 0;
    if (!record_key(cx, cz, dimension, MCME_BEDROCK_TAG_BLOCK_ENTITY,
                    key, sizeof key, &key_len)) {
        if (refusal != NULL) *refusal = MCME_WRITE_REFUSAL_USAGE;
        return false;
    }

    McmeArena *arena = mcme_arena_new();
    if (arena == NULL) {
        if (refusal != NULL) *refusal = MCME_WRITE_REFUSAL_INTERNAL;
        mcme_set_error("del block entity: out of memory");
        return false;
    }

    bool ok = false;
    McmeWriteRefusal why = MCME_WRITE_REFUSAL_INTERNAL;
    McmeBuffer payload, out;
    mcme_buffer_init(&payload);
    mcme_buffer_init(&out);
    McmeLevelDb *db = NULL;

    if (!mcme_leveldb_open_rw(&db, db_dir)) {
        why = MCME_WRITE_REFUSAL_STORE;
        goto done;
    }

    bool deleted = false;
    if (!mcme_leveldb_get(db, key, key_len, &payload, &deleted)) {
        why = mcme_leveldb_has(db, key, key_len)
            ? MCME_WRITE_REFUSAL_STORE
            : MCME_WRITE_REFUSAL_NOT_THERE;
        goto done;
    }
    if (deleted) {
        why = MCME_WRITE_REFUSAL_NOT_THERE;
        goto done;
    }

    McmeEntityHit hit;
    if (!record_scan(payload.data, payload.len, x, y, z, cx, cz,
                     arena, &hit)) {
        why = MCME_WRITE_REFUSAL_NOT_READABLE;
        goto done;
    }
    if (!hit.found) {
        why = MCME_WRITE_REFUSAL_NOT_THERE;
        goto done;
    }

    /* The document's bytes, and only those, leave the record. */
    bool grew = true;
    if (hit.at > 0) {
        grew = mcme_buffer_append(&out, payload.data, hit.at);
    }
    size_t tail = hit.at + hit.len;
    if (grew && payload.len > tail) {
        grew = mcme_buffer_append(&out, payload.data + tail,
                                  payload.len - tail);
    }
    if (!grew) {
        why = MCME_WRITE_REFUSAL_INTERNAL;
        mcme_set_error("del block entity: out of memory");
        goto done;
    }

    /* The last entity's record goes with it: mcme does not keep a
       record holding nothing, which is also what the game stores —
       no key at all. */
    if (out.len == 0) {
        if (!mcme_leveldb_delete(db, key, key_len)) {
            why = MCME_WRITE_REFUSAL_STORE;
            goto done;
        }
    } else if (!mcme_leveldb_put(db, key, key_len, out.data,
                                 out.len)) {
        why = MCME_WRITE_REFUSAL_STORE;
        goto done;
    }
    ok = true;

done:
    /* Every refusal says what was refused, not the error the store
       layer happened to leave behind: a missing record is "nothing
       there", not "key not found". */
    if (why == MCME_WRITE_REFUSAL_NOT_THERE) {
        mcme_set_error("del block entity: no block entity is stored "
                       "at block (%d, %d, %d) in dimension %d",
                       (int)x, (int)y, (int)z, (int)dimension);
    }
    if (!ok && refusal != NULL) *refusal = why;
    mcme_buffer_free(&out);
    mcme_buffer_free(&payload);
    if (db != NULL) mcme_leveldb_close(db);
    mcme_arena_free(arena);
    return ok;
}
