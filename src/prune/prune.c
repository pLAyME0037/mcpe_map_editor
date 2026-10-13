/*
 * `mcme chunk prune`'s body: whole chunks, selected by a box of
 * columns, whichever world half the probe names (decisions.md#D13).
 *
 * Bedrock's half is a scan: every key the store holds is parsed, and
 * the keys mcme_bedrock_key_parse names for a chunk the box reaches
 * are tombstoned. Only a scan sees every companion record — subchunks
 * in both spellings, the version record, biome/heightmap, block
 * entities, pending ticks — and only fixed-shape chunk records go:
 * actor records and level metadata are not chunk-shaped and are never
 * touched, because mcme deletes what it can name and guesses no tag
 * numbers. The tombstones go out in batches of one log record each,
 * sliced so even a world-scale prune stays under the log's ceiling
 * (D10); a failure part-way leaves whole batches landed, the same
 * honesty every multi-record write keeps.
 *
 * Java's half punches holes: a zeroed location entry is the format's
 * own "nothing here", the same bytes a slot the file never had
 * carries. Every region file the box reaches is opened first, so a
 * file that will not read refuses the whole prune with nothing
 * punched; the punch itself is an in-place write of the slot's entry
 * and timestamp — no re-encode, no payload read. The payload bytes
 * stay until `region compact` rewrites the file around what remains,
 * exactly as Bedrock's tombstones stay until `db compact`.
 *
 * Neither half reclaims space; the compact verbs own that (D13).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "stb_ds.h"

#include "mcme/prune.h"
#include "mcme/bedrock_keys.h"
#include "mcme/leveldb.h"
#include "mcme/region.h"

/* One log record holds this many tombstones at most: a prune of a
   million chunks is sliced rather than refused, and no single
   append comes near the log's ceiling. */
#define PRUNE_BATCH 4096

static void set_refusal(McmeWriteRefusal *refusal, McmeWriteRefusal why)
{
    if (refusal != NULL) *refusal = why;
}

/* The chunk columns a box reaches, normalised: cx1 <= cx2 and
   cz1 <= cz2 whatever order the corners came in. X/Z are floored to
   chunks by the rule every chunk verb speaks; Y is no part of it — a
   chunk is all of its Y, so the selection is a box of columns. */
static void box_columns(const McmeBox *box,
                        int32_t *out_cx1, int32_t *out_cz1,
                        int32_t *out_cx2, int32_t *out_cz2)
{
    int32_t ax1 = mcme_bedrock_chunk_of(box->x1);
    int32_t ax2 = mcme_bedrock_chunk_of(box->x2);
    int32_t az1 = mcme_bedrock_chunk_of(box->z1);
    int32_t az2 = mcme_bedrock_chunk_of(box->z2);
    *out_cx1 = ax1 < ax2 ? ax1 : ax2;
    *out_cx2 = ax1 < ax2 ? ax2 : ax1;
    *out_cz1 = az1 < az2 ? az1 : az2;
    *out_cz2 = az1 < az2 ? az2 : az1;
}

/* ========================================================================
 * Bedrock: scan, tombstone, slice
 * ======================================================================== */

/* A key to delete, copied out of the iterator's buffer — the batch is
   written after the scan, so the bytes must outlive the callback. A
   parse-true key is at most 14 bytes, so a fixed slot holds any. */
typedef struct {
    uint8_t key[14];
    uint8_t len;
    int32_t x, z;      /* the column, remembered for the chunk count */
} PruneKey;

typedef struct {
    PruneKey *keys;
    int32_t   cx1, cz1, cx2, cz2;
    int32_t   dimension;
} PruneScan;

/* Decides which of the store's keys a prune takes: chunk-shaped
   (mcme_bedrock_key_parse), the wanted dimension, and a column the
   box reaches. Everything else — actors, level metadata, tombstones
   already in the store — passes by untouched. */
static bool scan_key(void *userdata, const McmeLevelEntry *entry)
{
    PruneScan *s = userdata;
    if (entry->deleted) return true;
    if (entry->key_len > 14) return true;

    McmeBedrockKey k;
    if (!mcme_bedrock_key_parse(entry->key, entry->key_len, &k)) return true;
    if (k.dimension != s->dimension) return true;
    if (k.x < s->cx1 || k.x > s->cx2 || k.z < s->cz1 || k.z > s->cz2) {
        return true;
    }

    PruneKey take;
    memcpy(take.key, entry->key, entry->key_len);
    take.len = (uint8_t)entry->key_len;
    take.x = k.x;
    take.z = k.z;
    arrpush(s->keys, take);
    return true;
}

/* Column first, then the key bytes: the sort groups a chunk's keys
   together for the count and leaves the batch order deterministic. */
static int cmp_prune(const void *a, const void *b)
{
    const PruneKey *ka = a;
    const PruneKey *kb = b;
    if (ka->x != kb->x) return ka->x < kb->x ? -1 : 1;
    if (ka->z != kb->z) return ka->z < kb->z ? -1 : 1;
    size_t both = ka->len < kb->len ? ka->len : kb->len;
    int c = memcmp(ka->key, kb->key, both);
    if (c != 0) return c;
    return ka->len == kb->len ? 0 : (ka->len < kb->len ? -1 : 1);
}

static bool prune_bedrock(const McmeWorldInfo *info, const McmeBox *box,
                          int32_t dimension, McmePruneReport *report,
                          McmeWriteRefusal *refusal)
{
    if (info->db_path == NULL) {
        mcme_set_error("'%s' has no Bedrock store",
                       info->path);
        set_refusal(refusal, MCME_WRITE_REFUSAL_NOT_THERE);
        return false;
    }

    /* The write takes the exclusive flock the game takes: a store the
       game has open refuses the whole prune before a key is touched. */
    McmeLevelDb *db = NULL;
    if (!mcme_leveldb_open_rw(&db, info->db_path)) {
        set_refusal(refusal, MCME_WRITE_REFUSAL_STORE);
        return false;               /* open said what the problem is */
    }

    PruneScan scan;
    memset(&scan, 0, sizeof scan);
    box_columns(box, &scan.cx1, &scan.cz1, &scan.cx2, &scan.cz2);
    scan.dimension = dimension;
    mcme_leveldb_iterate(db, scan_key, &scan);

    size_t n = arrlen(scan.keys);
    if (n > 0) {
        qsort(scan.keys, n, sizeof *scan.keys, cmp_prune);

        /* One chunk is one distinct (x, z), whatever keys it owns:
           six subchunk records are one chunk, not six. */
        int64_t chunks = 1;
        for (size_t i = 1; i < n; ++i) {
            if (scan.keys[i].x != scan.keys[i - 1].x
                    || scan.keys[i].z != scan.keys[i - 1].z) {
                ++chunks;
            }
        }

        /* Batches, not one giant record: each is one append, whole or
           not at all (D10). An empty selection writes nothing at all —
           the store's log does not grow for a box already pruned. */
        bool ok = true;
        for (size_t i = 0; i < n && ok; i += PRUNE_BATCH) {
            size_t end = i + PRUNE_BATCH < n ? i + PRUNE_BATCH : n;
            McmeLevelOp *ops = NULL;
            arrsetcap(ops, end - i);
            for (size_t j = i; j < end; ++j) {
                McmeLevelOp op;
                op.key = scan.keys[j].key;
                op.key_len = scan.keys[j].len;
                op.val = NULL;
                op.val_len = 0;
                op.deleted = true;
                arrpush(ops, op);
            }
            ok = mcme_leveldb_write(db, ops, arrlen(ops));
            arrfree(ops);
        }

        if (!ok) {
            set_refusal(refusal, MCME_WRITE_REFUSAL_STORE);
            arrfree(scan.keys);
            mcme_leveldb_close(db);
            return false;
        }
        if (report != NULL) {
            report->chunks = chunks;
            report->keys = (int64_t)n;
        }
    }

    arrfree(scan.keys);
    mcme_leveldb_close(db);
    return true;
}

/* ========================================================================
 * Java: check every file the box reaches, then punch
 * ======================================================================== */

typedef struct {
    int32_t cx, cz;
} PunchSlot;

/* One region file's punches: slots gathered per file so the path is
   stored and counted once. */
typedef struct {
    char       path[MCME_REGION_PATH_MAX];
    PunchSlot *slots;
} PunchFile;

static void punches_free(PunchFile *files)
{
    for (size_t i = 0; i < (size_t)arrlen(files); ++i) {
        arrfree(files[i].slots);
    }
    arrfree(files);
}

static bool prune_java(const McmeWorldInfo *info, const McmeBox *box,
                       int32_t dimension, McmePruneReport *report,
                       McmeWriteRefusal *refusal)
{
    const char *dim = NULL;
    if (!mcme_java_dimension_dir(dimension, &dim)) {
        set_refusal(refusal, MCME_WRITE_REFUSAL_USAGE);
        return false;               /* the message says which number */
    }

    char dir[MCME_REGION_PATH_MAX];
    int written = dim != NULL
        ? snprintf(dir, sizeof dir, "%s/%s/region", info->path, dim)
        : snprintf(dir, sizeof dir, "%s/region", info->path);
    if (written < 0 || (size_t)written >= sizeof dir) {
        mcme_set_error("the path to '%s' is too long",
                       info->path);
        set_refusal(refusal, MCME_WRITE_REFUSAL_INTERNAL);
        return false;
    }
    if (!mcme_fs_exists(dir)) {
        mcme_set_error("there is no region directory at "
                       "'%s' — the game has not made that dimension",
                       dir);
        set_refusal(refusal, MCME_WRITE_REFUSAL_NOT_THERE);
        return false;
    }

    int32_t cx1, cz1, cx2, cz2;
    box_columns(box, &cx1, &cz1, &cx2, &cz2);

    int rx1, rz1, rx2, rz2;
    mcme_region_of((int)cx1, (int)cz1, &rx1, &rz1);
    mcme_region_of((int)cx2, (int)cz2, &rx2, &rz2);

    /* Pass one: every region file the box reaches, opened and read
       before anything is punched. A file that will not read refuses
       the whole prune here, with nothing written — the check fill
       makes, made at the same moment. A file that is not there is
       simply a hole: no chunk, no error. */
    PunchFile *files = NULL;
    int64_t chunks = 0;
    for (int rx = rx1; rx <= rx2; ++rx) {
        for (int rz = rz1; rz <= rz2; ++rz) {
            char path[MCME_REGION_PATH_MAX];
            if (mcme_region_path(path, sizeof path, info->path, dim,
                                 rx, rz) == NULL) {
                mcme_set_error("the path to region "
                               "(%d, %d) is too long", rx, rz);
                set_refusal(refusal, MCME_WRITE_REFUSAL_INTERNAL);
                punches_free(files);
                return false;
            }
            if (!mcme_fs_exists(path)) continue;

            McmeRegion reg;
            if (!mcme_region_open(&reg, path)) {
                set_refusal(refusal, MCME_WRITE_REFUSAL_NOT_READABLE);
                punches_free(files);
                return false;       /* open said what the problem is */
            }

            int32_t fx1 = cx1 > (int32_t)rx * 32 ? cx1 : (int32_t)rx * 32;
            int32_t fz1 = cz1 > (int32_t)rz * 32 ? cz1 : (int32_t)rz * 32;
            int32_t fx2 = cx2 < (int32_t)rx * 32 + 31
                        ? cx2 : (int32_t)rx * 32 + 31;
            int32_t fz2 = cz2 < (int32_t)rz * 32 + 31
                        ? cz2 : (int32_t)rz * 32 + 31;

            PunchFile f;
            memcpy(f.path, path, sizeof f.path);
            f.slots = NULL;
            for (int32_t cx = fx1; cx <= fx2; ++cx) {
                for (int32_t cz = fz1; cz <= fz2; ++cz) {
                    if (!mcme_region_chunk_present(&reg, cx, cz)) continue;
                    PunchSlot slot;
                    slot.cx = cx;
                    slot.cz = cz;
                    arrpush(f.slots, slot);
                    ++chunks;
                }
            }
            if (arrlen(f.slots) > 0) {
                arrpush(files, f);
            } else {
                arrfree(f.slots);
            }
            mcme_region_close(&reg);
        }
    }

    /* Pass two: punch the slots the first pass found. The zeroed
       entry is the format's own advertisement that nothing is stored
       here; the payload bytes stay for `region compact` (D13). */
    bool ok = true;
    int64_t touched = 0;
    for (size_t i = 0; i < (size_t)arrlen(files) && ok; ++i) {
        bool any = false;
        for (size_t j = 0; j < (size_t)arrlen(files[i].slots); ++j) {
            if (!mcme_region_remove_chunk(files[i].path,
                                          (int)files[i].slots[j].cx,
                                          (int)files[i].slots[j].cz,
                                          refusal)) {
                ok = false;
                break;
            }
            any = true;
        }
        if (any) ++touched;
    }
    punches_free(files);

    if (!ok) return false;          /* the refusal says what and where */
    if (report != NULL) {
        report->chunks = chunks;
        report->region_files = touched;
    }
    return true;
}

/* ========================================================================
 * The verb: probe, then whichever half answers
 * ======================================================================== */

bool mcme_prune(const char *world, const McmeBox *box, int32_t dimension,
                McmePruneReport *report, McmeWriteRefusal *refusal)
{
    set_refusal(refusal, MCME_WRITE_REFUSAL_NONE);
    if (report != NULL) {
        report->chunks = 0;
        report->keys = 0;
        report->region_files = 0;
    }

    if (world == NULL || box == NULL) {
        mcme_set_error("null argument");
        set_refusal(refusal, MCME_WRITE_REFUSAL_USAGE);
        return false;
    }

    McmeWorldInfo info;
    if (!mcme_world_probe(&info, world)) {
        mcme_set_error("'%s' is not a world", world);
        set_refusal(refusal, MCME_WRITE_REFUSAL_NOT_THERE);
        return false;
    }
    if (info.kind == MCME_WORLD_BEDROCK && info.db_path != NULL) {
        bool ok = prune_bedrock(&info, box, dimension, report, refusal);
        mcme_world_info_free(&info);
        return ok;
    }
    if (info.kind == MCME_WORLD_JAVA) {
        bool ok = prune_java(&info, box, dimension, report, refusal);
        mcme_world_info_free(&info);
        return ok;
    }

    mcme_set_error("'%s' is not a Bedrock or Java world",
                   world);
    set_refusal(refusal, MCME_WRITE_REFUSAL_NOT_THERE);
    mcme_world_info_free(&info);
    return false;
}
