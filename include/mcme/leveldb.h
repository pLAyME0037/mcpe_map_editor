/*
 * mcme LevelDB, in C.
 *
 * Minecraft Bedrock stores every world in a LevelDB key/value store under
 * `db/`. Nothing packaged LevelDB on the build machine, so the on-disk format
 * is implemented here rather than linked. See decisions.md#D9 for why, and for
 * the conditions under which that reverses.
 *
 * This header is the whole surface. Everything else under
 * src/world/leveldb/ is private to the implementation, so swapping in the real
 * library later means rewriting one directory.
 *
 * ---------------------------------------------------------------------------
 * What is implemented, and what is deliberately not
 * ---------------------------------------------------------------------------
 *
 * READ ONLY, and that is not an accident. A read-only store that refuses to
 * write cannot corrupt a user's world; a half-implemented writer can. Writing
 * lands after reads are proven, and the intended shape is appending log
 * records — LevelDB replays a log on open — rather than implementing
 * compaction.
 *
 * Supported: log records (FULL, FIRST, MIDDLE, LAST) with per-fragment
 * checksums; SSTable data blocks with prefix compression and restart points;
 * index blocks; the footer; the metaindex; block checksum verification; block
 * decompression for the codecs Minecraft actually writes; and reads of the
 * CURRENT file and MANIFEST version edits. Both table file spellings (.ldb and
 * .sst) are the same format and both are read.
 *
 * Not supported, each of which makes an affected DB fail loudly rather than
 * silently return wrong data:
 *   - snappy-compressed blocks (trailer type 1). Every block states its own
 *     compression type in a five-byte trailer after its contents, so this is
 *     read directly rather than inferred: an unsupported block is refused by
 *     type byte and offset. Minecraft's Bedrock fork writes bare DEFLATE
 *     (type 4) instead, and that IS implemented — without it no real Bedrock
 *     world would open at all.
 *   - table files with a filter other than bloom. Filters are never read — they
 *     accelerate point lookups and this reader only scans — but an unknown one
 *     marks a store this reader was not built against.
 *   - comparators other than "leveldb.BytewiseComparator". The merge assumes
 *     user-key-ascending then sequence-descending, which is only the truth for
 *     the default comparator.
 *
 * ---------------------------------------------------------------------------
 * Key layout (Bedrock's, not LevelDB's)
 * ---------------------------------------------------------------------------
 *
 * LevelDB sees opaque byte strings. Minecraft puts meaning in them:
 *
 *   x (4 LE)  z (4 LE)  [dimension (4 LE)]  tag (1)  [subchunk index]
 *
 * The dimension field is OMITTED when it is 0, so overworld chunk keys are 9
 * bytes and other dimensions 13. That is mcme's business and not this
 * module's: this module hands back the raw key.
 */

#ifndef MCME_LEVELDB_H
#define MCME_LEVELDB_H

#include "mcme/core.h"

/* One live entry. `key` and `val` point into memory owned by the iterator's
   caller-visible buffer and stay valid until the callback returns. `deleted`
   is true for a tombstone: the key exists in the store as a deletion, which
   callers iterating a merged view should normally skip. */
typedef struct {
    const uint8_t *key;
    size_t         key_len;
    const uint8_t *val;
    size_t         val_len;
    bool           deleted;
    uint64_t       sequence;
} McmeLevelEntry;

typedef struct McmeLevelDb McmeLevelDb;

/*
 * Opens a store read-only. `dir` is the db/ directory itself, containing
 * CURRENT, MANIFEST-* and the table and log files.
 *
 * Never writes, creates or locks anything. A LOCK file is ignored.
 *
 * On failure *out is set to NULL and mcme_last_error() explains why. A store
 * that exists but uses something unsupported says so; a store that is not a
 * store says so.
 */
bool mcme_leveldb_open(McmeLevelDb **out, const char *dir);
void mcme_leveldb_close(McmeLevelDb *db);

/*
 * Walks every entry in ascending key order, calling `fn` once per live key.
 *
 * Merging is done properly: the log, every level-0 table, and every deeper
 * level are merged by internal key, which is (user key ascending, sequence
 * descending). Duplicate user keys therefore surface newest-first, and only
 * the newest is reported. A tombstone suppresses older copies and is not
 * reported at all.
 *
 * Returning false from `fn` stops the walk early; the count returned is then
 * how many entries were delivered. The full count is also available from
 * mcme_leveldb_count().
 */
typedef bool (*McmeLevelEntryFn)(void *userdata, const McmeLevelEntry *entry);

size_t mcme_leveldb_iterate(McmeLevelDb *db, McmeLevelEntryFn fn, void *userdata);

/* Number of live entries, as mcme_leveldb_iterate would report them. */
size_t mcme_leveldb_count(McmeLevelDb *db);

/*
 * Fetches one key by exact match. On success *out holds the value (appended)
 * and *out_deleted reports whether it was a tombstone; a deleted key is a
 * success with out_deleted set and nothing appended.
 *
 * Returns false when the key is absent, and mcme_leveldb_has() distinguishes
 * that from a real failure.
 */
bool mcme_leveldb_get(McmeLevelDb *db, const void *key, size_t key_len,
                      McmeBuffer *out, bool *out_deleted);

/*
 * Is `key` present in the store? A tombstone counts as present: the store does
 * know the key, it just no longer has a value for it. Use get() to tell "was
 * deleted" from "never existed", and iterate()/count() for the live view only.
 *
 * Separate from get() so "absent" and "broken" are not collapsed into one
 * false.
 */
bool mcme_leveldb_has(McmeLevelDb *db, const void *key, size_t key_len);

/* Counts of the underlying files, for `mcme info` and diagnostics. */
size_t mcme_leveldb_table_count(const McmeLevelDb *db);
size_t mcme_leveldb_log_count(const McmeLevelDb *db);

/* The comparator name the MANIFEST recorded, e.g. "leveldb.BytewiseComparator".
   Borrowed; valid until close. NULL if the MANIFEST was unreadable. */
const char *mcme_leveldb_comparator(const McmeLevelDb *db);

#endif /* MCME_LEVELDB_H */
