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
 * READS FIRST, then log-append writes, then compaction, and the order is not
 * an accident. A read-only store that refuses to write cannot corrupt a user's
 * world; a half-implemented writer can. Reads were proven against the Python
 * binding before a single write existed, and the writer appends log records
 * only — LevelDB replays a log on open, so a store built that way is correct
 * for every reader without this code having to implement compaction. See
 * decisions.md#D9. Compaction (flushing the log into a table) arrived last,
 * because a log that only grows is a store that eventually refuses to open;
 * see decisions.md#D11.
 *
 * Supported for reading: log records (FULL, FIRST, MIDDLE, LAST) with
 * per-fragment checksums; SSTable data blocks with prefix compression and
 * restart points; index blocks; the footer; the metaindex; block checksum
 * verification; block decompression for the codecs Minecraft actually writes;
 * and reads of the CURRENT file and MANIFEST version edits. Both table file
 * spellings (.ldb and .sst) are the same format and both are read.
 *
 * Supported for writing: whole WriteBatches as single log records, put and
 * delete as one-entry batches, sequence assignment from the store's
 * high-water mark, an exclusive LOCK while the handle is open, and compaction
 * — rewriting the merged view as one new level-0 table and retiring the old
 * tables and logs. After each write the merged view is rebuilt, so a writable
 * handle reads its own writes and nothing else in the process can observe a
 * half-applied batch. Writes past MCME_LDB_LOG_COMPACT_AT compact first, so a
 * long editing session does not depend on anyone remembering to flush.
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
 *   - rotating the log as a separate operation. The log is retired by
 *     compaction instead: rotation without a flush would orphan every record
 *     in the old log, because readers trust the MANIFEST's log number. A log
 *     that grows past MCME_LDB_LOG_WRITE_MAX with compaction failing (disk
 *     full, permissions) is refused with an error rather than grown into a
 *     store only this session could read.
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

/*
 * Opens a store for writing. Same view as mcme_leveldb_open, plus an exclusive
 * flock on LOCK that is held until close — the lock Minecraft takes, so this
 * fails rather than editing a world the game has open. Creates the LOCK file
 * if it does not exist.
 *
 * Only one writable handle per store should exist; the lock enforces it.
 */
bool mcme_leveldb_open_rw(McmeLevelDb **out, const char *dir);
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

/* ========================================================================
 * Writing
 * ======================================================================== */

/* One operation in a batch. `val`/`val_len` are ignored when `deleted`. */
typedef struct {
    const void *key;
    size_t      key_len;
    const void *val;
    size_t      val_len;
    bool        deleted;
} McmeLevelOp;

/*
 * Applies `n` operations as one atomic WriteBatch, appended to the store's log
 * and made visible to this handle before returning.
 *
 * Atomic means what it does in LevelDB: one log record, so a reader that stops
 * at a torn tail drops the whole batch, never half of it. Within a batch,
 * later entries sort newer, so overwriting one key twice yields the last.
 *
 * Refuses a store opened read-only, an empty batch, and a NULL key. Returns
 * false with mcme_last_error() explaining which. After a false return the
 * handle is still usable unless the error says the view could not be
 * rebuilt — in that case every subsequent read and write refuses, because the
 * store on disk has the write and this handle's view does not.
 */
bool mcme_leveldb_write(McmeLevelDb *db, const McmeLevelOp *ops, size_t n);

/* One-entry batch conveniences. An empty value is legal and distinct from a
   deletion; a deletion of a key that does not exist is also legal and is
   recorded, exactly as stock LevelDB records it. */
bool mcme_leveldb_put(McmeLevelDb *db, const void *key, size_t key_len,
                      const void *val, size_t val_len);
bool mcme_leveldb_delete(McmeLevelDb *db, const void *key, size_t key_len);

/*
 * Rewrites the store's whole merged view as one new SSTable and retires the
 * old tables and logs: the flush that a growing log needs to stay openable.
 *
 * The view does not change. Every live entry goes into the new table under its
 * original internal key and sequence number — tombstones included, so has()
 * and get(deleted) answer the same before and after. What changes is the shape:
 * N tables plus a log become one table, and mcme_leveldb_log_count() goes to
 * zero until the next write.
 *
 * Crash-safe by ordering, not by hope: the new table is fsync'd, then the
 * MANIFEST edit is appended and fsync'd, and only then are the old files
 * deleted. A crash anywhere leaves a store a reader accepts — an unreferenced
 * table before the edit, unreferenced leftovers after it — never a MANIFEST
 * naming bytes that might not have reached the disk.
 *
 * Refuses a NULL or read-only handle, and a handle whose view could not be
 * rebuilt after an earlier write (reopen it first). On success the handle's
 * view is current; on failure before the MANIFEST edit the store is untouched,
 * and a failure after it leaves the store consistent on disk but this handle
 * marked broken — reopen it.
 */
bool mcme_leveldb_compact(McmeLevelDb *db);

#endif /* MCME_LEVELDB_H */
