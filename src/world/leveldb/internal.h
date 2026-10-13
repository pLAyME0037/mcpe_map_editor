/*
 * Private shared declarations for the LevelDB reader.
 *
 * Not installed, and not included from anywhere outside src/world/leveldb/.
 * The public surface is include/mcme/leveldb.h.
 */

#ifndef MCME_LEVELDB_INTERNAL_H
#define MCME_LEVELDB_INTERNAL_H

#include "mcme/core.h"
#include "mcme/leveldb.h"

#include <stdio.h>

/* LevelDB writes a log, a MANIFEST and table files in 32 KiB blocks. */
#define MCME_LDB_BLOCK_SIZE 32768u

/* The footer's trailing magic, little-endian. Present in every table file and
   the fastest way to recognise one. */
#define MCME_LDB_MAGIC 0xdb4775248b80fb57ULL

/* The footer is fixed width: two block handles, padded, then the magic. */
#define MCME_LDB_FOOTER_SIZE 48

/* ValueType, as stored in the low byte of an internal key's tag. */
typedef enum {
    MCME_LDB_DELETION = 0,
    MCME_LDB_VALUE    = 1,
} McmeLdbValueType;

/* ========================================================================
 * Varints
 *
 * LevelDB's own format: low 7 bits per byte, high bit set while more bytes
 * follow. Little-endian, so a short number stays short on disk.
 * ======================================================================== */

/*
 * Decodes one varint from [p, end). Returns the number of bytes consumed, or 0
 * if the encoding is truncated, overlong (past 10 bytes) or would overflow.
 *
 * A corrupt length field is the classic way to walk off the end of a buffer,
 * so this fails rather than clamping.
 */
size_t mcme_ldb_varint_get(const uint8_t *p, const uint8_t *end, uint64_t *out);

/* Appends `v` as a varint. Returns false only on a buffer allocation failure. */
bool mcme_ldb_varint_put(McmeBuffer *b, uint64_t v);

/* Encodes into a caller-supplied array. Returns the length, or 0 if `cap` is
   too small (a varint is never more than 10 bytes). */
size_t mcme_ldb_varint_encode(uint64_t v, uint8_t *out, size_t cap);

/* ========================================================================
 * CRC32C (Castagnoli)
 *
 * LevelDB checksums every log record and, optionally, every table block. The
 * polynomial is 0x1EDC6F41, reflected as 0x82F63B78.
 * ======================================================================== */

uint32_t mcme_ldb_crc32c(uint32_t seed, const void *data, size_t len);

/* LevelDB's masked CRC, used in log record headers:
     RotateRight(crc, 17) + 0xa282ead8
   The rotation keeps the checksum from being a linear function of the length
   field, which otherwise lets a corrupt length look valid. */
uint32_t mcme_ldb_mask_crc(uint32_t crc);

/* ========================================================================
 * Internal keys
 *
 * A table's key is "user key, then 8 bytes": the sequence number shifted left
 * by 8, or'd with the value type, stored little-endian. Comparison is bytewise
 * on the user key first, then by DECREASING sequence — which is what makes a
 * k-way merge surface the newest copy of a key first.
 * ======================================================================== */

/* Compares two internal keys. Result follows memcmp's convention. */
int mcme_ldb_internal_cmp(const void *a, size_t a_len,
                          const void *b, size_t b_len);

/* Compares the user-key halves of two internal keys. */
int mcme_ldb_user_cmp(const void *a, size_t a_len,
                      const void *b, size_t b_len);

/* Appends an internal key built from a user key and a sequence/type. */
bool mcme_ldb_put_internal_key(McmeBuffer *b, const void *user_key,
                               size_t user_len, uint64_t seq, uint8_t type);

/* ========================================================================
 * Files
 * ======================================================================== */

/* Reads a whole file into malloc'd storage, refusing anything over `max`. */
bool mcme_ldb_read_file(const char *path, uint8_t **out, size_t *out_len,
                        size_t max);

/* Directory join into a caller-supplied buffer. NULL if it would not fit. */
const char *mcme_ldb_path(char *buf, size_t cap, const char *dir,
                          const char *name);

/* Parses a trailing "NNNNNN" from a file name, e.g. "000007.log" -> 7.
   Returns false if the name has no such suffix. */
bool mcme_ldb_file_number(const char *name, uint64_t *out);

/* ========================================================================
 * Log files
 *
 * A MANIFEST is a log, so this reader serves both. See log.c for the record
 * framing and for why a short tail is not an error.
 * ======================================================================== */

/* Called once per complete, checksum-verified logical record. Returning false
   stops the walk. */
typedef bool (*McmeLdbRecordFn)(void *userdata,
                                const uint8_t *record, size_t len);

bool mcme_ldb_log_read(const uint8_t *data, size_t len,
                       McmeLdbRecordFn fn, void *ud, size_t *out_records);

/*
 * Appends one logical record to `fh`, fragmenting across 32 KiB blocks the
 * same way upstream's Writer does. `*block_offset` is the file size modulo the
 * block size — pass the size of what is already in the file so an append
 * resumes mid-block instead of stomping the block structure. The caller owns
 * flushing; this function does not fflush or fsync.
 */
bool mcme_ldb_log_append(FILE *fh, const void *record, size_t len,
                         size_t *block_offset);

/* ========================================================================
 * Tables (.ldb / .sst)
 * ======================================================================== */

/* One entry, as an offset pair into a shared key buffer and a shared value
   buffer. Storing offsets rather than pointers keeps the entry array compact
   and survives the buffers being realloc'd as they grow. */
typedef struct {
    size_t koff, klen;
    size_t voff, vlen;
} McmeLdbRawEntry;

/* A table is read, never loaded: `fp` stays open while the handle is open and
   read_block() seeks out one block at a time. The file's size is a property of
   the store, not a memory budget — compaction writes the whole store as one
   table, which for a real world runs to gigabytes, and a reader that slurped
   the file would refuse what this directory's own writer produced. */
typedef struct {
    FILE    *fp;
    size_t   size;
    char    *path;

    /* Block offsets, collected from the index block. */
    struct { size_t offset; size_t size; } *blocks;
    size_t block_count;
} McmeLdbTable;

bool mcme_ldb_table_open(McmeLdbTable *t, const char *path);
void mcme_ldb_table_close(McmeLdbTable *t);

/*
 * Appends every entry of the table, in ascending internal key order, to the
 * three stb_ds arrays. The caller owns all three afterwards.
 */
bool mcme_ldb_table_scan(McmeLdbTable *t,
                         uint8_t **keybuf, uint8_t **valbuf,
                         McmeLdbRawEntry **entries);

/* One entry as handed to the writer: an internal key and its value, with the
   pointers owned by the caller. Compaction passes its merged view in this
   shape; keys and values are stable for the duration of the call. */
typedef struct {
    const uint8_t *key;      /* internal key: user key + 8-byte tag */
    size_t         klen;
    const uint8_t *val;
    size_t         vlen;
} McmeLdbTableEntry;

/*
 * Writes `entries` as one SSTable to `path`, uncompressed blocks, and returns
 * with the file complete on disk (mcme_fs_write does the atomic rename).
 *
 * Entries must be in ascending internal-key order; the writer does not sort
 * and does not check. The resulting file is readable by this directory's
 * reader and by stock LevelDB — the latter verified against Minecraft's own
 * binding, not assumed.
 */
bool mcme_ldb_table_write(const char *path,
                          const McmeLdbTableEntry *entries, size_t n);

#endif /* MCME_LEVELDB_INTERNAL_H */
