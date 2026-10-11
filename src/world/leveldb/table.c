/*
 * LevelDB SSTable reader (.ldb / .sst).
 *
 * Layout, from the front:
 *
 *   [data blocks][metaindex block][index block][footer, 48 bytes]
 *
 * A data block is a run of entries followed by an array of restart offsets
 * and a count:
 *
 *   entry  := shared_prefix (varint)  bytes shared with the previous key
 *             non_shared     (varint)  bytes of key that are new
 *             value_len      (varint)
 *             key_tail       (non_shared bytes)
 *             value          (value_len bytes)
 *   block  := entry*
 *             restart_offsets (uint32 LE each)
 *             num_restarts    (uint32 LE)
 *
 * Every restart-th entry begins with shared == 0 and is a seek target. That is
 * the whole reason the restart array exists, and this reader walks entries
 * strictly forward, so it never needs to binary-search it — but the count is
 * still read, because it is what says where the entry array ends.
 *
 * The index block holds one entry per data block: key is a separator, value is
 * the block's handle encoded as two varints (offset, size). Walking the index
 * in order therefore walks the table's keys in order, which is all this reader
 * needs to produce a full ordered scan.
 *
 * The metaindex block and the bloom filter behind it are NOT read. The filter
 * exists to accelerate seeks, and this reader only ever scans; reading it would
 * add a parser whose only consumer is an optimisation. The metaindex IS walked,
 * but only to reject a filter this reader cannot interpret — never to infer
 * anything about compression, which the block trailer states outright.
 */

#include "internal.h"

#include "mcme/compress.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "stb_ds.h"

/* The footer is fixed width: two block handles, padded, then the magic. */
#define LDB_FOOTER_SIZE 48

/* Table files are tens of megabytes at most for a Bedrock world; refuse
   anything larger rather than let a bogus size drive the allocation. */
#define LDB_TABLE_MAX ((size_t)512u * 1024u * 1024u)

/* ========================================================================
 * Block contents
 *
 * Every block on disk is its contents followed by a five-byte trailer that is
 * NOT counted in the block handle's size:
 *
 *   n bytes  block contents (entries, restart array, restart count)
 *   1 byte   compression type: 0 none, 1 snappy, 4 bare DEFLATE
 *   4 bytes  masked CRC32C of the contents and the type byte, little-endian
 *
 * A reader that treats the handle's size as the whole block reads the restart
 * array at the wrong offset and mis-detects the block's end. The trailer is
 * where this module checks integrity and where it decides whether the
 * contents need decompressing.
 *
 * Type 4 is not stock LevelDB. Minecraft's Bedrock fork compresses table
 * blocks with bare DEFLATE and numbers it 4, so a reader that implements only
 * the codecs in google/leveldb rejects every real Bedrock world — and rejects
 * it as "unsupported compression", which reads like an optional feature being
 * missing rather than the whole store.
 * ======================================================================== */

/* Compression types named by a block trailer. Minecraft's Bedrock LevelDB fork
   writes bare DEFLATE and labels it 4; stock LevelDB writes 1 for snappy,
   which is deliberately not implemented here. */
#define LDB_BLOCK_NO_COMPRESSION 0
#define LDB_BLOCK_RAW_DEFLATE    4
#define LDB_BLOCK_TRAILER 5

/*
 * Validates that [data, data+len) is a well-formed block, checking that the
 * restart array is present and that walking every entry lands exactly on it.
 *
 * This is a separate pass from iteration on purpose: a block whose entry array
 * overshoots would otherwise be a way to read past the end, and proving the
 * arithmetic once up front is cheaper than guarding every step.
 */
static bool block_is_wellformed(const uint8_t *data, size_t len)
{
    if (len < 4) return false;

    uint32_t num_restarts =
        (uint32_t)data[len - 4] | ((uint32_t)data[len - 3] << 8) |
        ((uint32_t)data[len - 2] << 16) | ((uint32_t)data[len - 1] << 24);
    if (num_restarts == 0) return false;

    size_t restart_bytes = (size_t)num_restarts * 4u;
    if (restart_bytes + 4 > len) return false;

    const uint8_t *entries_end = data + len - restart_bytes - 4;
    const uint8_t *p = data;

    while (p < entries_end) {
        uint64_t shared = 0, non_shared = 0, value_len = 0;
        size_t n;

        n = mcme_ldb_varint_get(p, entries_end, &shared);
        if (n == 0) return false;
        p += n;

        n = mcme_ldb_varint_get(p, entries_end, &non_shared);
        if (n == 0) return false;
        p += n;

        n = mcme_ldb_varint_get(p, entries_end, &value_len);
        if (n == 0) return false;
        p += n;

        if (non_shared > (uint64_t)(entries_end - p)) return false;
        p += non_shared;

        if (value_len > (uint64_t)(entries_end - p)) return false;
        p += value_len;
    }

    /* The walk must land exactly on the restart array. Landing short means the
       entries stopped early; running past was already rejected. */
    return p == entries_end;
}

/* Forward iterator over one block's entries. */
typedef struct {
    const uint8_t *data;
    size_t         len;
    const uint8_t *entries_end;
    const uint8_t *restarts;
    uint32_t       num_restarts;

    const uint8_t *at;
    uint8_t       *key;        /* rebuilt here; owned */
    size_t         key_cap;
    size_t         key_len;

    const uint8_t *val;
    size_t         val_len;

    /* Set when the walk stopped on damage rather than on the restart array.
       Both look identical from the outside — block_iter_next() just returns
       false — so without this a block whose entries run out early is silently
       truncated instead of reported, and a reader hands back a store that is
       missing keys with no complaint at all. */
    bool           failed;
} LdbBlockIter;

static void block_iter_init(LdbBlockIter *it, const uint8_t *data, size_t len)
{
    memset(it, 0, sizeof *it);
    it->data = data;
    it->len = len;
    it->num_restarts = (uint32_t)data[len - 4] | ((uint32_t)data[len - 3] << 8) |
                       ((uint32_t)data[len - 2] << 16) | ((uint32_t)data[len - 1] << 24);
    size_t restart_bytes = (size_t)it->num_restarts * 4u;
    it->restarts = data + len - restart_bytes - 4;
    it->entries_end = it->restarts;
    it->at = data;
}

static void block_iter_free(LdbBlockIter *it)
{
    free(it->key);
    it->key = NULL;
    it->key_cap = 0;
}

/* Rebuilds the full key from the previous one plus this entry's shared prefix
   and new tail. Returns false when the block is finished, and also sets
   it->failed when it returns false early on damage. */
static bool block_iter_next(LdbBlockIter *it)
{
    if (it->at >= it->entries_end) return false;

    const uint8_t *end = it->entries_end;
    const uint8_t *p = it->at;

    uint64_t shared = 0, non_shared = 0, value_len = 0;
    size_t n;

    n = mcme_ldb_varint_get(p, end, &shared);
    if (n == 0) { it->failed = true; return false; }
    p += n;
    n = mcme_ldb_varint_get(p, end, &non_shared);
    if (n == 0) { it->failed = true; return false; }
    p += n;
    n = mcme_ldb_varint_get(p, end, &value_len);
    if (n == 0) { it->failed = true; return false; }
    p += n;

    if (non_shared > (uint64_t)(end - p)) { it->failed = true; return false; }
    const uint8_t *tail = p;
    p += non_shared;
    if (value_len > (uint64_t)(end - p)) { it->failed = true; return false; }
    it->val = p;
    it->val_len = (size_t)value_len;

    /* The first entry of the block (shared == 0) needs no previous key; every
       later one extends it. Requiring shared <= key_len stops a corrupt prefix
       from claiming bytes we never stored. */
    if (shared > (uint64_t)it->key_len) { it->failed = true; return false; }

    size_t new_len = (size_t)shared + (size_t)non_shared;
    if (new_len + 1 > it->key_cap) {
        size_t cap = (new_len + 1) * 2;
        uint8_t *grown = (uint8_t *)realloc(it->key, cap);
        if (grown == NULL) {
            mcme_set_error("leveldb: out of memory rebuilding a key");
            it->failed = true;
            return false;
        }
        it->key = grown;
        it->key_cap = cap;
    }
    if (non_shared > 0) memcpy(it->key + shared, tail, (size_t)non_shared);
    it->key_len = new_len;
    it->key[it->key_len] = '\0';

    /* Step over the value as well as the key. Forgetting the value leaves the
       cursor inside it, so the next call parses value bytes as an entry
       header and stops after a single entry — which reads as an empty-looking
       table rather than as corruption. */
    it->at = p + value_len;
    return true;
}

/* ========================================================================
 * Table
 * ======================================================================== */

/* Records a decompressed block so table_close() can free it. Returns the
   pointer, or NULL with the error set. */
static uint8_t *own_block(McmeLdbTable *t, uint8_t *data)
{
    if (t->inflated_count == t->inflated_cap) {
        size_t next = t->inflated_cap ? t->inflated_cap * 2 : 8;
        uint8_t **grown = (uint8_t **)realloc(t->inflated, next * sizeof *grown);
        if (grown == NULL) return NULL;
        t->inflated = grown;
        t->inflated_cap = next;
    }
    t->inflated[t->inflated_count++] = data;
    return data;
}

static void free_inflated(McmeLdbTable *t)
{
    for (size_t i = 0; i < t->inflated_count; ++i) free(t->inflated[i]);
    free(t->inflated);
    t->inflated = NULL;
    t->inflated_count = 0;
    t->inflated_cap = 0;
}

/*
 * Reads one block, verifying its trailer and decompressing it if needed.
 *
 * `size` is the block handle's stored size, which counts the contents and NOT
 * the five-byte trailer. *out_len is the length of the returned contents,
 * which differs from `size` whenever the block was compressed — callers must
 * use it rather than the handle's size.
 *
 * For an uncompressed block the result points into the mapped file. For a
 * compressed one it points into a buffer this function allocates and records
 * on `t` for table_close() to free.
 */
static bool read_block(McmeLdbTable *t, size_t offset, size_t size,
                       const uint8_t **out, size_t *out_len)
{
    if (size == 0 || offset > t->size || size > t->size - offset) {
        mcme_set_error("leveldb: block at %zu size %zu is outside %s",
                       offset, size, t->path);
        return false;
    }

    /* The trailer sits after the block's contents and is not part of `size`,
       so a block that ends within five bytes of the file has been truncated. */
    if (LDB_BLOCK_TRAILER > t->size - offset - size) {
        mcme_set_error("leveldb: block at %zu in %s ends with a truncated trailer",
                       offset, t->path);
        return false;
    }

    const uint8_t *block = t->file + offset;
    uint8_t type = block[size];

    uint32_t stored = (uint32_t)block[size + 1] |
                      ((uint32_t)block[size + 2] << 8) |
                      ((uint32_t)block[size + 3] << 16) |
                      ((uint32_t)block[size + 4] << 24);

    /* The checksum covers the contents and then the type byte, chained so the
       whole thing is one CRC32C over their concatenation. Verified before the
       type is acted on, so a block whose type byte was altered is caught as
       corruption rather than fed to a decompressor. */
    uint32_t crc = mcme_ldb_crc32c(0, block, size);
    crc = mcme_ldb_crc32c(crc, &type, 1);
    if (mcme_ldb_mask_crc(crc) != stored) {
        mcme_set_error("leveldb: block at %zu in %s fails its checksum",
                       offset, t->path);
        return false;
    }

    if (type == LDB_BLOCK_NO_COMPRESSION) {
        *out = block;
        *out_len = size;
        return true;
    }

    if (type == LDB_BLOCK_RAW_DEFLATE) {
        /* Minecraft's Bedrock fork of LevelDB compresses table blocks with
           bare DEFLATE and calls it type 4. Nothing in the block states the
           decompressed size, so the output grows as it is produced. Getting
           this wrong is not a subtle loss of performance: every real Bedrock
           world compresses its blocks this way, so a reader implementing only
           stock LevelDB's codecs reads none of them. */
        McmeBuffer plain;
        mcme_buffer_init(&plain);

        if (!mcme_decompress(MCME_COMPRESS_RAW_DEFLATE, block, size,
                             &plain, LDB_TABLE_MAX)) {
            /* mcme_set_error and mcme_last_error share one buffer, so the
               inner message has to be copied out before it is overwritten. */
            char why[256];
            snprintf(why, sizeof why, "%s", mcme_last_error());
            mcme_set_error("leveldb: block at %zu in %s is raw DEFLATE but "
                           "does not decode: %s", offset, t->path, why);
            mcme_buffer_free(&plain);
            return false;
        }

        uint8_t *owned = own_block(t, plain.data);
        if (owned == NULL) {
            mcme_buffer_free(&plain);
            mcme_set_error("leveldb: out of memory reading a block of %s",
                           t->path);
            return false;
        }
        *out = owned;
        *out_len = plain.len;
        return true;
    }

    mcme_set_error("leveldb: block at %zu in %s uses compression type %u; "
                   "only uncompressed and raw-DEFLATE blocks are read",
                   offset, t->path, (unsigned)type);
    return false;
}

/* Reads a block handle: two varints, offset then size. */
static bool read_handle(const uint8_t **p, const uint8_t *end,
                        size_t *offset, size_t *size)
{
    uint64_t o = 0, s = 0;
    size_t n = mcme_ldb_varint_get(*p, end, &o);
    if (n == 0) return false;
    *p += n;
    n = mcme_ldb_varint_get(*p, end, &s);
    if (n == 0) return false;
    *p += n;

    *offset = (size_t)o;
    *size = (size_t)s;
    return true;
}

/*
 * Walks the metaindex and rejects a filter this reader does not know about.
 *
 * Filters are an acceleration for point lookups and this reader only scans, so
 * no filter is ever read — the check is not about performance. It is here
 * because a store carrying a filter this reader has never heard of is a store
 * built by something outside the tested shape, and saying so is more useful
 * than quietly reading past a block whose purpose we cannot account for.
 *
 * Compression is NOT inferred here. Every block states its own compression
 * type in its trailer, and read_block() reads that and decodes or refuses the
 * block by name — a direct answer rather than a guess from neighbouring
 * metadata.
 */
static bool table_check_meta(McmeLdbTable *t, size_t offset, size_t size)
{
    const uint8_t *block = NULL;
    size_t block_len = 0;
    if (!read_block(t, offset, size, &block, &block_len)) return false;
    if (!block_is_wellformed(block, block_len)) {
        mcme_set_error("leveldb: malformed metaindex block in %s", t->path);
        return false;
    }

    LdbBlockIter it;
    block_iter_init(&it, block, block_len);
    bool ok = true;

    while (ok && block_iter_next(&it)) {
        static const char kFilterPrefix[] = "filter.";
        if (it.key_len >= sizeof kFilterPrefix - 1 &&
            memcmp(it.key, kFilterPrefix, sizeof kFilterPrefix - 1) == 0) {
            /* Only the built-in bloom filter is understood. Anything else is
               a store written by something this reader was not built against. */
            static const char kBloom[] = "filter.leveldb.BuiltinBloomFilter2";
            if (it.key_len != sizeof kBloom - 1 ||
                memcmp(it.key, kBloom, sizeof kBloom - 1) != 0) {
                mcme_set_error("leveldb: %s uses unsupported filter '%.*s'",
                               t->path, (int)it.key_len, (const char *)it.key);
                ok = false;
            }
        }
    }

    block_iter_free(&it);
    if (it.failed) {
        mcme_set_error("leveldb: malformed metaindex entry in %s", t->path);
        ok = false;
    }
    return ok;
}

bool mcme_ldb_table_open(McmeLdbTable *t, const char *path)
{
    memset(t, 0, sizeof *t);

    if (!mcme_ldb_read_file(path, &t->file, &t->size, LDB_TABLE_MAX)) {
        return false;
    }
    t->path = strdup(path);
    if (t->path == NULL) {
        free(t->file);
        memset(t, 0, sizeof *t);
        mcme_set_error("leveldb: out of memory");
        return false;
    }

    if (t->size < LDB_FOOTER_SIZE) {
        mcme_set_error("leveldb: %s is %zu bytes, too small for a footer",
                       path, t->size);
        goto fail;
    }

    /* The footer sits at the very end and ends in a magic number, which is the
       quickest proof that this really is a table file. */
    const uint8_t *footer = t->file + t->size - LDB_FOOTER_SIZE;
    uint64_t magic = 0;
    for (int i = 0; i < 8; ++i) {
        magic |= (uint64_t)footer[LDB_FOOTER_SIZE - 8 + i] << (i * 8);
    }
    if (magic != MCME_LDB_MAGIC) {
        mcme_set_error("leveldb: %s does not end in the table magic "
                       "(got 0x%llx); is it really an .ldb file?",
                       path, (unsigned long long)magic);
        goto fail;
    }

    /* The two handles are the metaindex and the index, then padding, then the
       magic. Anything between the handles and the padding is ignored, which is
       what lets a future version add fields. */
    const uint8_t *p = footer;
    const uint8_t *limit = footer + LDB_FOOTER_SIZE - 8;
    size_t meta_off = 0, meta_size = 0, index_off = 0, index_size = 0;

    if (!read_handle(&p, limit, &meta_off, &meta_size) ||
        !read_handle(&p, limit, &index_off, &index_size)) {
        mcme_set_error("leveldb: %s has a truncated footer", path);
        goto fail;
    }

    if (!table_check_meta(t, meta_off, meta_size)) goto fail;

    /* The index block holds one entry per data block. Collect the handles so
       the scan is a simple loop over an array rather than a nested walk. */
    const uint8_t *index_block = NULL;
    size_t index_len = 0;
    if (!read_block(t, index_off, index_size, &index_block, &index_len)) goto fail;
    if (!block_is_wellformed(index_block, index_len)) {
        mcme_set_error("leveldb: malformed index block in %s", path);
        goto fail;
    }

    LdbBlockIter it;
    block_iter_init(&it, index_block, index_len);
    size_t cap = 0;

    while (block_iter_next(&it)) {
        const uint8_t *hp = it.val;
        const uint8_t *hend = it.val + it.val_len;
        size_t off = 0, sz = 0;
        if (!read_handle(&hp, hend, &off, &sz)) {
            mcme_set_error("leveldb: %s has an unreadable block handle", path);
            block_iter_free(&it);
            goto fail;
        }

        if (t->block_count == cap) {
            cap = cap ? cap * 2 : 64;
            void *grown = realloc(t->blocks, cap * sizeof *t->blocks);
            if (grown == NULL) {
                mcme_set_error("leveldb: out of memory indexing %s", path);
                block_iter_free(&it);
                goto fail;
            }
            t->blocks = grown;
        }
        t->blocks[t->block_count].offset = off;
        t->blocks[t->block_count].size = sz;
        t->block_count++;
    }
    if (it.failed) {
        mcme_set_error("leveldb: malformed index entry in %s", path);
        block_iter_free(&it);
        goto fail;
    }
    block_iter_free(&it);

    return true;

fail:
    free_inflated(t);
    free(t->path);
    free(t->file);
    free(t->blocks);
    memset(t, 0, sizeof *t);
    return false;
}

void mcme_ldb_table_close(McmeLdbTable *t)
{
    free_inflated(t);
    free(t->file);
    free(t->path);
    free(t->blocks);
    memset(t, 0, sizeof *t);
}

/*
 * Appends every entry in the table to the caller's three stb_ds arrays, in
 * ascending internal key order.
 *
 * Table order is already the right order for a single table, so no sorting
 * happens here. Merging tables, and applying log records on top of them, is
 * db.c's job.
 */
bool mcme_ldb_table_scan(McmeLdbTable *t,
                         uint8_t **keybuf, uint8_t **valbuf,
                         McmeLdbRawEntry **entries)
{
    for (size_t b = 0; b < t->block_count; ++b) {
        const uint8_t *block = NULL;
        size_t block_len = 0;
        if (!read_block(t, t->blocks[b].offset, t->blocks[b].size,
                        &block, &block_len)) {
            return false;
        }
        if (!block_is_wellformed(block, block_len)) {
            mcme_set_error("leveldb: malformed data block %zu in %s", b, t->path);
            return false;
        }

        LdbBlockIter it;
        block_iter_init(&it, block, block_len);
        while (block_iter_next(&it)) {
            McmeLdbRawEntry e;
            e.koff = arrlen(*keybuf);
            e.klen = it.key_len;
            e.voff = arrlen(*valbuf);
            e.vlen = it.val_len;

            /* Copy into the buffers rather than pointing into the file, so the
               entries outlive the table being closed. */
            for (size_t i = 0; i < it.key_len; ++i) arrput(*keybuf, it.key[i]);
            for (size_t i = 0; i < it.val_len; ++i) arrput(*valbuf, it.val[i]);

            arrput(*entries, e);
        }
        if (it.failed) {
            mcme_set_error("leveldb: malformed entry in data block %zu of %s",
                           b, t->path);
            block_iter_free(&it);
            return false;
        }
        block_iter_free(&it);
    }
    return true;
}

/* ========================================================================
 * Table writer
 *
 * The mirror of everything above, and the piece that lets a log be flushed
 * into a table so it can then be replaced — compaction rests on this
 * producing files that this directory's reader and stock LevelDB both accept.
 *
 * Uncompressed blocks (type 0). Every block states its own compression in
 * its trailer, so this is a valid table for every reader; compressing would
 * save disk at the cost of output the golden fixtures could no longer diff
 * against a byte-level description.
 *
 * Index keys are the full last key of each block rather than upstream's
 * shortest separator. A separator is a size optimisation; the full key is
 * always a valid separator, and there is no code here that could get the
 * shortening subtly wrong in a way that only surfaces in someone else's
 * index binary search.
 * ======================================================================== */

/* Upstream's constants: a 4 KiB target per data block, a restart point every
   16 entries. Minecraft's own fork uses ~100 KiB blocks; readers accept any
   size, and 4 KiB is what the rest of the ecosystem writes and tests. */
#define LDB_WRITE_BLOCK_TARGET ((size_t)4096)
#define LDB_WRITE_RESTART     16

typedef struct {
    McmeBuffer entries;      /* entry stream, prefix-compressed */
    McmeBuffer restarts;     /* uint32 LE offsets into `entries` */
    uint32_t   count;        /* entries since the last restart */
    bool       has_prev;     /* an entry has been added since init */
} LdbBlockBuilder;

static void block_builder_init(LdbBlockBuilder *b)
{
    mcme_buffer_init(&b->entries);
    mcme_buffer_init(&b->restarts);
    b->count = 0;
    b->has_prev = false;

    /* Upstream seeds the restart array with offset 0, so even an empty block
       carries one restart and block_is_wellformed() — which rejects a zero
       restart count — accepts it. That matters for the metaindex, which a
       filterless table writes empty. */
    static const uint8_t zero[4] = { 0, 0, 0, 0 };
    mcme_buffer_append(&b->restarts, zero, 4);
}

static void block_builder_free(LdbBlockBuilder *b)
{
    mcme_buffer_free(&b->entries);
    mcme_buffer_free(&b->restarts);
}

/*
 * Adds one entry. `prev` is the preceding key in the same block, or NULL for
 * the block's first entry — the shared prefix is measured against it, and a
 * restart entry always stores its whole key so a seek can land on it without
 * reconstructing anything.
 */
static bool block_builder_add(LdbBlockBuilder *b,
                              const uint8_t *prev, size_t prev_len,
                              const uint8_t *key, size_t klen,
                              const uint8_t *val, size_t vlen)
{
    size_t shared = 0;
    if (b->count >= LDB_WRITE_RESTART) {
        uint32_t off = (uint32_t)b->entries.len;
        uint8_t le[4] = { (uint8_t)off, (uint8_t)(off >> 8),
                          (uint8_t)(off >> 16), (uint8_t)(off >> 24) };
        if (!mcme_buffer_append(&b->restarts, le, 4)) return false;
        b->count = 0;
    } else if (b->has_prev && prev != NULL) {
        size_t n = prev_len < klen ? prev_len : klen;
        while (shared < n && prev[shared] == key[shared]) shared++;
    }

    if (!mcme_ldb_varint_put(&b->entries, shared) ||
        !mcme_ldb_varint_put(&b->entries, klen - shared) ||
        !mcme_ldb_varint_put(&b->entries, vlen) ||
        !mcme_buffer_append(&b->entries, key + shared, klen - shared) ||
        !mcme_buffer_append(&b->entries, val, vlen)) {
        return false;
    }

    b->count++;
    b->has_prev = true;
    return true;
}

/*
 * Appends the finished block to `out` and reports the handle size a caller
 * must record: the contents only. The five-byte trailer follows and is
 * excluded — bug #2 in decisions.md#D9, now written rather than only read.
 */
static bool block_builder_finish(LdbBlockBuilder *b, McmeBuffer *out,
                                 size_t *out_size)
{
    size_t start = out->len;

    if (!mcme_buffer_append(out, b->entries.data, b->entries.len) ||
        !mcme_buffer_append(out, b->restarts.data, b->restarts.len)) {
        return false;
    }

    uint32_t nr = (uint32_t)(b->restarts.len / 4);
    uint8_t le[4] = { (uint8_t)nr, (uint8_t)(nr >> 8),
                      (uint8_t)(nr >> 16), (uint8_t)(nr >> 24) };
    if (!mcme_buffer_append(out, le, 4)) return false;

    size_t size = out->len - start;

    /* Trailer: type byte, then the masked CRC32C over contents AND type —
       bug #3 from D9: the checksum covers what is on disk, not what it
       decompresses to. read_block() verifies exactly this. */
    uint8_t type = LDB_BLOCK_NO_COMPRESSION;
    uint32_t crc = mcme_ldb_crc32c(0, out->data + start, size);
    crc = mcme_ldb_crc32c(crc, &type, 1);
    crc = mcme_ldb_mask_crc(crc);
    uint8_t trailer[LDB_BLOCK_TRAILER] = {
        type, (uint8_t)crc, (uint8_t)(crc >> 8),
        (uint8_t)(crc >> 16), (uint8_t)(crc >> 24)
    };
    if (!mcme_buffer_append(out, trailer, sizeof trailer)) return false;

    *out_size = size;
    return true;
}

/* One index-block entry: the last key of a data block and its handle. */
typedef struct {
    const uint8_t *key;      /* points into the caller's entry array */
    size_t         klen;
    size_t         off, size;
} LdbIndexItem;

bool mcme_ldb_table_write(const char *path,
                          const McmeLdbTableEntry *entries, size_t n)
{
    if (path == NULL || (entries == NULL && n > 0)) {
        mcme_set_error("leveldb: NULL argument writing a table");
        return false;
    }

    McmeBuffer file;
    mcme_buffer_init(&file);

    LdbBlockBuilder block;
    block_builder_init(&block);

    LdbIndexItem *items = NULL;      /* stb_ds */
    bool ok = true;

    for (size_t i = 0; ok && i < n; ++i) {
        const uint8_t *prev = (i > 0) ? entries[i - 1].key : NULL;
        size_t prev_len = (i > 0) ? entries[i - 1].klen : 0;

        ok = block_builder_add(&block, prev, prev_len,
                               entries[i].key, entries[i].klen,
                               entries[i].val, entries[i].vlen);
        if (!ok) break;

        if (block.entries.len < LDB_WRITE_BLOCK_TARGET) continue;

        LdbIndexItem it;
        it.key = entries[i].key;     /* caller's storage outlives this call */
        it.klen = entries[i].klen;
        it.off = file.len;
        ok = block_builder_finish(&block, &file, &it.size);
        if (!ok) break;
        arrput(items, it);
        block_builder_free(&block);
        block_builder_init(&block);
    }

    /* The trailing half-full block, if any. */
    if (ok && block.has_prev) {
        LdbIndexItem it;
        it.key = entries[n - 1].key;
        it.klen = entries[n - 1].klen;
        it.off = file.len;
        ok = block_builder_finish(&block, &file, &it.size);
        if (ok) arrput(items, it);
    }

    /* Metaindex: an empty block. No filter is written, so none is claimed —
       table_check_meta() walks this and finds nothing to reject. */
    LdbBlockBuilder meta;
    block_builder_init(&meta);
    size_t meta_off = file.len, meta_size = 0;
    if (ok) ok = block_builder_finish(&meta, &file, &meta_size);
    block_builder_free(&meta);

    /* Index: one entry per data block, key = the block's last internal key,
       value = its handle as two varints (offset, contents size). Walking it
       in order walks the table's keys in order, which is what the reader's
       scan and stock LevelDB's seeks both rely on. */
    LdbBlockBuilder index;
    block_builder_init(&index);
    size_t index_off = file.len, index_size = 0;
    for (size_t i = 0; ok && i < (size_t)arrlen(items); ++i) {
        uint8_t handle[20];
        size_t hn = mcme_ldb_varint_encode(items[i].off, handle, sizeof handle);
        size_t k = hn == 0 ? 0
                 : mcme_ldb_varint_encode(items[i].size, handle + hn,
                                          sizeof handle - hn);
        if (hn == 0 || k == 0) { ok = false; break; }
        hn += k;

        const uint8_t *prev = (i > 0) ? items[i - 1].key : NULL;
        size_t prev_len = (i > 0) ? items[i - 1].klen : 0;
        ok = block_builder_add(&index, prev, prev_len,
                               items[i].key, items[i].klen, handle, hn);
    }
    if (ok) ok = block_builder_finish(&index, &file, &index_size);
    block_builder_free(&index);

    /* Footer: both handles, zero-padded to 40 bytes, then the magic. The
       reader checks the magic first — the quickest proof the file is a
       table at all. */
    if (ok) {
        McmeBuffer footer;
        mcme_buffer_init(&footer);
        ok = mcme_ldb_varint_put(&footer, meta_off) &&
             mcme_ldb_varint_put(&footer, meta_size) &&
             mcme_ldb_varint_put(&footer, index_off) &&
             mcme_ldb_varint_put(&footer, index_size);
        while (ok && footer.len < LDB_FOOTER_SIZE - 8) {
            ok = mcme_buffer_append_u8(&footer, 0);
        }
        for (int i = 0; ok && i < 8; ++i) {
            ok = mcme_buffer_append_u8(&footer,
                                       (uint8_t)((MCME_LDB_MAGIC >> (i * 8)) & 0xff));
        }
        if (ok) ok = mcme_buffer_append(&file, footer.data, footer.len);
        mcme_buffer_free(&footer);
    }

    /* The file appears under its final name as soon as this returns, and the
       MANIFEST edit that makes it live is the caller's job — which happens
       only after this succeeded. A crash mid-write therefore leaves a
       half-built table nothing trusts: readers open exactly the files the
       MANIFEST lists, and this one is not listed yet. A temporary-plus-rename
       would defend against a reader that scanned the directory for table
       files instead; there is no such reader here to defend against. */
    if (ok) ok = mcme_fs_write(path, file.data, file.len);

    block_builder_free(&block);
    arrfree(items);
    mcme_buffer_free(&file);
    return ok;
}
