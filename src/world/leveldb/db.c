/*
 * LevelDB store: opening a db/ directory and reading everything out of it.
 *
 * A LevelDB store is:
 *
 *   CURRENT          one line of text naming the current MANIFEST
 *   MANIFEST-NNNNNN  a LOG file of VersionEdit records describing which table
 *                    files exist, at which level, and the store's options
 *   NNNNNN.log       recently written records not yet flushed to a table
 *   NNNNNN.ldb       SSTables, in order from newest (level 0) to oldest
 *   LOCK             advisory; ignored, because this reader never writes
 *
 * Reads have to consider all of those at once, because the newest copy of a key
 * is the one that counts and it may be in any of them.
 *
 * ---------------------------------------------------------------------------
 * How the merge is done, and why it is this blunt
 * ---------------------------------------------------------------------------
 *
 * Every entry from every source is collected, sorted by internal key, and then
 * deduplicated by user key. That is O(n log n) in the total entry count and
 * holds the whole store in memory.
 *
 * The alternative is a k-way merge over per-source iterators, which is what
 * LevelDB itself does and what a store measured in gigabytes would need. It is
 * also substantially more code, and it has to solve the awkward part of making
 * a log file yield entries in key order when logs are written in insertion
 * order — which means buffering and sorting a log anyway.
 *
 * Given that this reader exists to enumerate and fetch chunks, and that the
 * simplexity rules favour the obvious correct thing, collecting and sorting
 * wins today. The cost is bounded and explicit: mcme_leveldb_open refuses a
 * store with more than MCME_LDB_MAX_ENTRIES rather than swapping. If that
 * ceiling ever bites, the merge is the thing to replace, and nothing outside
 * this file needs to change.
 *
 * Duplicates across sources are harmless rather than a special case: the same
 * record appearing in both an old log and the table it was flushed into is
 * byte-identical, so sorting by (user key, sequence descending) and keeping the
 * first hit yields the same answer either way.
 */

#include "internal.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

#include "stb_ds.h"

/* Ceiling on collected entries. Roughly 160 bytes each once the sort array is
   counted, so 8 million entries is around 1.3 GiB — a real ceiling, and one
   better hit as an error than as the OOM killer. */
#define MCME_LDB_MAX_ENTRIES ((size_t)8000000u)

/* A MANIFEST or a log larger than this is not a store we are willing to read. */
#define MCME_LDB_LOG_MAX ((size_t)64u * 1024u * 1024u)

/* VersionEdit field tags. These are LevelDB's own numbering from
   db/version_edit.h, and the two file tags are easy to transpose:
     1 comparator, 2 log number, 3 next file number, 4 last sequence,
     5 compact pointer, 6 deleted file, 7 new file, 9 previous log number.
   Tag 8 was briefly used for large-value refs and is now unknown. */
#define LDB_TAG_COMPARATOR     1
#define LDB_TAG_LOG_NUMBER     2
#define LDB_TAG_NEXT_FILE      3
#define LDB_TAG_LAST_SEQUENCE  4
#define LDB_TAG_COMPACT_POINTER 5
#define LDB_TAG_DELETED_FILE   6
#define LDB_TAG_NEW_FILE       7
#define LDB_TAG_PREV_LOG       9

/* One merged, deduplicated entry. `key` points into db->keybuf, which is
   frozen once open() finishes, so these pointers stay valid for the store's
   lifetime. */
typedef struct {
    const uint8_t *key;      /* internal key: user key + 8-byte tag */
    size_t         klen;
    size_t         voff, vlen;
    bool           deleted;
    uint64_t       sequence;
} LiveEntry;

/* One live table as the MANIFEST describes it: its file number and the level
   it lives at. The level is not needed to read — a reader that scans every
   live table in sequence order gets the right answer without it — but a writer
   that compacts must delete files at the level stock LevelDB recorded them at,
   because recovery rejects an edit that deletes a file from the wrong level,
   and the level is the only way to name which copy of a file number an edit
   means. */
typedef struct {
    uint64_t level;
    uint64_t number;
} LdbTableRef;

struct McmeLevelDb {
    char    *dir;
    char    *comparator;

    McmeLdbRawEntry *raw;    /* stb_ds: what the sources contributed, unsorted */
    uint8_t *keybuf;         /* stb_ds */
    uint8_t *valbuf;         /* stb_ds */

    LiveEntry *live;         /* stb_ds: sorted, deduplicated, points into keybuf */
    size_t     table_count;
    size_t     log_count;

    LdbTableRef *tables;     /* stb_ds: the MANIFEST's live set, with levels */
    uint64_t     next_file_number; /* from the MANIFEST; compaction mints new
                                      numbers above it and everything on disk */

    /* Writable-handle state. Zeroed and unused for a read-only open, which is
       still the common case: nothing outside the edit path writes, and a
       handle that cannot write cannot corrupt anything by being misused. */
    bool     writable;
    bool     broken;         /* a write landed but the view could not be
                                rebuilt; every read now refuses rather than
                                answering from a view missing that write */
    int      lock_fd;        /* exclusive flock on LOCK, -1 when unheld */
    char    *manifest_name;  /* e.g. "MANIFEST-000001"                    */
    FILE    *log;            /* the append handle for log_number          */
    size_t   log_size;       /* bytes in it, for the block offset and for
                                refusing a log the reader could no longer
                                open                                        */
    uint64_t log_number;     /* the log file this handle appends to       */
    uint64_t last_sequence;  /* max of the MANIFEST's and the data's      */
};

/* stb_ds's arrlen is ptrdiff_t. Comparing it against a size_t is a sign-compare
   warning under -Werror, and casting at every call site is noise, so this is
   the one place the conversion happens. */
#define LDB_LEN(a) ((size_t)arrlen(a))

/* ========================================================================
 * MANIFEST: VersionEdit records
 * ======================================================================== */

/* Reads a length-prefixed string (varint length, then bytes). */
static bool read_length_prefixed(const uint8_t **p, const uint8_t *end,
                                 const uint8_t **out, size_t *out_len)
{
    uint64_t n = 0;
    size_t used = mcme_ldb_varint_get(*p, end, &n);
    if (used == 0) return false;
    *p += used;
    if (n > (uint64_t)(end - *p)) return false;
    *out = *p;
    *out_len = (size_t)n;
    *p += (size_t)n;
    return true;
}

typedef struct {
    char         **comparator;
    LdbTableRef   *tables;      /* stb_ds, in MANIFEST order */
    uint64_t       log_number;  /* last one wins, as the MANIFEST is a sequence
                                   of edits rather than a single document */
    uint64_t       next_file_number;
    uint64_t       last_sequence;
} ManifestCtx;

/*
 * Parses one VersionEdit record.
 *
 * Keeps the comparator name and the live table set (with levels, adding on
 * NEW_FILE and removing on DELETED_FILE — the set is what a compaction has to
 * know it is replacing). Everything else is walked past, because the record
 * still has to be traversed correctly to reach the next one.
 */
static bool parse_version_edit(const uint8_t *data, size_t len, ManifestCtx *ctx)
{
    const uint8_t *p = data;
    const uint8_t *end = data + len;

    while (p < end) {
        uint64_t tag = 0;
        size_t n = mcme_ldb_varint_get(p, end, &tag);
        if (n == 0) {
            mcme_set_error("leveldb: unreadable tag in MANIFEST");
            return false;
        }
        p += n;

        switch (tag) {
        case LDB_TAG_COMPARATOR: {
            const uint8_t *s = NULL;
            size_t slen = 0;
            if (!read_length_prefixed(&p, end, &s, &slen)) {
                mcme_set_error("leveldb: truncated comparator name in MANIFEST");
                return false;
            }
            free(*ctx->comparator);
            *ctx->comparator = (char *)malloc(slen + 1);
            if (*ctx->comparator == NULL) {
                mcme_set_error("leveldb: out of memory");
                return false;
            }
            memcpy(*ctx->comparator, s, slen);
            (*ctx->comparator)[slen] = '\0';
            break;
        }

        case LDB_TAG_LOG_NUMBER: {
            uint64_t v = 0;
            n = mcme_ldb_varint_get(p, end, &v);
            if (n == 0) {
                mcme_set_error("leveldb: truncated number in MANIFEST");
                return false;
            }
            p += n;
            ctx->log_number = v;
            break;
        }

        case LDB_TAG_NEXT_FILE: {
            uint64_t v = 0;
            n = mcme_ldb_varint_get(p, end, &v);
            if (n == 0) {
                mcme_set_error("leveldb: truncated number in MANIFEST");
                return false;
            }
            p += n;
            ctx->next_file_number = v;
            break;
        }

        case LDB_TAG_LAST_SEQUENCE: {
            uint64_t v = 0;
            n = mcme_ldb_varint_get(p, end, &v);
            if (n == 0) {
                mcme_set_error("leveldb: truncated number in MANIFEST");
                return false;
            }
            p += n;
            ctx->last_sequence = v;
            break;
        }

        case LDB_TAG_PREV_LOG: {
            uint64_t v = 0;
            n = mcme_ldb_varint_get(p, end, &v);
            if (n == 0) {
                mcme_set_error("leveldb: truncated number in MANIFEST");
                return false;
            }
            p += n;
            break;
        }

        case LDB_TAG_DELETED_FILE: {
            /* Level, then file number. Removing the file from the live set is
               what keeps a compaction honest: the MANIFEST says the file is
               gone, so a reader that demanded it anyway would fail to open a
               perfectly healthy store — and a writer that ignored this would
               resurrect a file every later edit deleted. */
            uint64_t level = 0, number = 0;
            n = mcme_ldb_varint_get(p, end, &level);
            if (n == 0) {
                mcme_set_error("leveldb: truncated deleted-file record in MANIFEST");
                return false;
            }
            p += n;
            n = mcme_ldb_varint_get(p, end, &number);
            if (n == 0) {
                mcme_set_error("leveldb: truncated deleted-file record in MANIFEST");
                return false;
            }
            p += n;

            for (size_t i = 0; i < LDB_LEN(ctx->tables); ++i) {
                if (ctx->tables[i].number == number) {
                    ctx->tables[i] = ctx->tables[LDB_LEN(ctx->tables) - 1];
                    arrsetlen(ctx->tables, LDB_LEN(ctx->tables) - 1);
                    break;
                }
            }
            /* A delete naming a file no edit ever added is odd but harmless:
               the file is not in the live set, which is the state the edit
               asked for. */
            break;
        }

        case LDB_TAG_COMPACT_POINTER: {
            /* Level, then the internal key compaction moved to. Both are
               skipped: a reader that scans every live table in sequence order
               gets the right answer without knowing where compaction put
               anything, but the record still has to be walked to reach the
               next one. This tag is what a store that has ever been compacted
               is full of, and not handling it stops every later record. */
            uint64_t level = 0;
            n = mcme_ldb_varint_get(p, end, &level);
            if (n == 0) {
                mcme_set_error("leveldb: truncated compact pointer in MANIFEST");
                return false;
            }
            p += n;

            const uint8_t *s = NULL;
            size_t slen = 0;
            if (!read_length_prefixed(&p, end, &s, &slen)) {
                mcme_set_error("leveldb: truncated compact pointer key in MANIFEST");
                return false;
            }
            break;
        }

        case LDB_TAG_NEW_FILE: {
            uint64_t level = 0, number = 0, size = 0;
            n = mcme_ldb_varint_get(p, end, &level);
            if (n == 0) return false;
            p += n;
            n = mcme_ldb_varint_get(p, end, &number);
            if (n == 0) return false;
            p += n;
            n = mcme_ldb_varint_get(p, end, &size);
            if (n == 0) return false;
            p += n;

            const uint8_t *s = NULL;
            size_t slen = 0;
            if (!read_length_prefixed(&p, end, &s, &slen)) return false;
            if (!read_length_prefixed(&p, end, &s, &slen)) return false;

            /* File numbers are never reused, so a number already in the set
               can only be a re-add after a delete — replace rather than
               duplicate, which would open the same file twice. */
            bool seen = false;
            for (size_t i = 0; i < LDB_LEN(ctx->tables); ++i) {
                if (ctx->tables[i].number == number) {
                    ctx->tables[i].level = level;
                    seen = true;
                    break;
                }
            }
            if (!seen) {
                LdbTableRef ref = { level, number };
                arrput(ctx->tables, ref);
            }
            break;
        }

        default:
            /* An unknown tag means a newer LevelDB wrote this store. Rather
               than mis-parse the remainder, say so: guessing here would
               silently corrupt everything after it. */
            mcme_set_error("leveldb: MANIFEST uses unknown field tag %llu; "
                           "this store was written by a newer LevelDB",
                           (unsigned long long)tag);
            return false;
        }
    }
    return true;
}

static bool on_manifest_record(void *ud, const uint8_t *record, size_t len)
{
    ManifestCtx *ctx = (ManifestCtx *)ud;
    /* A MANIFEST that stops parsing is fatal: without it we do not know which
       table files are live, and guessing could read a table that was compacted
       away. Continue walking subsequent records anyway only if this one was
       merely short — parse_version_edit already said why it failed. */
    return parse_version_edit(record, len, ctx);
}

/* ========================================================================
 * Log: WriteBatch records
 * ======================================================================== */

/*
 * Expands one WriteBatch record into raw entries.
 *
 * A record is:
 *   8 bytes  first sequence number, little-endian
 *   4 bytes  entry count, little-endian
 *   then, once per entry:
 *     1 byte   value type: 0 deletion, 1 value
 *     varint   key length, then the key
 *     varint   value length, then the value   (absent for a deletion)
 *
 * Each entry's sequence is base + i, which is what lets a merged sort order a
 * log's contents even though the log itself is in insertion order.
 *
 * The key is stored in internal form (user key + tag) so log entries and table
 * entries are directly comparable by the same sort.
 */
static bool expand_write_batch(const uint8_t *data, size_t len,
                               uint8_t **keybuf, uint8_t **valbuf,
                               McmeLdbRawEntry **entries)
{
    if (len < 12) return false;      /* no room for sequence and count */

    uint64_t seq = 0;
    for (int i = 0; i < 8; ++i) seq |= (uint64_t)data[i] << (i * 8);
    uint32_t count = (uint32_t)data[8] | ((uint32_t)data[9] << 8) |
                     ((uint32_t)data[10] << 16) | ((uint32_t)data[11] << 24);

    const uint8_t *p = data + 12;
    const uint8_t *end = data + len;

    for (uint32_t i = 0; i < count; ++i) {
        if (p >= end) return false;
        uint8_t type = *p++;
        if (type != MCME_LDB_DELETION && type != MCME_LDB_VALUE) return false;

        uint64_t klen = 0;
        size_t n = mcme_ldb_varint_get(p, end, &klen);
        if (n == 0 || klen > (uint64_t)(end - p)) return false;
        p += n;
        const uint8_t *key = p;
        p += (size_t)klen;

        const uint8_t *val = NULL;
        size_t vlen = 0;
        if (type == MCME_LDB_VALUE) {
            uint64_t raw_vlen = 0;
            n = mcme_ldb_varint_get(p, end, &raw_vlen);
            if (n == 0 || raw_vlen > (uint64_t)(end - p)) return false;
            p += n;
            val = p;
            vlen = (size_t)raw_vlen;
            p += vlen;
        }

        McmeLdbRawEntry e;
        e.koff = arrlen(*keybuf);
        e.klen = (size_t)klen + 8;
        e.voff = arrlen(*valbuf);
        e.vlen = vlen;

        for (size_t k = 0; k < (size_t)klen; ++k) arrput(*keybuf, key[k]);
        uint64_t tag = ((seq + i) << 8) | (uint64_t)type;
        for (int k = 0; k < 8; ++k) {
            arrput(*keybuf, (uint8_t)((tag >> (k * 8)) & 0xff));
        }
        for (size_t k = 0; k < vlen; ++k) arrput(*valbuf, val[k]);

        arrput(*entries, e);
    }
    return true;
}

typedef struct {
    uint8_t         **keybuf;
    uint8_t         **valbuf;
    McmeLdbRawEntry **entries;
    size_t            expanded;
    size_t            skipped;
} BatchCtx;

static bool on_log_record(void *ud, const uint8_t *record, size_t len)
{
    BatchCtx *ctx = (BatchCtx *)ud;
    if (expand_write_batch(record, len, ctx->keybuf, ctx->valbuf, ctx->entries)) {
        ctx->expanded++;
        return true;
    }
    /* A record that does not parse as a WriteBatch is not necessarily damage —
       but in a data log it should be one, so count it and let the caller
       decide whether an all-skipped log is a problem. */
    ctx->skipped++;
    return true;
}

/* ========================================================================
 * Directory scanning
 * ======================================================================== */

/* Collects regular file names in `dir` ending in `ext`, sorted by file number
   so the oldest comes first. Names are a stb_ds array of strdup'd strings. */
static bool collect_files(const char *dir, const char *ext, char ***out)
{
    DIR *d = opendir(dir);
    if (d == NULL) {
        mcme_set_error("leveldb: cannot open directory '%s'", dir);
        return false;
    }

    struct dirent *e;
    size_t ext_len = strlen(ext);
    while ((e = readdir(d)) != NULL) {
        size_t len = strlen(e->d_name);
        if (len <= ext_len || strcmp(e->d_name + len - ext_len, ext) != 0) continue;

        char path[4096];
        if (mcme_ldb_path(path, sizeof path, dir, e->d_name) == NULL) continue;

        struct stat st;
        if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) continue;

        char *copy = strdup(e->d_name);
        if (copy != NULL) arrput(*out, copy);
    }
    closedir(d);

    /* Insertion sort by file number. It does not change the final merge —
       sequence numbers decide that — but it makes a failure deterministic
       rather than dependent on readdir's order, which is not stable. */
    for (size_t i = 1; i < LDB_LEN(*out); ++i) {
        char *key = (*out)[i];
        uint64_t kn = 0;
        mcme_ldb_file_number(key, &kn);
        size_t j = i;
        while (j > 0) {
            uint64_t pn = 0;
            mcme_ldb_file_number((*out)[j - 1], &pn);
            if (pn <= kn) break;
            (*out)[j] = (*out)[j - 1];
            --j;
        }
        (*out)[j] = key;
    }
    return true;
}

/* Collects every file number currently in the directory, for choosing a fresh
   log file number that cannot collide with anything on disk. */
static uint64_t max_file_number(const char *dir)
{
    uint64_t max = 0;
    DIR *d = opendir(dir);
    if (d == NULL) return 0;

    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        uint64_t n = 0;
        if (!mcme_ldb_file_number(e->d_name, &n)) continue;
        if (n >= max) max = n + 1;
    }
    closedir(d);
    return max;
}

static void free_names(char ***names)
{
    for (size_t i = 0; i < LDB_LEN(*names); ++i) free((*names)[i]);
    arrfree(*names);
    *names = NULL;
}

/* ========================================================================
 * Sort and dedupe
 * ======================================================================== */

static int entry_cmp(const void *pa, const void *pb)
{
    const LiveEntry *a = (const LiveEntry *)pa;
    const LiveEntry *b = (const LiveEntry *)pb;
    /* Internal-key order is (user key ascending, sequence descending), so the
       first entry for a user key after sorting is the newest. */
    return mcme_ldb_internal_cmp(a->key, a->klen, b->key, b->klen);
}

/*
 * Turns the collected raw entries into the merged view.
 *
 * Raw entries are (offset, length) pairs into keybuf and valbuf, because those
 * buffers are still growing while the sources are read. Only once every source
 * has contributed is each key resolved to a pointer — and from then on both
 * buffers are frozen, so the pointers stay valid for the store's lifetime.
 */
static bool finalize(McmeLevelDb *db)
{
    size_t raw_count = LDB_LEN(db->raw);
    if (arrsetcap(db->live, raw_count) == NULL) {
        mcme_set_error("leveldb: out of memory building the merged view");
        return false;
    }

    for (size_t i = 0; i < raw_count; ++i) {
        const McmeLdbRawEntry *r = &db->raw[i];
        LiveEntry e;
        e.key = db->keybuf + r->koff;
        e.klen = r->klen;
        e.voff = r->voff;
        e.vlen = r->vlen;
        e.deleted = false;
        e.sequence = 0;

        /* The tag is the last 8 bytes of an internal key: sequence in the top
           56 bits, value type in the low 8. A key too short to carry one is
           malformed; it is still sorted, but reports as sequence zero rather
           than reading past its end. */
        if (r->klen >= 8) {
            const uint8_t *t = db->keybuf + r->koff + r->klen - 8;
            uint64_t tag = 0;
            for (int k = 0; k < 8; ++k) tag |= (uint64_t)t[k] << (k * 8);
            e.sequence = tag >> 8;
            e.deleted = ((tag & 0xff) == MCME_LDB_DELETION);
        }
        arrput(db->live, e);
    }

    arrfree(db->raw);
    db->raw = NULL;

    size_t n = LDB_LEN(db->live);
    if (n > 1) {
        qsort(db->live, n, sizeof *db->live, entry_cmp);
    }

    /* Keep the first entry for each user key: internal-key order puts the
       highest sequence first, so the survivor is the newest copy, tombstone or
       not. An older copy sitting behind a tombstone must NOT be resurrected,
       which is exactly why dedupe runs before the delete check in iterate. */
    size_t w = 0;
    for (size_t i = 0; i < n; ++i) {
        LiveEntry *e = &db->live[i];

        if (w > 0) {
            size_t prev_user = db->live[w - 1].klen - 8;
            size_t this_user = e->klen - 8;
            if (prev_user == this_user &&
                (this_user == 0 ||
                 memcmp(db->live[w - 1].key, e->key, this_user) == 0)) {
                continue;
            }
        }
        db->live[w++] = *e;
    }
    arrsetlen(db->live, w);
    return true;
}

/* ========================================================================
 * CURRENT / MANIFEST
 * ======================================================================== */

/*
 * Reads CURRENT and the MANIFEST it names, filling in `tables` (the live set,
 * with levels) and the scalar state. Returns false with the error set on any
 * real failure.
 */
static bool read_manifest(McmeLevelDb *db, LdbTableRef **tables)
{
    char path[4096];
    if (mcme_ldb_path(path, sizeof path, db->dir, "CURRENT") == NULL) {
        mcme_set_error("leveldb: directory path too long: %s", db->dir);
        return false;
    }

    char *text = mcme_fs_read_text(path);
    if (text == NULL) {
        mcme_set_error("leveldb: '%s' has no CURRENT file; not a LevelDB store?",
                       db->dir);
        return false;
    }

    size_t len = strlen(text);
    while (len > 0 && (text[len - 1] == '\n' || text[len - 1] == '\r')) {
        text[--len] = '\0';
    }

    bool ok = false;
    if (len == 0) {
        mcme_set_error("leveldb: CURRENT in '%s' is empty", db->dir);
    } else if (mcme_ldb_path(path, sizeof path, db->dir, text) == NULL) {
        mcme_set_error("leveldb: directory path too long: %s", db->dir);
    } else {
        /* Remembered because a writable handle appends to this file: a new
           VersionEdit goes in the MANIFEST CURRENT names, not in whichever
           MANIFEST happens to sort first. */
        free(db->manifest_name);
        db->manifest_name = strdup(text);
        if (db->manifest_name == NULL) {
            mcme_set_error("leveldb: out of memory");
            free(text);
            return false;
        }

        uint8_t *data = NULL;
        size_t data_len = 0;
        if (!mcme_ldb_read_file(path, &data, &data_len, MCME_LDB_LOG_MAX)) {
            /* fall through with the error already set */
        } else {
            ManifestCtx ctx = { &db->comparator, *tables, 0, 0, 0 };
            size_t records = 0;
            ok = mcme_ldb_log_read(data, data_len, on_manifest_record, &ctx,
                                   &records);
            *tables = ctx.tables;
            db->log_number = ctx.log_number;
            db->next_file_number = ctx.next_file_number;
            db->last_sequence = ctx.last_sequence;
            free(data);
            if (ok && db->comparator == NULL) {
                /* Every MANIFEST records a comparator; one that does not is
                   not a MANIFEST we understand. */
                mcme_set_error("leveldb: '%s' is a log but names no comparator",
                               path);
                ok = false;
            }
        }
    }

    free(text);
    return ok;
}

/*
 * (Re)reads every source into the merged view.
 *
 * Called on open and again after each successful append, because the merged
 * view is the store's only read surface and a write that the view does not
 * reflect would be a write the caller cannot see. Rebuilding from disk is the
 * blunt, obviously-correct choice — the same code path as open, so there is no
 * second merge implementation to keep in agreement — and its cost is the same
 * O(entries) the open already paid.
 *
 * On failure the store is left `broken`: the write is on disk and a reopen
 * will see it, but this handle's view no longer matches the store, so every
 * read refuses rather than answering from a view missing that write.
 */
static bool load(McmeLevelDb *db)
{
    arrfree(db->raw);   db->raw = NULL;
    arrfree(db->live);  db->live = NULL;
    /* Length zero rather than freed: the buffers keep their capacity across a
       reload, and a literal 0 here would trip -Wtype-limits inside stb_ds's
       unsigned capacity comparison. */
    size_t reset = 0;
    arrsetlen(db->keybuf, reset);
    arrsetlen(db->valbuf, reset);
    arrsetlen(db->tables, reset);
    db->table_count = 0;
    db->log_count = 0;

    if (!read_manifest(db, &db->tables)) goto fail;

    /* Only the default bytewise comparator is implemented. LevelDB orders
       internal keys by user key then sequence, which is exactly what this
       reader's merge assumes; a custom comparator would make that wrong, and
       silently reading with the wrong order produces a store that looks
       populated but answers lookups incorrectly. */
    static const char kBytewise[] = "leveldb.BytewiseComparator";
    if (db->comparator != NULL &&
        (strlen(db->comparator) != sizeof kBytewise - 1 ||
         strcmp(db->comparator, kBytewise) != 0)) {
        mcme_set_error("leveldb: unsupported comparator '%s'", db->comparator);
        goto fail;
    }

    char path[4096];

    /* --- Tables, from the manifest's live set. --- */
    size_t table_total = LDB_LEN(db->tables);
    for (size_t i = 0; i < table_total; ++i) {
        char name[64];
        /* File numbers are zero-padded to six digits; numbers past that are
           written without padding, so snprintf handles both. */
        snprintf(name, sizeof name, "%06llu.ldb",
                 (unsigned long long)db->tables[i].number);

        if (mcme_ldb_path(path, sizeof path, db->dir, name) == NULL) continue;

        struct stat st;
        if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) {
            /* LevelDB calls .sst what it has not yet renamed to .ldb; both
               spellings exist in the wild for the same format. */
            snprintf(name, sizeof name, "%06llu.sst",
                     (unsigned long long)db->tables[i].number);
            if (mcme_ldb_path(path, sizeof path, db->dir, name) == NULL) continue;
            if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) {
                mcme_set_error("leveldb: MANIFEST lists table %llu, which is "
                               "missing from '%s'",
                               (unsigned long long)db->tables[i].number, db->dir);
                goto fail;
            }
        }

        McmeLdbTable table;
        if (!mcme_ldb_table_open(&table, path)) goto fail;
        if (!mcme_ldb_table_scan(&table, &db->keybuf, &db->valbuf, &db->raw)) {
            mcme_ldb_table_close(&table);
            goto fail;
        }
        mcme_ldb_table_close(&table);
        db->table_count++;

        if (LDB_LEN(db->raw) > MCME_LDB_MAX_ENTRIES) {
            mcme_set_error("leveldb: store has over %llu entries",
                           (unsigned long long)MCME_LDB_MAX_ENTRIES);
            goto fail;
        }
    }

    /* --- Logs: records not yet flushed into a table. --- */
    char **logs = NULL;
    if (!collect_files(db->dir, ".log", &logs)) goto fail;

    size_t log_total = LDB_LEN(logs);
    for (size_t i = 0; i < log_total; ++i) {
        if (mcme_ldb_path(path, sizeof path, db->dir, logs[i]) == NULL) continue;

        uint8_t *data = NULL;
        size_t data_len = 0;
        if (!mcme_ldb_read_file(path, &data, &data_len, MCME_LDB_LOG_MAX)) {
            free_names(&logs);
            goto fail;
        }

        BatchCtx ctx = { &db->keybuf, &db->valbuf, &db->raw, 0, 0 };
        size_t records = 0;
        if (!mcme_ldb_log_read(data, data_len, on_log_record, &ctx, &records)) {
            free(data);
            free_names(&logs);
            goto fail;
        }
        free(data);

        /* A data log in which no record expanded to a batch means the format
           is not what we assumed. Report it rather than silently contributing
           nothing, which would look like a valid but empty world. */
        if (records > 0 && ctx.expanded == 0) {
            mcme_set_error("leveldb: '%s' holds %zu records, none of them "
                           "WriteBatches", path, records);
            free_names(&logs);
            goto fail;
        }
        db->log_count++;
    }
    free_names(&logs);

    if (LDB_LEN(db->raw) > MCME_LDB_MAX_ENTRIES) {
        mcme_set_error("leveldb: store has over %llu entries",
                       (unsigned long long)MCME_LDB_MAX_ENTRIES);
        goto fail;
    }

    if (!finalize(db)) goto fail;

    /* The sequence the next write must use. The MANIFEST's last_sequence can
       be ahead of the data (a batch may be written and the MANIFEST updated
       before anything is flushed) and the data can be ahead of the MANIFEST
       (a crash between the two), so the answer is the max of both — never
       either alone, or a re-used sequence number would make an old copy of a
       key sort newer than the write that replaced it. */
    for (size_t i = 0; i < LDB_LEN(db->live); ++i) {
        if (db->live[i].sequence > db->last_sequence) {
            db->last_sequence = db->live[i].sequence;
        }
    }
    return true;

fail:
    /* The table set belongs to the handle now — it is what a later compaction
       would delete — so a failed load leaves it cleared rather than stale. */
    arrsetlen(db->tables, reset);
    return false;
}

/* ========================================================================
 * Open / close
 * ======================================================================== */

/*
 * Takes an exclusive flock on LOCK, the same lock Minecraft and stock LevelDB
 * take. flock is released by the kernel when the fd closes, including on a
 * crash, so there is no stale-lock state to recover from — the cost is that
 * it is advisory, and a program that ignores LOCK can still write. That is
 * LevelDB's own position on it, and it is enough to keep mcme from opening a
 * store the game currently has open.
 */
static bool ldb_lock(McmeLevelDb *db)
{
    char path[4096];
    if (mcme_ldb_path(path, sizeof path, db->dir, "LOCK") == NULL) {
        mcme_set_error("leveldb: directory path too long: %s", db->dir);
        return false;
    }

    int fd = open(path, O_RDWR | O_CREAT, 0644);
    if (fd < 0) {
        mcme_set_error("leveldb: cannot open '%s': %s", path, strerror(errno));
        return false;
    }
    if (flock(fd, LOCK_EX | LOCK_NB) != 0) {
        mcme_set_error("leveldb: '%s' is locked; close whatever else has it "
                       "open (the game?) and retry", db->dir);
        close(fd);
        return false;
    }
    db->lock_fd = fd;
    return true;
}

/*
 * Shared body of mcme_leveldb_open and mcme_leveldb_open_rw.
 *
 * `writable` additionally takes an exclusive flock on LOCK, which is the one
 * thing a read-only open deliberately does not do: two readers of one store
 * are harmless, and a reader next to a writer is how LevelDB itself is used,
 * but two writers are how a world gets corrupted. Minecraft holds the same
 * lock while the game has the world open, so this doubles as "the game is not
 * running against this directory".
 */
static bool open_common(McmeLevelDb **out, const char *dir, bool writable)
{
    /* *out is cleared on every path that returns false, including this one.
       Clearing it after the argument check instead would leave a caller's
       stale pointer in place on the most common mistake — passing a NULL
       directory — which is exactly the case where a caller is most likely to
       have skipped initialising it. */
    if (out != NULL) *out = NULL;

    if (out == NULL || dir == NULL) {
        mcme_set_error("leveldb: NULL argument");
        return false;
    }

    McmeLevelDb *db = (McmeLevelDb *)calloc(1, sizeof *db);
    if (db == NULL) {
        mcme_set_error("leveldb: out of memory");
        return false;
    }
    db->dir = strdup(dir);
    if (db->dir == NULL) {
        free(db);
        mcme_set_error("leveldb: out of memory");
        return false;
    }
    db->lock_fd = -1;
    db->writable = writable;

    if (writable && !ldb_lock(db)) {
        mcme_leveldb_close(db);
        return false;
    }

    if (!load(db)) {
        mcme_leveldb_close(db);
        return false;
    }

    *out = db;
    return true;
}

bool mcme_leveldb_open(McmeLevelDb **out, const char *dir)
{
    return open_common(out, dir, false);
}

bool mcme_leveldb_open_rw(McmeLevelDb **out, const char *dir)
{
    return open_common(out, dir, true);
}

void mcme_leveldb_close(McmeLevelDb *db)
{
    if (db == NULL) return;
    if (db->log != NULL) {
        fflush(db->log);
        fclose(db->log);
    }
    if (db->lock_fd >= 0) close(db->lock_fd);
    arrfree(db->raw);
    arrfree(db->live);
    arrfree(db->keybuf);
    arrfree(db->valbuf);
    arrfree(db->tables);
    free(db->comparator);
    free(db->manifest_name);
    free(db->dir);
    free(db);
}

/* ========================================================================
 * Reading
 * ======================================================================== */

/* User-key length of a merged entry. Internal keys always carry the 8-byte
   tag, which both producers guarantee; the subtraction is guarded anyway so a
   malformed entry cannot underflow. */
static size_t user_len(size_t internal_len)
{
    return (internal_len >= 8) ? internal_len - 8 : internal_len;
}

static const LiveEntry *find(const McmeLevelDb *db, const void *key, size_t key_len)
{
    size_t lo = 0, hi = arrlen(db->live);
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        const LiveEntry *e = &db->live[mid];

        size_t elen = user_len(e->klen);
        size_t n = elen < key_len ? elen : key_len;
        int r = (n > 0) ? memcmp(e->key, key, n) : 0;
        if (r == 0) r = (elen == key_len) ? 0 : (elen < key_len ? -1 : 1);

        if (r < 0) lo = mid + 1;
        else if (r > 0) hi = mid;
        else return e;   /* dedupe guarantees at most one entry per user key */
    }
    return NULL;
}

/*
 * Resolves an entry's value bytes.
 *
 * An empty value keeps an offset into valbuf but contributes no bytes, so on a
 * store whose entries are all deletions valbuf is never allocated and stays
 * NULL. Forming `NULL + 0` is undefined behaviour, and ASan builds say so — so
 * the empty case returns a real address instead of an offset.
 */
static const uint8_t *value_ptr(const McmeLevelDb *db, const LiveEntry *e)
{
    if (e->vlen == 0) return (const uint8_t *)"";
    return db->valbuf + e->voff;
}

size_t mcme_leveldb_iterate(McmeLevelDb *db, McmeLevelEntryFn fn, void *userdata)
{
    if (db == NULL || fn == NULL) return 0;

    size_t delivered = 0;
    size_t total = LDB_LEN(db->live);
    for (size_t i = 0; i < total; ++i) {
        const LiveEntry *e = &db->live[i];
        if (e->deleted) continue;      /* a tombstone is not a live key */

        McmeLevelEntry entry;
        entry.key = e->key;
        entry.key_len = user_len(e->klen);
        entry.val = value_ptr(db, e);
        entry.val_len = e->vlen;
        entry.deleted = false;
        entry.sequence = e->sequence;

        delivered++;
        if (!fn(userdata, &entry)) break;
    }
    return delivered;
}

size_t mcme_leveldb_count(McmeLevelDb *db)
{
    if (db == NULL) return 0;
    size_t n = 0;
    size_t total = LDB_LEN(db->live);
    for (size_t i = 0; i < total; ++i) {
        if (!db->live[i].deleted) n++;
    }
    return n;
}

bool mcme_leveldb_get(McmeLevelDb *db, const void *key, size_t key_len,
                      McmeBuffer *out, bool *out_deleted)
{
    if (db == NULL || key == NULL || out == NULL) {
        mcme_set_error("leveldb: NULL argument");
        return false;
    }
    if (out_deleted != NULL) *out_deleted = false;

    const LiveEntry *e = find(db, key, key_len);
    if (e == NULL) {
        mcme_set_error("leveldb: key not found");
        return false;
    }
    if (e->deleted) {
        /* Deleted is a real answer, not a failure: the caller asked about a
           key and the store says it was removed. */
        if (out_deleted != NULL) *out_deleted = true;
        return true;
    }
    return mcme_buffer_append(out, value_ptr(db, e), e->vlen);
}

bool mcme_leveldb_has(McmeLevelDb *db, const void *key, size_t key_len)
{
    if (db == NULL || key == NULL) return false;
    return find(db, key, key_len) != NULL;
}

size_t mcme_leveldb_table_count(const McmeLevelDb *db)
{
    return db == NULL ? 0 : db->table_count;
}

size_t mcme_leveldb_log_count(const McmeLevelDb *db)
{
    return db == NULL ? 0 : db->log_count;
}

const char *mcme_leveldb_comparator(const McmeLevelDb *db)
{
    return db == NULL ? NULL : db->comparator;
}

/* ========================================================================
 * Writing
 *
 * The shape is log-append and nothing else, which is decision D9's stated
 * scope: LevelDB replays a log on open and compacts it into tables later, so
 * a writer that only ever appends records produces a store every LevelDB
 * reads correctly — mcme's own reader included, because it merges the log
 * with the tables exactly as the real library does.
 *
 * What is deliberately NOT here: table writing, compaction, log rotation.
 * Rotation in particular is not a missing feature but an unsafe one at this
 * scope — LevelDB recovers only the logs numbered at or above the MANIFEST's
 * log number, on the assumption that everything older was flushed into a
 * table first. A writer that starts a new log without that flush would leave
 * the previous log's records invisible to every reader that trusts the
 * MANIFEST. Appending to one log until the process exits has none of that
 * problem, and the ceiling below turns "forever" into an error message
 * instead of a 64 MiB file this reader then refuses to reopen.
 * ======================================================================== */

/* Refuse a log past what mcme_ldb_read_file will take on reopen, so a session
   of edits cannot grow a store that only the session that wrote it can read.
   Half the ceiling leaves room for the record being appended. This is a
   backstop now, not the plan: writes compact past MCME_LDB_LOG_COMPACT_AT
   below, so reaching here means compaction itself has been failing. */
#define MCME_LDB_LOG_WRITE_MAX ((size_t)32u * 1024u * 1024u)

/* Log size at which the next write flushes everything into a table first.
   LevelDB's own default write buffer is 4 MiB, so a store that alternates
   between mcme and the game sees the same shape from either side. */
#define MCME_LDB_LOG_COMPACT_AT ((size_t)4u * 1024u * 1024u)

/*
 * Opens (or creates) the log this handle appends to and positions for
 * append. The MANIFEST's log file may be gone — a store that was compacted
 * then closed often has none — in which case a fresh one is created under a
 * number nothing on disk uses, and the MANIFEST is told about it by the
 * caller. That edit matters: without it, the next reader is told there is no
 * log and must not go looking for one, and every record appended here would
 * be invisible.
 */
static bool open_log(McmeLevelDb *db)
{
    if (db->log != NULL) return true;

    char name[64];
    if (db->log_number == 0) {
        db->log_number = max_file_number(db->dir);
        if (db->log_number == 0) db->log_number = 1;
    }
    snprintf(name, sizeof name, "%06llu.log",
             (unsigned long long)db->log_number);

    char path[4096];
    if (mcme_ldb_path(path, sizeof path, db->dir, name) == NULL) {
        mcme_set_error("leveldb: directory path too long: %s", db->dir);
        return false;
    }

    struct stat st;
    bool existed = (stat(path, &st) == 0 && S_ISREG(st.st_mode));
    if (existed) db->log_size = (size_t)st.st_size;

    db->log = fopen(path, "ab");
    if (db->log == NULL) {
        mcme_set_error("leveldb: cannot open '%s' for append: %s",
                       path, strerror(errno));
        return false;
    }
    return true;
}

/*
 * Appends one WriteBatch as a single log record and reloads the view.
 *
 * `ops` is one atomic unit: a single record, so a reader that stops at a torn
 * tail drops the whole batch rather than half of it, which is the same
 * guarantee stock LevelDB gives. Sequence numbers are handed out in order
 * from the store's high-water mark, so within a batch the sort order is the
 * array order — a caller overwriting one key twice in a batch gets the last
 * write, as it must.
 */
bool mcme_leveldb_write(McmeLevelDb *db, const McmeLevelOp *ops, size_t n)
{
    if (db == NULL || ops == NULL || n == 0) {
        mcme_set_error("leveldb: NULL argument or empty batch");
        return false;
    }
    if (!db->writable) {
        mcme_set_error("leveldb: store was opened read-only");
        return false;
    }
    if (db->broken) {
        mcme_set_error("leveldb: a previous write landed but the view could "
                       "not be rebuilt; reopen the store before writing again");
        return false;
    }

    /* The batch payload, built in full before anything touches the file: an
       allocation failure here must not leave half a record on disk. Layout is
       the 8-byte sequence, the 4-byte count, then one record per operation. */
    McmeBuffer payload;
    mcme_buffer_init(&payload);

    uint64_t base = db->last_sequence + 1;
    uint8_t header[12];
    for (int i = 0; i < 8; ++i) {
        header[i] = (uint8_t)((base >> (i * 8)) & 0xff);
    }
    for (int i = 0; i < 4; ++i) {
        header[8 + i] = (uint8_t)(((uint32_t)n >> (i * 8)) & 0xff);
    }

    bool ok = mcme_buffer_append(&payload, header, sizeof header);
    for (size_t i = 0; ok && i < n; ++i) {
        if (ops[i].key == NULL || (ops[i].val == NULL && ops[i].val_len > 0)) {
            mcme_set_error("leveldb: batch entry %zu has no key or a NULL value",
                           i);
            ok = false;
            break;
        }
        uint8_t type = ops[i].deleted ? MCME_LDB_DELETION : MCME_LDB_VALUE;
        ok = mcme_buffer_append_u8(&payload, type) &&
             mcme_ldb_varint_put(&payload, ops[i].key_len) &&
             mcme_buffer_append(&payload, ops[i].key, ops[i].key_len);
        if (ok && !ops[i].deleted) {
            ok = mcme_ldb_varint_put(&payload, ops[i].val_len) &&
                 mcme_buffer_append(&payload, ops[i].val, ops[i].val_len);
        }
    }

    if (!ok) {
        mcme_buffer_free(&payload);
        return false;
    }

    if (db->log_size + payload.len + 7 > MCME_LDB_LOG_WRITE_MAX) {
        /* Reachable in two ways: a single batch bigger than the ceiling, or
           compaction failing to shrink a log that was already large — in
           which case mcme_last_error() still holds compaction's reason.
           Copied out before set_error, which writes the same buffer the
           pointer came from. */
        char why[1024];
        snprintf(why, sizeof why, "%s", mcme_last_error());
        mcme_set_error("leveldb: log would exceed %u bytes%s%s",
                       (unsigned)MCME_LDB_LOG_WRITE_MAX,
                       why[0] != '\0' ? ": " : "", why);
        mcme_buffer_free(&payload);
        return false;
    }

    if (!open_log(db)) {
        mcme_buffer_free(&payload);
        return false;
    }

    /* Flush first when the log has grown past the trigger. Compacting before
       the append means a failed compaction leaves the store exactly as it
       was, with this write simply not happened — the alternative, appending
       first, would leave a record in a log the compaction then deletes, and
       the caller would have to be told that "failed" sometimes means
       "landed". This runs on every write because open_log just stat'd the
       file, so a log grown past the trigger in an earlier session is caught
       by the first write of the next one too. */
    if (db->log_size > MCME_LDB_LOG_COMPACT_AT) {
        if (!mcme_leveldb_compact(db)) {
            mcme_buffer_free(&payload);
            return false;
        }
        if (!open_log(db)) {
            mcme_buffer_free(&payload);
            return false;
        }
    }

    /* The writer resumes where the file left off, mid-block if that is where
       it was: the offset is the file size modulo the block size, and the
       record may cross any number of block boundaries from there. */
    size_t block_offset = db->log_size % MCME_LDB_BLOCK_SIZE;
    ok = mcme_ldb_log_append(db->log, payload.data, payload.len, &block_offset);
    long pos = ok ? ftell(db->log) : -1;
    mcme_buffer_free(&payload);
    if (!ok || pos < 0) {
        if (ok) mcme_set_error("leveldb: cannot track log position: %s",
                               strerror(errno));
        db->broken = true;
        return false;
    }

    /* The file position after the append is the new size, padding included —
       which is exactly what the next append's block offset and the reopen
       ceiling both need, and one number instead of a second implementation
       of what emit_fragment just wrote. */
    db->log_size = (size_t)pos;

    /* Flush before claiming success: the record is in the OS's page cache
       after fflush, which is the durability stock LevelDB promises without
       its sync option, and it is what keeps the next append from landing
       before this one in the file. */
    if (fflush(db->log) != 0) {
        mcme_set_error("leveldb: flush after write failed: %s",
                       strerror(errno));
        db->broken = true;
        return false;
    }

    db->last_sequence = base + n - 1;

    if (!load(db)) {
        db->broken = true;
        return false;
    }
    return true;
}

bool mcme_leveldb_put(McmeLevelDb *db, const void *key, size_t key_len,
                      const void *val, size_t val_len)
{
    McmeLevelOp op = { key, key_len, val, val_len, false };
    return mcme_leveldb_write(db, &op, 1);
}

bool mcme_leveldb_delete(McmeLevelDb *db, const void *key, size_t key_len)
{
    McmeLevelOp op = { key, key_len, NULL, 0, true };
    return mcme_leveldb_write(db, &op, 1);
}

/* ========================================================================
 * Compaction: flush everything into one new table
 *
 * Rewrites the whole merged view as a single new SSTable, records that in the
 * MANIFEST, and only then removes what it replaces. This is decision D11's
 * shape — a full rewrite rather than a level-by-level one — and it is what
 * turns "the log grows until an error" into "the log is flushed whenever it
 * gets large" (mcme_leveldb_write compacts past MCME_LDB_LOG_COMPACT_AT).
 *
 * Two properties are worth stating because the code below is ordered around
 * them:
 *
 *   The view does not change. Every live entry — tombstones included — goes
 *   into the new table under its original internal key and sequence number.
 *   Dropping tombstones is what real LevelDB does when compacting the
 *   bottommost level, but here it would change has() and get(deleted)
 *   answers across a compaction, and "compaction changed what the store
 *   says" is a bug class not worth entering for the bytes it saves. An
 *   emptied key therefore stays deleted across compaction and reopen.
 *
 *   A crash at any point leaves a store some reader accepts. The order is
 *   table (fsync'd) before MANIFEST (fsync'd) before deletion:
 *     - crash before the MANIFEST edit: the new table is an unreferenced
 *       file; the old tables and log are still the truth, still on disk;
 *     - crash after it: the MANIFEST no longer names the old files, so
 *       readers use the new table and treat the leftovers as garbage
 *       (stock LevelDB deletes such files on its next open; mcme leaves
 *       them alone, because its MANIFEST parsing is what decides liveness
 *       and trusting the directory instead would be the dangerous part);
 *     - either way no reader ever sees a MANIFEST naming a table whose
 *       bytes might not have reached the disk — which is the one thing
 *       that could not be undone by a later open.
 * The fsyncs are what make those windows real; without them the MANIFEST
 * could land ahead of the table it points at, and no later recovery can
 * invent the missing bytes.
 * ======================================================================== */

/* Appends a length-prefixed slice (varint length, then bytes), the MANIFEST's
   encoding for the smallest/largest keys inside a NEW_FILE record. */
static bool append_len_prefixed(McmeBuffer *b, const void *data, size_t len)
{
    if (len == 0) data = "";      /* NULL + 0 is undefined, and ASan says so */
    return mcme_ldb_varint_put(b, len) && mcme_buffer_append(b, data, len);
}

/* fsyncs a file by path. mcme_fs_write closes what it writes, so the table
   has to be reopened to be synced — and it must be synced: the MANIFEST edit
   below makes this file the store, and a MANIFEST that outlives the page
   cache holding its table is a store that cannot be read back. */
static bool fsync_path(const char *path)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        mcme_set_error("leveldb: cannot reopen '%s' to sync it: %s",
                       path, strerror(errno));
        return false;
    }
    bool ok = fsync(fd) == 0;
    if (!ok) {
        mcme_set_error("leveldb: cannot sync '%s': %s", path, strerror(errno));
    }
    close(fd);
    return ok;
}

bool mcme_leveldb_compact(McmeLevelDb *db)
{
    if (db == NULL) {
        mcme_set_error("leveldb: NULL argument");
        return false;
    }
    if (!db->writable) {
        mcme_set_error("leveldb: store was opened read-only");
        return false;
    }
    if (db->broken) {
        mcme_set_error("leveldb: a previous write landed but the view could "
                       "not be rebuilt; reopen the store before compacting");
        return false;
    }

    /* A store with no entries and no tables has nothing to consolidate, and
       writing an empty table plus a MANIFEST edit for it would be churn that
       a later reader has to walk past. A log file too small to matter is not
       a reason to skip: its records are entries, so live is not empty. */
    size_t total = LDB_LEN(db->live);
    if (total == 0 && LDB_LEN(db->tables) == 0) return true;

    /* The entries, in the merged view's ascending internal-key order — which
       is the order the table writer requires and does not check for. Keys
       point into keybuf and values into valbuf, both frozen until the load()
       at the end. */
    McmeLdbTableEntry *entries = NULL;
    if (total > 0) {
        entries = (McmeLdbTableEntry *)malloc(total * sizeof *entries);
        if (entries == NULL) {
            mcme_set_error("leveldb: out of memory compacting %zu entries", total);
            return false;
        }
        for (size_t i = 0; i < total; ++i) {
            const LiveEntry *e = &db->live[i];
            entries[i].key = e->key;
            entries[i].klen = e->klen;
            entries[i].val = value_ptr(db, e);
            entries[i].vlen = e->vlen;
        }
    }

    /* Fresh numbers, above everything the MANIFEST has handed out and above
       everything on disk — an orphan left by a crashed compaction must never
       be reused, or a later open could find two files wanting one number. */
    uint64_t next = max_file_number(db->dir);
    if (db->next_file_number > next) next = db->next_file_number;
    if (next == 0) next = 1;
    uint64_t table_number = next;
    uint64_t log_number = next + 1;
    uint64_t after = next + 2;

    char name[64];
    char path[4096];
    snprintf(name, sizeof name, "%06llu.ldb", (unsigned long long)table_number);
    bool ok = mcme_ldb_path(path, sizeof path, db->dir, name) != NULL;
    if (ok) ok = mcme_ldb_table_write(path, entries, total);
    free(entries);
    if (!ok) {
        /* A short write can leave a partial file under this number. Nothing
           names it, so every reader ignores it; stock LevelDB would delete it
           on its next open. */
        return false;
    }

    struct stat st;
    if (stat(path, &st) != 0) {
        mcme_set_error("leveldb: cannot stat new table '%s': %s",
                       path, strerror(errno));
        return false;
    }
    uint64_t table_size = (uint64_t)st.st_size;

    if (!fsync_path(path)) return false;

    /* The VersionEdit. Tag order is the record's own business — both readers
       walk it tag by tag — so the deletions come first, then the new file,
       then the three scalars the recovery of a log-free store depends on. */
    McmeBuffer rec;
    mcme_buffer_init(&rec);

    for (size_t i = 0; ok && i < LDB_LEN(db->tables); ++i) {
        ok = mcme_ldb_varint_put(&rec, LDB_TAG_DELETED_FILE) &&
             mcme_ldb_varint_put(&rec, db->tables[i].level) &&
             mcme_ldb_varint_put(&rec, db->tables[i].number);
    }

    /* The new file, at level 0 like every flush, with the table's smallest
       and largest internal keys. A level-0 file may overlap anything; the
       keys still have to be the real first and last, because stock LevelDB
       skips a file whose range lies entirely below the lookup key, and a
       wrong range would make keys unreachable rather than merely untidy. */
    ok = ok &&
         mcme_ldb_varint_put(&rec, LDB_TAG_NEW_FILE) &&
         mcme_ldb_varint_put(&rec, 0) &&
         mcme_ldb_varint_put(&rec, table_number) &&
         mcme_ldb_varint_put(&rec, table_size) &&
         append_len_prefixed(&rec, total > 0 ? db->live[0].key : NULL,
                             total > 0 ? db->live[0].klen : 0) &&
         append_len_prefixed(&rec, total > 0 ? db->live[total - 1].key : NULL,
                             total > 0 ? db->live[total - 1].klen : 0);

    /* last_sequence must be written here, not merely kept in memory: every
       record this store ever had is about to stop existing as a log, so the
       next writer's sequence floor lives or dies with this field. Reusing a
       sequence would let an old copy of a key — now sitting in the new table
       — sort newer than the write that replaces it, which is bug #2 of D9
       arriving from the other direction. */
    ok = ok &&
         mcme_ldb_varint_put(&rec, LDB_TAG_LOG_NUMBER) &&
         mcme_ldb_varint_put(&rec, log_number) &&
         mcme_ldb_varint_put(&rec, LDB_TAG_NEXT_FILE) &&
         mcme_ldb_varint_put(&rec, after) &&
         mcme_ldb_varint_put(&rec, LDB_TAG_LAST_SEQUENCE) &&
         mcme_ldb_varint_put(&rec, db->last_sequence);

    if (ok) {
        char mpath[4096];
        ok = mcme_ldb_path(mpath, sizeof mpath, db->dir, db->manifest_name) != NULL;
        if (ok) {
            struct stat mst;
            if (stat(mpath, &mst) != 0) {
                mcme_set_error("leveldb: cannot stat '%s': %s",
                               mpath, strerror(errno));
                ok = false;
            } else {
                /* The MANIFEST is a log file, so an append has to continue
                   its block structure from wherever the file currently ends —
                   the same arithmetic mcme_leveldb_write uses for a data log. */
                size_t block_offset = (size_t)mst.st_size % MCME_LDB_BLOCK_SIZE;
                FILE *mf = fopen(mpath, "ab");
                if (mf == NULL) {
                    mcme_set_error("leveldb: cannot open '%s' for append: %s",
                                   mpath, strerror(errno));
                    ok = false;
                } else {
                    ok = mcme_ldb_log_append(mf, rec.data, rec.len, &block_offset);
                    if (ok) ok = fflush(mf) == 0;
                    /* Synced before anything is deleted: from here on the
                       MANIFEST is the store, and a deletion that outlives it
                       would remove the only copy of the data. */
                    if (ok) ok = fsync(fileno(mf)) == 0;
                    if (!ok && mcme_last_error()[0] == '\0') {
                        mcme_set_error("leveldb: cannot write '%s': %s",
                                       mpath, strerror(errno));
                    }
                    fclose(mf);
                }
            }
        }
    }
    mcme_buffer_free(&rec);
    if (!ok) return false;   /* the store is untouched: nothing was deleted */

    /* Everything above is durable, so the old files are now garbage rather
       than data. Close the log handle before unlinking its file — the fd
       would keep a deleted file alive and its space unreclaimed. */
    if (db->log != NULL) {
        fflush(db->log);
        fclose(db->log);
        db->log = NULL;
    }

    for (size_t i = 0; i < LDB_LEN(db->tables); ++i) {
        char old[64], oldpath[4096];
        /* Both spellings: whichever the MANIFEST named, the file on disk is
           one of them. ENOENT is not an error — a crashed earlier compaction
           may already have removed it. */
        snprintf(old, sizeof old, "%06llu.ldb",
                 (unsigned long long)db->tables[i].number);
        if (mcme_ldb_path(oldpath, sizeof oldpath, db->dir, old) != NULL) {
            unlink(oldpath);
        }
        snprintf(old, sizeof old, "%06llu.sst",
                 (unsigned long long)db->tables[i].number);
        if (mcme_ldb_path(oldpath, sizeof oldpath, db->dir, old) != NULL) {
            unlink(oldpath);
        }
    }

    /* Every log file, not just the one this handle held: the new table was
       built from the merged view, which already contained every record in
       every log on disk, so all of them are fully superseded. This also
       clears logs an earlier crash left behind. A failed unlink litters; it
       does not corrupt — the MANIFEST no longer points at any of it. */
    char **logs = NULL;
    if (collect_files(db->dir, ".log", &logs)) {
        for (size_t i = 0; i < LDB_LEN(logs); ++i) {
            char lpath[4096];
            if (mcme_ldb_path(lpath, sizeof lpath, db->dir, logs[i]) != NULL) {
                unlink(lpath);
            }
        }
        free_names(&logs);
    }

    db->log_number = log_number;
    db->log_size = 0;
    db->next_file_number = after;

    if (!load(db)) {
        /* The MANIFEST edit is on disk and durable, so a reopen sees the
           compacted store; this handle just cannot show it. */
        db->broken = true;
        return false;
    }
    return true;
}
