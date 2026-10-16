#include "mcme/bedrock_chunk.h"
#include "mcme/bedrock_entity.h"
#include "mcme/bedrock_keys.h"
#include "mcme/bedrock_write.h"
#include "mcme/core.h"
#include "mcme/java_chunk.h"
#include "mcme/compress.h"
#include "mcme/convert.h"
#include "mcme/leveldb.h"
#include "mcme/nbt.h"
#include "mcme/prune.h"
#include "mcme/region.h"
#include "mcme/translate.h"
#include "mcme/world.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/*
 * World-format self-tests: the Java Anvil region reader.
 *
 * tests/golden/worlds/java is a world written by amulet-core 1.9.50 (see
 * tools/ref_worlds.py) and tests/golden/world is that world's chunk payloads
 * re-serialised by amulet_nbt 2.1.8 (see tools/ref_region.py). The manifest
 * lists every chunk with its coordinates, codec and uncompressed NBT size.
 *
 * For each chunk the test does the same thing the tool will: read the sector
 * table, decompress the payload, then parse and re-serialise the NBT. The
 * parsed tree is written back to binary big-endian and must reproduce the
 * reference's canonical bytes exactly, and must produce identical compact
 * SNBT. That is the Phase 2 discipline carried into the container layer —
 * proving the sector arithmetic and the zlib framing land on the same document
 * the reference saw, not merely on some document.
 *
 * Absent chunks, absent files, truncated files and bad compression bytes are
 * all exercised as refusal paths, because a map editor gets pointed at broken
 * worlds by its users on purpose.
 *
 * Skips rather than fails when the golden data is missing, so `mcme selftest`
 * remains runnable from any directory.
 */

static int g_failed;

#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) {                                                         \
            g_failed++;                                                        \
            printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);           \
        }                                                                      \
    } while (0)

#define CHECK_MSG(cond, ...)                                                   \
    do {                                                                       \
        if (!(cond)) {                                                         \
            g_failed++;                                                        \
            printf("  FAIL %s:%d  %s\n    ", __FILE__, __LINE__, #cond);       \
            printf(__VA_ARGS__);                                               \
            printf("\n");                                                      \
        }                                                                      \
    } while (0)

static const char kWorldDir[] = "tests/golden/worlds/java";
static const char kChunkDir[] = "tests/golden/world";

/* ========================================================================
 * Manifest
 * ======================================================================== */

typedef struct {
    char     region[64];   /* "r.0.0" */
    int      cx, cz;
    char     codec[16];
    size_t   raw_len;
} ChunkEntry;

static size_t load_manifest(ChunkEntry *out, size_t cap)
{
    char path[512];
    snprintf(path, sizeof path, "%s/MANIFEST.txt", kChunkDir);

    FILE *fh = fopen(path, "r");
    if (fh == NULL) return 0;

    size_t n = 0;
    char line[512];
    while (n < cap && fgets(line, sizeof line, fh) != NULL) {
        if (line[0] == '#' || line[0] == '\n') continue;

        ChunkEntry e;
        memset(&e, 0, sizeof e);
        unsigned long raw = 0;
        if (sscanf(line, "%63s %d %d %15s %lu",
                   e.region, &e.cx, &e.cz, e.codec, &raw) != 5) {
            continue;
        }
        e.raw_len = (size_t)raw;
        out[n++] = e;
    }
    fclose(fh);
    return n;
}

/* Reads a file and returns malloc'd bytes, or NULL. */
static uint8_t *slurp(const char *path, size_t *out_len)
{
    void *data = NULL;
    size_t len = 0;
    if (!mcme_fs_read(path, &data, &len)) return NULL;
    *out_len = len;
    return (uint8_t *)data;
}

/* ========================================================================
 * Container structure
 * ======================================================================== */

static void test_container(void)
{
    printf("  container\n");

    /* r.0.0 holds the chunks at (0,0), (1,0), (0,1) and (31,31); r.-1.-1 holds
       (-1,-1). The split is the point of the fixture: it proves the sharding
       rule floor(chunk / 32) rather than just the slot arithmetic. */
    static const struct {
        const char *name;
        size_t      chunks;
        int         slot_x, slot_z;
    } regions[] = {
        { "r.0.0",     4, 0,  0  },
        { "r.-1.-1",   1, 31, 31 },
    };

    for (size_t i = 0; i < sizeof regions / sizeof regions[0]; ++i) {
        char path[512];
        snprintf(path, sizeof path, "%s/region/%s.mca", kWorldDir, regions[i].name);

        McmeRegion reg;
        if (!mcme_region_open(&reg, path)) {
            printf("  SKIP %s (%s)\n", regions[i].name, mcme_last_error());
            continue;
        }

        CHECK_MSG(mcme_region_chunk_count(&reg) == regions[i].chunks,
                  "%s holds %zu chunks, expected %zu",
                  regions[i].name, mcme_region_chunk_count(&reg), regions[i].chunks);

        /* Chunk (31,31) sits in the last slot of r.0.0 and, because of the
           floor division, the same slot number in r.-1.-1 for chunk (-1,-1).
           Both being present in their respective files is what proves the
           region coordinate is derived from the chunk coordinate correctly. */
        CHECK(mcme_region_chunk_present(&reg, regions[i].slot_x, regions[i].slot_z));
        CHECK(!mcme_region_chunk_present(&reg, 31 - regions[i].slot_x, regions[i].slot_z));

        McmeChunkLoc loc = mcme_region_chunk_loc(&reg, regions[i].slot_x,
                                                 regions[i].slot_z);
        CHECK(loc.sector_offset >= 2);  /* the two header sectors are never used */
        CHECK(loc.sector_count > 0);

        mcme_region_close(&reg);
    }

    /* The sharding rule itself: floor(chunk / 32), so chunk -1 lands in
       region -1, where slot_for's mask finds it at slot 31 — the pair
       that makes (31, 31) in r.-1.-1 mean chunk (-1, -1). C division
       would truncate that to region 0. */
    {
        static const struct {
            int cx, cz, rx, rz;
        } shards[] = {
            {   0,   0,  0,  0 },
            {  31,  31,  0,  0 },
            {  32,   0,  1,  0 },
            {  -1,  -1, -1, -1 },
            { -32, -32, -1, -1 },
            { -33, -32, -2, -1 },
        };

        for (size_t i = 0; i < sizeof shards / sizeof shards[0]; ++i) {
            int rx = 99, rz = 99;
            mcme_region_of(shards[i].cx, shards[i].cz, &rx, &rz);
            CHECK_MSG(rx == shards[i].rx && rz == shards[i].rz,
                      "region of chunk (%d, %d) is (%d, %d), want (%d, %d)",
                      shards[i].cx, shards[i].cz, rx, rz,
                      shards[i].rx, shards[i].rz);
        }
        mcme_region_of(7, -9, NULL, NULL);  /* either axis may be skipped */
    }

    /* Opening a directory, a missing file, and a file too short to hold the
       header must all fail cleanly. */
    {
        McmeRegion reg;
        CHECK(!mcme_region_open(&reg, "tests/golden/worlds/java/region"));
        CHECK(!mcme_region_open(&reg, "tests/golden/worlds/java/region/r.9.9.mca"));
        CHECK(!mcme_region_open(&reg, "does/not/exist.mca"));
        CHECK(!mcme_region_open(&reg, NULL));
        CHECK(!mcme_region_open(NULL, "x"));

        /* A real region file truncated to a single byte must fail rather than
           be read as a header. */
        char path[512];
        snprintf(path, sizeof path, "%s/region/r.0.0.mca", kWorldDir);
        size_t full_len = 0;
        uint8_t *full = slurp(path, &full_len);
        if (full != NULL) {
            char tmp[] = "build/truncated.mca";
            CHECK(mcme_fs_write(tmp, full, 1));
            CHECK(!mcme_region_open(&reg, tmp));

            /* An all-zero header is a structurally valid empty region: every
               slot advertises "no chunk here", so it must open and report zero
               chunks. Note the payload is zeros rather than the real file's
               first 8 KiB — the real header carries live offsets, which after
               truncation would correctly be rejected as pointing past the end. */
            static const uint8_t empty_header[MCME_REGION_HEADER];
            CHECK(mcme_fs_write(tmp, empty_header, sizeof empty_header));
            CHECK(mcme_region_open(&reg, tmp));
            CHECK(mcme_region_chunk_count(&reg) == 0);
            mcme_region_close(&reg);

            /* Same file with one live offset pointing into the missing body:
               the open-time validation must reject it. */
            uint8_t bad_header[MCME_REGION_HEADER];
            memcpy(bad_header, empty_header, sizeof bad_header);
            bad_header[0] = 0x00; bad_header[1] = 0x00; bad_header[2] = 0x03;
            bad_header[3] = 0x01;                       /* slot 0 at sector 3 */
            CHECK(mcme_fs_write(tmp, bad_header, sizeof bad_header));
            CHECK(!mcme_region_open(&reg, tmp));

            remove(tmp);
            free(full);
        }
    }

    /* Refusing to read from a region that was never opened, or from an absent
       slot, is a caller error worth catching here rather than in the field. */
    {
        McmeRegion reg;
        memset(&reg, 0, sizeof reg);
        McmeBuffer out;
        mcme_buffer_init(&out);
        CHECK(!mcme_region_read_chunk(&reg, 0, 0, NULL, &out));
        CHECK(!mcme_region_read_chunk(&reg, 0, 0, NULL, NULL));
        mcme_buffer_free(&out);
    }

    /* The five-byte payload header the inventory reads, pinned to what the
       reference wrote: each chunk's own length, all zlib, the file's own
       timestamp — every answer taken without decompressing a byte. */
    {
        static const struct {
            const char *file;
            int         cx, cz;
            uint32_t    length;
        } payloads[] = {
            { "r.0.0",     0,  0, 248 },
            { "r.0.0",     1,  0, 252 },
            { "r.0.0",     0,  1, 252 },
            { "r.0.0",    31, 31, 251 },
            { "r.-1.-1",  31, 31, 250 },
        };

        for (size_t i = 0; i < sizeof payloads / sizeof payloads[0]; ++i) {
            char path[512];
            snprintf(path, sizeof path,
                     "tests/golden/worlds/java/region/%s.mca",
                     payloads[i].file);
            McmeRegion reg;
            CHECK(mcme_region_open(&reg, path));

            McmeChunkPayload p;
            CHECK(mcme_region_chunk_payload(&reg, payloads[i].cx,
                                            payloads[i].cz, &p));
            CHECK(p.length == payloads[i].length);
            CHECK(p.compression == MCME_ANVIL_COMP_ZLIB);
            CHECK(mcme_region_chunk_timestamp(&reg, payloads[i].cx,
                                              payloads[i].cz) == 1791643789u);

            /* A slot neither file fills holds nothing to report. */
            CHECK(!mcme_region_chunk_payload(&reg, 16, 16, &p));
            mcme_region_close(&reg);
        }
    }

    /* The header read's own refusals: caller errors, a length of zero, a
       payload past the file's end, one past the region limit — and the
       answer it must NOT refuse: a compression byte mcme does not know is
       the file's word, and the inventory reports it. */
    {
        McmeRegion reg;
        McmeChunkPayload p;
        CHECK(!mcme_region_chunk_payload(NULL, 0, 0, &p));
        memset(&reg, 0, sizeof reg);
        CHECK(!mcme_region_chunk_payload(&reg, 0, 0, &p));  /* never opened */
        CHECK(mcme_region_open(&reg,
                               "tests/golden/worlds/java/region/r.0.0.mca"));
        CHECK(!mcme_region_chunk_payload(&reg, 0, 0, NULL));
        mcme_region_close(&reg);

        char tmp[] = "build/header.mca";
        uint8_t file[MCME_REGION_HEADER + 2u * MCME_REGION_SECTOR];
        memset(file, 0, sizeof file);
        file[0] = 0; file[1] = 0; file[2] = 2;   /* slot (0, 0) at sector 2 */
        file[3] = 1;                             /* occupying one sector */

        /* Zeros at the payload's place: a length of 0. */
        CHECK(mcme_fs_write(tmp, file, sizeof file));
        CHECK(mcme_region_open(&reg, tmp));
        CHECK(!mcme_region_chunk_payload(&reg, 0, 0, &p));
        mcme_region_close(&reg);

        /* 65536 declared in a file holding 8192 bytes after the header:
           the payload would run past the end, so the header describes no
           such chunk. */
        file[MCME_REGION_HEADER + 0] = 0x00;
        file[MCME_REGION_HEADER + 1] = 0x01;
        file[MCME_REGION_HEADER + 2] = 0x00;
        file[MCME_REGION_HEADER + 3] = 0x00;
        CHECK(mcme_fs_write(tmp, file, sizeof file));
        CHECK(mcme_region_open(&reg, tmp));
        CHECK(!mcme_region_chunk_payload(&reg, 0, 0, &p));
        mcme_region_close(&reg);

        /* Past the region limit: refused on the claim alone, before the
           file's own size is even consulted. */
        file[MCME_REGION_HEADER + 0] = 0x02;
        file[MCME_REGION_HEADER + 1] = 0x00;
        file[MCME_REGION_HEADER + 2] = 0x00;
        file[MCME_REGION_HEADER + 3] = 0x01;
        CHECK(mcme_fs_write(tmp, file, sizeof file));
        CHECK(mcme_region_open(&reg, tmp));
        CHECK(!mcme_region_chunk_payload(&reg, 0, 0, &p));
        mcme_region_close(&reg);

        /* Ten bytes claiming compression 7 — a byte the format does not
           name: reported as it is, because an inventory answers what the
           file holds, not what mcme can open. */
        file[MCME_REGION_HEADER + 0] = 0x00;
        file[MCME_REGION_HEADER + 1] = 0x00;
        file[MCME_REGION_HEADER + 2] = 0x00;
        file[MCME_REGION_HEADER + 3] = 0x0a;
        file[MCME_REGION_HEADER + 4] = 7;
        CHECK(mcme_fs_write(tmp, file, sizeof file));
        CHECK(mcme_region_open(&reg, tmp));
        CHECK(mcme_region_chunk_payload(&reg, 0, 0, &p));
        CHECK(p.length == 9);
        CHECK(p.compression == 7);
        mcme_region_close(&reg);

        remove(tmp);
    }
}

/* ========================================================================
 * Chunk payloads against the reference
 * ======================================================================== */

static void test_chunks(void)
{
    printf("  chunk payloads (amulet-core world, amulet_nbt re-serialisation)\n");

    ChunkEntry entries[512];
    size_t count = load_manifest(entries, sizeof entries / sizeof entries[0]);
    if (count == 0) {
        printf("  SKIP chunk payloads (no %s/MANIFEST.txt)\n", kChunkDir);
        return;
    }

    /* Cache each region file open across its chunks so the test reads the
       container the way the tool will, not once per chunk. */
    McmeRegion reg;
    bool reg_open = false;
    char open_region[64];
    open_region[0] = '\0';

    for (size_t i = 0; i < count; ++i) {
        ChunkEntry *e = &entries[i];

        if (!reg_open || strcmp(open_region, e->region) != 0) {
            if (reg_open) mcme_region_close(&reg);
            char path[512];
            int sn = snprintf(path, sizeof path,
                              "%s/region/%s.mca", kWorldDir, e->region);
            CHECK(sn > 0 && (size_t)sn < sizeof path);
            if (!mcme_region_open(&reg, path)) {
                printf("  SKIP %s (%s)\n", e->region, mcme_last_error());
                continue;
            }
            reg_open = true;
            sn = snprintf(open_region, sizeof open_region, "%s",
                          e->region);
            CHECK(sn > 0 && (size_t)sn < sizeof open_region);
        }

        McmeCompression codec = MCME_COMPRESS_UNKNOWN;
        McmeBuffer raw;
        mcme_buffer_init(&raw);
        bool ok = mcme_region_read_chunk(&reg, e->cx, e->cz, &codec, &raw);
        CHECK_MSG(ok, "%s chunk (%d, %d) failed to read: %s",
                  e->region, e->cx, e->cz, mcme_last_error());
        if (!ok) {
            mcme_buffer_free(&raw);
            continue;
        }

        CHECK_MSG(raw.len == e->raw_len,
                  "%s chunk (%d, %d) inflated to %zu bytes, expected %zu",
                  e->region, e->cx, e->cz, raw.len, e->raw_len);

        /* The manifest records which codec the reference found; we must have
           chosen the same one from the 5-byte chunk header. */
        McmeCompression want = MCME_COMPRESS_UNKNOWN;
        if (strcmp(e->codec, "zlib") == 0) want = MCME_COMPRESS_ZLIB;
        else if (strcmp(e->codec, "gzip") == 0) want = MCME_COMPRESS_GZIP;
        else if (strcmp(e->codec, "store") == 0) want = MCME_COMPRESS_STORE;
        CHECK_MSG(codec == want, "%s chunk (%d, %d) used %s, expected %s",
                  e->region, e->cx, e->cz,
                  mcme_compression_name(codec), mcme_compression_name(want));

        /* Parse, then re-serialise both ways and compare against the
           reference's own output. */
        McmeArena *arena = mcme_arena_new();
        McmeNbtDoc doc;
        ok = mcme_nbt_parse(&doc, arena, raw.data, raw.len, false /* big endian */);
        CHECK_MSG(ok, "%s chunk (%d, %d) failed to parse as NBT: %s",
                  e->region, e->cx, e->cz, mcme_last_error());

        if (ok) {
            char base[256];
            int sn = snprintf(base, sizeof base, "%s/%s.%d.%d",
                              kChunkDir, e->region, e->cx, e->cz);
            CHECK(sn > 0 && (size_t)sn < sizeof base);

            char nbt_path[512];
            snprintf(nbt_path, sizeof nbt_path, "%s.nbt", base);
            size_t want_len = 0;
            uint8_t *want_nbt = slurp(nbt_path, &want_len);
            if (want_nbt == NULL) {
                printf("  SKIP %s.nbt\n", nbt_path);
            } else {
                McmeBuffer got;
                mcme_buffer_init(&got);
                CHECK(mcme_nbt_write_binary(&doc, &got));
                CHECK_MSG(got.len == want_len &&
                          memcmp(got.data, want_nbt, want_len) == 0,
                          "%s chunk (%d, %d) re-serialised to %zu bytes, "
                          "expected %zu and identical bytes",
                          e->region, e->cx, e->cz, got.len, want_len);
                mcme_buffer_free(&got);
                free(want_nbt);
            }

            char snbt_path[512];
            snprintf(snbt_path, sizeof snbt_path, "%s.snbt", base);
            char *want_snbt = mcme_fs_read_text(snbt_path);
            if (want_snbt == NULL) {
                printf("  SKIP %s.snbt\n", snbt_path);
            } else {
                /* The reference writes compact SNBT with a trailing newline. */
                size_t want_snbt_len = strlen(want_snbt);
                while (want_snbt_len > 0 &&
                       (want_snbt[want_snbt_len - 1] == '\n' ||
                        want_snbt[want_snbt_len - 1] == '\r')) {
                    want_snbt[--want_snbt_len] = '\0';
                }

                McmeBuffer got;
                mcme_buffer_init(&got);
                CHECK(mcme_nbt_write_snbt(doc.root, &got, 0));
                CHECK_MSG(got.len == want_snbt_len &&
                          memcmp(got.data, want_snbt, want_snbt_len) == 0,
                          "%s chunk (%d, %d) SNBT differs from the reference\n"
                          "    got  %.*s\n    want %s",
                          e->region, e->cx, e->cz,
                          (int)got.len, (const char *)got.data, want_snbt);
                mcme_buffer_free(&got);
                free(want_snbt);
            }
        }

        mcme_arena_free(arena);
        mcme_buffer_free(&raw);
    }

    if (reg_open) mcme_region_close(&reg);
}

/* ========================================================================
 * Bedrock chunk keys
 * ======================================================================== */

static const char kForgeWorld[] = "tests/golden/worlds/bedrock-forge";

/* ========================================================================
 * Writing a chunk back — append, repoint, touch nothing else
 * ======================================================================== */

/* Opens a region file and pins the chunk at (cx, cz): present, stamped
   with `stamp`, compressed the way Java writes it, and decoding to a
   document whose DataVersion is `version`. */
static void check_chunk_written(const char *file, int cx, int cz,
                                int32_t version, uint32_t stamp)
{
    McmeRegion reg;
    if (!mcme_region_open(&reg, file)) {
        CHECK_MSG(false, "reopen %s: %s", file, mcme_last_error());
        return;
    }
    CHECK_MSG(mcme_region_chunk_present(&reg, cx, cz),
              "%s holds no chunk at (%d, %d)", file, cx, cz);
    CHECK_MSG(mcme_region_chunk_timestamp(&reg, cx, cz) == stamp,
              "%s chunk (%d, %d) timestamp %u, want %u",
              file, cx, cz, mcme_region_chunk_timestamp(&reg, cx, cz), stamp);

    McmeChunkPayload p;
    if (mcme_region_chunk_payload(&reg, cx, cz, &p)) {
        CHECK_MSG(p.compression == MCME_ANVIL_COMP_ZLIB,
                  "%s chunk (%d, %d) compression %u, want zlib",
                  file, cx, cz, p.compression);
    } else {
        CHECK_MSG(false, "%s chunk (%d, %d): %s",
                  file, cx, cz, mcme_last_error());
    }

    McmeArena *arena = mcme_arena_new();
    if (arena == NULL) {
        CHECK_MSG(false, "out of memory");
        mcme_region_close(&reg);
        return;
    }

    McmeBuffer nbt;
    mcme_buffer_init(&nbt);
    if (!mcme_region_read_chunk(&reg, cx, cz, NULL, &nbt)) {
        CHECK_MSG(false, "%s chunk (%d, %d): %s",
                  file, cx, cz, mcme_last_error());
    } else {
        McmeNbtDoc back;
        if (!mcme_nbt_parse(&back, arena, nbt.data, nbt.len, false)) {
            CHECK_MSG(false, "%s chunk (%d, %d) will not decode: %s",
                      file, cx, cz, mcme_last_error());
        } else {
            const McmeTag *v = mcme_nbt_path(back.root, "DataVersion");
            CHECK_MSG(v != NULL && v->type == MCME_TAG_Int
                      && v->as_int == version,
                      "%s chunk (%d, %d) DataVersion %d, want %d",
                      file, cx, cz, v != NULL ? v->as_int : -1, version);
        }
    }
    mcme_buffer_free(&nbt);
    mcme_arena_free(arena);
    mcme_region_close(&reg);
}

static void test_region_write(void)
{
    printf("  region chunk write (append, repoint, byte fidelity)\n");

    /* The golden region, copied: every write verb runs against a
       scratch file, never the fixture itself. */
    char src[512];
    char dst[] = "build/w.mca";
    snprintf(src, sizeof src, "%s/region/r.0.0.mca", kWorldDir);
    void *golden;
    size_t golden_len;
    if (!mcme_fs_read(src, &golden, &golden_len)) {
        printf("  SKIP region write (cannot read %s)\n", src);
        return;
    }
    CHECK(mcme_fs_write(dst, golden, golden_len));

    McmeArena *arena = mcme_arena_new();
    CHECK(arena != NULL);
    if (arena == NULL) {
        free(golden);
        remove(dst);
        return;
    }

    /* Slot (2, 2) is 66: the fixture's chunks sit at slots 0, 1, 32 and
       1023, so the new entry (bytes 264..267) and timestamp (4360..4363)
       are theirs nowhere. */
    static const size_t kEntryAt = 264;
    static const size_t kStampAt = 4096 + 264;
    static const char kDoc[] = "{DataVersion: 7, xPos: 3, zPos: -4}";
    static const char kDoc2[] = "{DataVersion: 9}";

    McmeTag *doc = NULL;
    CHECK(mcme_nbt_parse_snbt(arena, kDoc, sizeof kDoc - 1, &doc));

    McmeWriteRefusal refusal = MCME_WRITE_REFUSAL_INTERNAL;
    CHECK(mcme_region_set_chunk(dst, 2, 2, doc, arena,
                                0x01020304u, &refusal));
    CHECK(refusal == MCME_WRITE_REFUSAL_NONE);
    check_chunk_written(dst, 2, 2, 7, 0x01020304u);

    /* Every byte outside the slot's entry, its timestamp and the
       appended tail is the golden file, bit for bit. */
    void *now;
    size_t now_len;
    CHECK(mcme_fs_read(dst, &now, &now_len));
    CHECK(now_len > golden_len);
    CHECK(memcmp(now, golden, kEntryAt) == 0);
    CHECK(memcmp((char *)now + kEntryAt + 4, (char *)golden + kEntryAt + 4,
                 kStampAt - kEntryAt - 4) == 0);
    CHECK(memcmp((char *)now + kStampAt + 4, (char *)golden + kStampAt + 4,
                 golden_len - kStampAt - 4) == 0);

    /* Writing the same chunk again moves the payload further out: the
       first write's tail is slack now, the count stays at five, and the
       other slots are still the golden file. */
    McmeTag *doc2 = NULL;
    CHECK(mcme_nbt_parse_snbt(arena, kDoc2, sizeof kDoc2 - 1, &doc2));
    CHECK(mcme_region_set_chunk(dst, 2, 2, doc2, arena,
                                0x0A0B0C0Du, &refusal));
    check_chunk_written(dst, 2, 2, 9, 0x0A0B0C0Du);

    McmeRegion reg;
    CHECK(mcme_region_open(&reg, dst));
    CHECK(mcme_region_chunk_count(&reg) == 5);
    mcme_region_close(&reg);

    free(now);
    CHECK(mcme_fs_read(dst, &now, &now_len));
    CHECK(memcmp(now, golden, kEntryAt) == 0);
    CHECK(memcmp((char *)now + kEntryAt + 4, (char *)golden + kEntryAt + 4,
                 kStampAt - kEntryAt - 4) == 0);
    CHECK(memcmp((char *)now + kStampAt + 4, (char *)golden + kStampAt + 4,
                 golden_len - kStampAt - 4) == 0);
    free(now);

    /* A region file that does not exist is created with its 8 KiB
       header, one chunk in it. */
    char fresh[] = "build/w_new.mca";
    CHECK(mcme_region_set_chunk(fresh, 0, 0, doc, arena, 1u, &refusal));
    check_chunk_written(fresh, 0, 0, 7, 1u);
    CHECK(mcme_region_open(&reg, fresh));
    CHECK(mcme_region_chunk_count(&reg) == 1);
    mcme_region_close(&reg);

    /* Every refusal the library promises. */
    CHECK(!mcme_region_set_chunk(NULL, 0, 0, doc, arena, 0, &refusal));
    CHECK(refusal == MCME_WRITE_REFUSAL_USAGE);
    CHECK(!mcme_region_set_chunk(dst, 0, 0, NULL, arena, 0, &refusal));
    CHECK(refusal == MCME_WRITE_REFUSAL_USAGE);
    CHECK(!mcme_region_set_chunk(dst, 0, 0, doc, NULL, 0, &refusal));
    CHECK(refusal == MCME_WRITE_REFUSAL_USAGE);

    static const char kList[] = "[1, 2]";
    McmeTag *list = NULL;
    CHECK(mcme_nbt_parse_snbt(arena, kList, sizeof kList - 1, &list));
    CHECK(!mcme_region_set_chunk(dst, 0, 0, list, arena, 0, &refusal));
    CHECK(refusal == MCME_WRITE_REFUSAL_USAGE);

    CHECK(!mcme_region_set_chunk("build/w_nodir_x/r.0.0.mca", 0, 0,
                                 doc, arena, 0, &refusal));
    CHECK(refusal == MCME_WRITE_REFUSAL_NOT_THERE);

    char junk[] = "build/w_bad.mca";
    CHECK(mcme_fs_write(junk, "junkjunk", 8));
    CHECK(!mcme_region_set_chunk(junk, 0, 0, doc, arena, 0, &refusal));
    CHECK(refusal == MCME_WRITE_REFUSAL_NOT_READABLE);

    /* A slot's sector count is one byte, so 255 sectors is the largest
       chunk a region can claim: a document whose encoded payload runs
       past that is refused, not truncated into one the reader would
       misjudge. Built through the tag API — NBT strings cap at 65535
       bytes, so the size has to come from an array. */
    {
        size_t n = 500000;
        int32_t *ints = malloc(n * sizeof *ints);
        CHECK(ints != NULL);
        if (ints != NULL) {
            uint32_t state = 0x12345678u;
            for (size_t i = 0; i < n; ++i) {
                state = state * 1664525u + 1013904223u;   /* LCG */
                ints[i] = (int32_t)state;
            }
            McmeTag *big = mcme_nbt_new(arena, MCME_TAG_Compound);
            McmeTag *arr = mcme_nbt_new(arena, MCME_TAG_Int_Array);
            CHECK(big != NULL && arr != NULL);
            CHECK(mcme_nbt_set_ints(arena, arr, ints, n));
            CHECK(mcme_nbt_compound_init(arena, big, 1));
            CHECK(mcme_nbt_compound_put(arena, big, "pad", 3, arr));
            CHECK(!mcme_region_set_chunk(dst, 5, 5, big, arena, 0, &refusal));
            CHECK(refusal == MCME_WRITE_REFUSAL_USAGE);
        }
        free(ints);
    }

    mcme_arena_free(arena);
    free(golden);
    remove(dst);
    remove(fresh);
    remove(junk);
}

/* ========================================================================
 * Compaction — the slack every append leaves, gone
 * ======================================================================== */

static void test_region_compact(void)
{
    printf("  region compact (pack, preserve, idempotent)\n");

    /* The golden region copied, then written twice in one slot, the
       way test_region_write leaves it: a dead payload version and the
       slack it sits in. */
    char src[512];
    char dst[] = "build/c.mca";
    snprintf(src, sizeof src, "%s/region/r.0.0.mca", kWorldDir);
    void *golden;
    size_t golden_len;
    if (!mcme_fs_read(src, &golden, &golden_len)) {
        printf("  SKIP region compact (cannot read %s)\n", src);
        return;
    }
    CHECK(mcme_fs_write(dst, golden, golden_len));
    free(golden);

    McmeArena *arena = mcme_arena_new();
    CHECK(arena != NULL);
    if (arena == NULL) {
        remove(dst);
        return;
    }
    static const char kDoc[] = "{DataVersion: 7, xPos: 3, zPos: -4}";
    static const char kDoc2[] = "{DataVersion: 9}";
    McmeTag *doc = NULL;
    McmeTag *doc2 = NULL;
    CHECK(mcme_nbt_parse_snbt(arena, kDoc, sizeof kDoc - 1, &doc));
    CHECK(mcme_nbt_parse_snbt(arena, kDoc2, sizeof kDoc2 - 1, &doc2));

    McmeWriteRefusal refusal = MCME_WRITE_REFUSAL_INTERNAL;
    CHECK(mcme_region_set_chunk(dst, 2, 2, doc, arena, 0x01020304u, &refusal));
    CHECK(mcme_region_set_chunk(dst, 2, 2, doc2, arena, 0x0A0B0C0Du, &refusal));

    /* Snapshot what the file holds before it is replaced: for every
       slot, its frame's place, size and stamp. */
    uint32_t old_off[MCME_REGION_ENTRIES];
    size_t old_len[MCME_REGION_ENTRIES];
    uint32_t old_ts[MCME_REGION_ENTRIES];
    memset(old_len, 0, sizeof old_len);
    McmeRegion reg;
    CHECK(mcme_region_open(&reg, dst));
    for (size_t slot = 0; slot < MCME_REGION_ENTRIES; ++slot) {
        int cx = (int)(slot % 32);
        int cz = (int)(slot / 32);
        if (!mcme_region_chunk_present(&reg, cx, cz)) continue;
        McmeChunkPayload p;
        McmeChunkLoc loc = mcme_region_chunk_loc(&reg, cx, cz);
        CHECK(mcme_region_chunk_payload(&reg, cx, cz, &p));
        old_off[slot] = loc.sector_offset;
        old_len[slot] = 5u + p.length;
        old_ts[slot] = mcme_region_chunk_timestamp(&reg, cx, cz);
    }
    mcme_region_close(&reg);

    void *before;
    size_t before_len;
    CHECK(mcme_fs_read(dst, &before, &before_len));

    McmeRegionCompact summary;
    CHECK(mcme_region_compact(dst, &summary, &refusal));
    CHECK(refusal == MCME_WRITE_REFUSAL_NONE);

    /* Five chunks carried, the file smaller than the slack made it. */
    CHECK(summary.chunks == 5);
    CHECK(summary.before == before_len);
    CHECK(summary.after < before_len);

    void *after;
    size_t after_len;
    CHECK(mcme_fs_read(dst, &after, &after_len));
    CHECK(after_len == summary.after);

    /* The packed layout: slots in order from sector 2, the counts the
       payloads' own sizes imply, the stamps carried over, and every
       frame's bytes the ones the old file held. */
    {
        McmeRegion fresh_reg;
        CHECK(mcme_region_open(&fresh_reg, dst));
        size_t at = 2;
        size_t held = 0;
        for (size_t slot = 0; slot < MCME_REGION_ENTRIES; ++slot) {
            int cx = (int)(slot % 32);
            int cz = (int)(slot / 32);
            if (old_len[slot] == 0) {
                CHECK_MSG(!mcme_region_chunk_present(&fresh_reg, cx, cz),
                          "compacted file holds slot %zu, which the old "
                          "one did not", slot);
                continue;
            }
            McmeChunkLoc loc = mcme_region_chunk_loc(&fresh_reg, cx, cz);
            CHECK_MSG(loc.sector_offset == at,
                      "slot %zu at sector %u, want %zu", slot,
                      loc.sector_offset, at);
            CHECK_MSG(loc.sector_count == (old_len[slot] + 4095u) / 4096u,
                      "slot %zu claims %u sectors, its frame needs %zu",
                      slot, loc.sector_count,
                      (old_len[slot] + 4095u) / 4096u);
            CHECK_MSG(mcme_region_chunk_timestamp(&fresh_reg, cx, cz)
                          == old_ts[slot],
                      "slot %zu stamp %u, want %u", slot,
                      mcme_region_chunk_timestamp(&fresh_reg, cx, cz),
                      old_ts[slot]);
            CHECK(memcmp((char *)after + at * MCME_REGION_SECTOR,
                         (char *)before
                             + (size_t)old_off[slot] * MCME_REGION_SECTOR,
                         old_len[slot]) == 0);
            at += loc.sector_count;
            ++held;
        }
        mcme_region_close(&fresh_reg);
        CHECK(held == 5);
        CHECK(at * MCME_REGION_SECTOR == after_len);
    }

    /* The last write wins, and the golden chunks came through
       untouched: what read_chunk inflates is unchanged. */
    check_chunk_written(dst, 2, 2, 9, 0x0A0B0C0Du);
    check_chunk_written(dst, 0, 0, 3463, 1791643789u);
    check_chunk_written(dst, 31, 31, 3463, 1791643789u);

    /* Compacting the compacted file changes no byte. */
    char again[] = "build/c_again.mca";
    CHECK(mcme_fs_write(again, after, after_len));
    McmeRegionCompact summary2;
    CHECK(mcme_region_compact(again, &summary2, &refusal));
    CHECK(summary2.chunks == 5);
    CHECK(summary2.after == after_len);
    void *twice;
    size_t twice_len;
    CHECK(mcme_fs_read(again, &twice, &twice_len));
    CHECK(twice_len == after_len);
    CHECK(memcmp(twice, after, after_len) == 0);
    free(twice);
    free(after);
    free(before);

    /* Slot order, not append order: the chunk written first sits at
       the higher slot, so packing moves it — and its stamp moves
       with it. */
    {
        char hole[] = "build/c_hole.mca";
        remove(hole);
        CHECK(mcme_region_set_chunk(hole, 31, 31, doc, arena,
                                    0x11111111u, &refusal));
        CHECK(mcme_region_set_chunk(hole, 0, 0, doc2, arena,
                                    0x22222222u, &refusal));
        CHECK(mcme_region_compact(hole, NULL, &refusal));
        CHECK(refusal == MCME_WRITE_REFUSAL_NONE);

        McmeRegion hr;
        CHECK(mcme_region_open(&hr, hole));
        McmeChunkLoc l0 = mcme_region_chunk_loc(&hr, 0, 0);
        McmeChunkLoc l1 = mcme_region_chunk_loc(&hr, 31, 31);
        CHECK(l0.sector_offset == 2);
        CHECK(l1.sector_offset == l0.sector_offset + l0.sector_count);
        CHECK(mcme_region_chunk_timestamp(&hr, 0, 0) == 0x22222222u);
        CHECK(mcme_region_chunk_timestamp(&hr, 31, 31) == 0x11111111u);
        mcme_region_close(&hr);
        check_chunk_written(hole, 0, 0, 9, 0x22222222u);
        check_chunk_written(hole, 31, 31, 7, 0x11111111u);

        void *hb;
        size_t hb_len;
        CHECK(mcme_fs_read(hole, &hb, &hb_len));
        CHECK(hb_len == 8192 + 2 * MCME_REGION_SECTOR);
        free(hb);
        remove(hole);
    }

    /* A file with nothing in it compacts to its header, whole. */
    {
        char empty[] = "build/c_empty.mca";
        uint8_t hdr[8192];
        memset(hdr, 0, sizeof hdr);
        CHECK(mcme_fs_write(empty, hdr, sizeof hdr));
        McmeRegionCompact sum;
        CHECK(mcme_region_compact(empty, &sum, &refusal));
        CHECK(sum.chunks == 0);
        CHECK(sum.before == 8192 && sum.after == 8192);
        remove(empty);
    }

    /* Every refusal the library promises. */
    CHECK(!mcme_region_compact(NULL, NULL, &refusal));
    CHECK(refusal == MCME_WRITE_REFUSAL_USAGE);
    CHECK(!mcme_region_compact("build/c_missing.mca", NULL, &refusal));
    CHECK(refusal == MCME_WRITE_REFUSAL_NOT_THERE);

    char junk[] = "build/c_junk.mca";
    CHECK(mcme_fs_write(junk, "junkjunk", 8));
    CHECK(!mcme_region_compact(junk, NULL, &refusal));
    CHECK(refusal == MCME_WRITE_REFUSAL_NOT_READABLE);

    /* A slot that points at a payload header that will not read —
       its length runs past the file — refuses the whole file: a
       compactor that skipped what it could not parse would drop it. */
    {
        char bad[] = "build/c_bad.mca";
        uint8_t buf[8192 + MCME_REGION_SECTOR];
        memset(buf, 0, sizeof buf);
        buf[2] = 2;                 /* slot 0: offset, sector 2 */
        buf[3] = 1;                 /* one sector claimed */
        buf[8192 + 0] = 0x00;       /* length 1 MiB — past the file */
        buf[8192 + 1] = 0x10;
        buf[8192 + 2] = 0x00;
        buf[8192 + 3] = 0x00;
        buf[8192 + 4] = MCME_ANVIL_COMP_ZLIB;
        CHECK(mcme_fs_write(bad, buf, sizeof buf));
        CHECK(!mcme_region_compact(bad, NULL, &refusal));
        CHECK(refusal == MCME_WRITE_REFUSAL_NOT_READABLE);
        remove(bad);
    }

    /* A payload bigger than the 255 sectors a slot's count byte can
       claim: mcme reads it (the length field says so), but a rewrite
       could not record it honestly, so the file is refused. */
    {
        char big[] = "build/c_big.mca";
        size_t sz = 8192 + 4 + 255u * MCME_REGION_SECTOR;
        uint8_t *buf = calloc(1, sz);
        CHECK(buf != NULL);
        if (buf != NULL) {
            buf[2] = 2;
            buf[3] = 255;
            buf[8192 + 0] = 0x00;   /* length field: 255 sectors of
                                        comp byte + data, so the frame
                                        is four bytes longer and rounds
                                        up to 256 */
            buf[8192 + 1] = 0x0F;
            buf[8192 + 2] = 0xF0;
            buf[8192 + 3] = 0x00;
            buf[8192 + 4] = MCME_ANVIL_COMP_ZLIB;
            CHECK(mcme_fs_write(big, buf, sz));
            free(buf);
        }
        CHECK(!mcme_region_compact(big, NULL, &refusal));
        CHECK(refusal == MCME_WRITE_REFUSAL_NOT_READABLE);
        remove(big);
    }

    mcme_arena_free(arena);
    remove(dst);
    remove(again);
}

/* ========================================================================
 * Java blocks — palettes answering for one position
 * ======================================================================== */

/* The block at a position, as the verbs print it: the spec. */
static void check_java_block(const McmeTag *root, int x, int y, int z,
                             const char *want)
{
    McmeBuffer spec;
    mcme_buffer_init(&spec);
    if (!mcme_java_block_spec(root, x, y, z, &spec)) {
        CHECK_MSG(false, "spec at (%d, %d, %d): %s", x, y, z,
                  mcme_last_error());
    } else {
        CHECK_MSG(spec.len == strlen(want)
                  && memcmp(spec.data, want, spec.len) == 0,
                  "block at (%d, %d, %d) is '%.*s', want '%s'",
                  x, y, z, (int)spec.len,
                  spec.data != NULL ? (const char *)spec.data : "",
                  want);
    }
    mcme_buffer_free(&spec);
}

/* The position's slot in the packing — stated here, not asked of
   the code under test. */
static size_t slot_of(int x, int y, int z)
{
    return (size_t)(x & 15) + (size_t)(z & 15) * 16
           + (size_t)(y & 15) * 256;
}

/* One named entry for a palette list. */
static McmeTag *palette_entry(McmeArena *arena, const char *name)
{
    McmeTag *e = mcme_nbt_new(arena, MCME_TAG_Compound);
    McmeTag *nm = mcme_nbt_new(arena, MCME_TAG_String);
    if (e == NULL || nm == NULL
            || !mcme_nbt_compound_init(arena, e, 1)
            || !mcme_nbt_set_string(arena, nm, name, strlen(name))
            || !mcme_nbt_compound_put(arena, e, "Name", 4, nm)) {
        return NULL;
    }
    return e;
}

/* A chunk with one section, Y 0, holding a two-entry palette (stone,
   dirt) with `pos` pointing at `idx` — the format's own packing:
   4 bits, sixteen entries to a long, none spanning. `nlongs` is the
   data array's length: 256 is whole, 1 is short for the palette it
   carries, 0 is no array at all. */
static McmeTag *two_block_chunk(McmeArena *arena, size_t pos, int idx,
                                size_t nlongs)
{
    McmeTag *root = mcme_nbt_new(arena, MCME_TAG_Compound);
    McmeTag *secs = mcme_nbt_new(arena, MCME_TAG_List);
    McmeTag *sec = mcme_nbt_new(arena, MCME_TAG_Compound);
    McmeTag *ytag = mcme_nbt_new(arena, MCME_TAG_Byte);
    McmeTag *bs = mcme_nbt_new(arena, MCME_TAG_Compound);
    McmeTag *pal = mcme_nbt_new(arena, MCME_TAG_List);
    McmeTag *data = mcme_nbt_new(arena, MCME_TAG_Long_Array);
    McmeTag *stone = palette_entry(arena, "minecraft:stone");
    McmeTag *dirt = palette_entry(arena, "minecraft:dirt");
    if (root == NULL || secs == NULL || sec == NULL || ytag == NULL
            || bs == NULL || pal == NULL || data == NULL
            || stone == NULL || dirt == NULL) {
        return NULL;
    }
    int64_t words[256];     /* 4096 blocks at 4 bits: 256 longs */
    memset(words, 0, sizeof words);
    if (nlongs > 0 && pos / 16 < nlongs) {
        words[pos / 16] = (int64_t)((uint64_t)idx
                                    << ((pos % 16) * 4));
    }

    if (!mcme_nbt_compound_init(arena, root, 1)
            || !mcme_nbt_list_alloc(arena, secs, MCME_TAG_Compound, 1)
            || !mcme_nbt_compound_init(arena, sec, 3)
            || !mcme_nbt_set_byte(ytag, 0)
            || !mcme_nbt_compound_put(arena, sec, "Y", 1, ytag)
            || !mcme_nbt_compound_init(arena, bs, 2)
            || !mcme_nbt_list_alloc(arena, pal, MCME_TAG_Compound, 2)
            || !mcme_nbt_set_longs(arena, data, words, nlongs)) {
        return NULL;
    }
    pal->list.items[0] = *stone;
    pal->list.items[1] = *dirt;
    if (!mcme_nbt_compound_put(arena, bs, "palette", 7, pal)) {
        return NULL;
    }
    if (nlongs > 0
            && !mcme_nbt_compound_put(arena, bs, "data", 4, data)) {
        return NULL;
    }
    if (!mcme_nbt_compound_put(arena, sec, "block_states", 12, bs)) {
        return NULL;
    }
    secs->list.items[0] = *sec;
    if (!mcme_nbt_compound_put(arena, root, "sections", 8, secs)) {
        return NULL;
    }
    return root;
}

static void test_java_chunk(void)
{
    printf("  java blocks (palettes, packing, sections)\n");

    McmeArena *arena = mcme_arena_new();
    CHECK(arena != NULL);
    if (arena == NULL) return;

    /* The golden chunk, copied: its sections are stored without
       block_states, which is the format's own air everywhere. */
    char src[512];
    char dst[] = "build/jb.mca";
    snprintf(src, sizeof src, "%s/region/r.0.0.mca", kWorldDir);
    void *golden;
    size_t golden_len;
    if (!mcme_fs_read(src, &golden, &golden_len)) {
        printf("  SKIP java blocks (cannot read %s)\n", src);
        mcme_arena_free(arena);
        return;
    }
    CHECK(mcme_fs_write(dst, golden, golden_len));

    McmeRegion reg;
    CHECK(mcme_region_open(&reg, dst));
    McmeBuffer nbt;
    mcme_buffer_init(&nbt);
    CHECK(mcme_region_read_chunk(&reg, 0, 0, NULL, &nbt));
    mcme_region_close(&reg);
    McmeNbtDoc doc;
    CHECK(mcme_nbt_parse(&doc, arena, nbt.data, nbt.len, false));
    check_java_block(doc.root, 5, 5, 5, "minecraft:air");
    check_java_block(doc.root, 0, 0, 0, "minecraft:air");
    check_java_block(doc.root, 15, 255, 15, "minecraft:air");

    /* The edited tree goes back through the region write, and the
       chunk that comes back says the same thing — the first edit
       into an untouched section makes a palette of air plus the
       block, and the rest of the section keeps its air. */
    {
        bool changed = false;
        McmeWriteRefusal refusal = MCME_WRITE_REFUSAL_NONE;
        CHECK(mcme_java_set_block(arena, doc.root, 0, 0, 0,
                                  "minecraft:stone", NULL, &changed,
                                  &refusal));
        CHECK(changed);
        CHECK(mcme_region_set_chunk(dst, 0, 0, doc.root, arena,
                                    4242u, &refusal));
        McmeRegion r2;
        CHECK(mcme_region_open(&r2, dst));
        McmeBuffer back;
        mcme_buffer_init(&back);
        CHECK(mcme_region_read_chunk(&r2, 0, 0, NULL, &back));
        mcme_region_close(&r2);
        McmeNbtDoc back_doc;
        CHECK(mcme_nbt_parse(&back_doc, arena, back.data, back.len,
                             false));
        check_java_block(back_doc.root, 0, 0, 0, "minecraft:stone");
        check_java_block(back_doc.root, 1, 0, 0, "minecraft:air");
        check_java_block(back_doc.root, 5, 5, 5, "minecraft:air");
        check_java_block(back_doc.root, 0, 16, 0, "minecraft:air");
        mcme_buffer_free(&back);
    }
    mcme_buffer_free(&nbt);

    /* A two-entry palette, one position dirty: the packing decodes
       where the reference put it. */
    McmeTag *chunk = two_block_chunk(arena, slot_of(1, 2, 3), 1, 256);
    CHECK(chunk != NULL);
    if (chunk != NULL) {
        check_java_block(chunk, 1, 2, 3, "minecraft:dirt");
        check_java_block(chunk, 0, 0, 0, "minecraft:stone");
        check_java_block(chunk, 15, 15, 15, "minecraft:stone");
        check_java_block(chunk, 0, 16, 0, "minecraft:air");

        /* A new block appends an entry and repacks; the other
           positions keep their blocks. */
        static const char kStates[] = "{pillar_axis: \"x\"}";
        McmeTag *states = NULL;
        CHECK(mcme_nbt_parse_snbt(arena, kStates, sizeof kStates - 1,
                                  &states));
        bool changed = false;
        McmeWriteRefusal refusal = MCME_WRITE_REFUSAL_INTERNAL;
        CHECK(mcme_java_set_block(arena, chunk, 0, 0, 0,
                                  "minecraft:oak_log", states,
                                  &changed, &refusal));
        CHECK(changed);
        CHECK(refusal == MCME_WRITE_REFUSAL_NONE);
        check_java_block(chunk, 0, 0, 0,
                         "minecraft:oak_log{pillar_axis: \"x\"}");
        check_java_block(chunk, 1, 2, 3, "minecraft:dirt");
        check_java_block(chunk, 0, 1, 0, "minecraft:stone");
        {
            const McmeTag *pal =
                mcme_nbt_path(chunk, "sections.0.block_states.palette");
            CHECK(pal != NULL && pal->list.len == 3);
        }

        /* A block the palette already holds reuses its entry: the
           palette does not grow. */
        CHECK(mcme_java_set_block(arena, chunk, 2, 0, 0,
                                  "minecraft:dirt", NULL,
                                  &changed, &refusal));
        CHECK(changed);
        check_java_block(chunk, 2, 0, 0, "minecraft:dirt");
        check_java_block(chunk, 1, 2, 3, "minecraft:dirt");
        {
            const McmeTag *pal =
                mcme_nbt_path(chunk, "sections.0.block_states.palette");
            CHECK(pal != NULL && pal->list.len == 3);
        }

        /* The same block again edits nothing, and the caller does
           not write. */
        CHECK(mcme_java_set_block(arena, chunk, 0, 0, 0,
                                  "minecraft:oak_log", states,
                                  &changed, &refusal));
        CHECK(!changed);

        /* Air on a position no section covers is a no-op too: no
           section is invented to hold nothing. */
        CHECK(mcme_java_set_block(arena, chunk, 0, 20, 0,
                                  "minecraft:air", NULL,
                                  &changed, &refusal));
        CHECK(!changed);
        {
            const McmeTag *secs = mcme_nbt_get(chunk, "sections");
            CHECK(secs != NULL && secs->list.len == 1);
        }

        /* A section the chunk does not list is created, kept in Y
           order: above the stored one, and before it. */
        CHECK(mcme_java_set_block(arena, chunk, 0, 20, 0,
                                  "minecraft:stone", NULL,
                                  &changed, &refusal));
        CHECK(changed);
        check_java_block(chunk, 0, 20, 0, "minecraft:stone");
        check_java_block(chunk, 5, 20, 5, "minecraft:air");
        CHECK(mcme_java_set_block(arena, chunk, 0, -16, 0,
                                  "minecraft:stone", NULL,
                                  &changed, &refusal));
        CHECK(changed);
        {
            const McmeTag *secs = mcme_nbt_get(chunk, "sections");
            CHECK(secs != NULL && secs->list.len == 3);
            const McmeTag *y0 = mcme_nbt_at(secs, 0);
            const McmeTag *y2 = mcme_nbt_at(secs, 2);
            CHECK(y0 != NULL
                  && mcme_nbt_byte_or(mcme_nbt_get(y0, "Y"), 99) == -1);
            CHECK(y2 != NULL
                  && mcme_nbt_byte_or(mcme_nbt_get(y2, "Y"), 99) == 1);
        }
        check_java_block(chunk, 0, -16, 0, "minecraft:stone");
    }

    /* Every refusal the library promises. */
    {
        McmeWriteRefusal refusal = MCME_WRITE_REFUSAL_NONE;
        bool changed = false;
        McmeTag *chunk = two_block_chunk(arena, 0, 0, 256);
        CHECK(chunk != NULL);
        CHECK(!mcme_java_set_block(NULL, chunk, 0, 0, 0,
                                   "minecraft:stone", NULL,
                                   &changed, &refusal));
        CHECK(refusal == MCME_WRITE_REFUSAL_USAGE);
        CHECK(!mcme_java_set_block(arena, chunk, 0, 0, 0, NULL,
                                   NULL, &changed, &refusal));
        CHECK(refusal == MCME_WRITE_REFUSAL_USAGE);
        static const char kList[] = "[1, 2]";
        McmeTag *not_states = NULL;
        CHECK(mcme_nbt_parse_snbt(arena, kList, sizeof kList - 1,
                                  &not_states));
        CHECK(!mcme_java_set_block(arena, chunk, 0, 0, 0,
                                   "minecraft:stone", not_states,
                                   &changed, &refusal));
        CHECK(refusal == MCME_WRITE_REFUSAL_USAGE);

        /* A y no section byte could name: the format cannot hold
           the edit. (Reading it is air — nothing could list it.) */
        CHECK(!mcme_java_set_block(arena, chunk, 0, 2048 * 16, 0,
                                   "minecraft:stone", NULL,
                                   &changed, &refusal));
        CHECK(refusal == MCME_WRITE_REFUSAL_INTERNAL);

        /* A chunk that is not a compound is not the format mcme
           reads — for the edit and for the read alike. */
        McmeTag *not_root = mcme_nbt_new(arena, MCME_TAG_List);
        CHECK(not_root != NULL);
        CHECK(!mcme_java_set_block(arena, not_root, 0, 0, 0,
                                   "minecraft:stone", NULL,
                                   &changed, &refusal));
        CHECK(refusal == MCME_WRITE_REFUSAL_NOT_READABLE);
        McmeBuffer spec;
        mcme_buffer_init(&spec);
        CHECK(!mcme_java_block_spec(not_root, 0, 0, 0, &spec));
        mcme_buffer_free(&spec);

        /* A palette of two without a data array at all. */
        McmeTag *no_data = two_block_chunk(arena, 0, 0, 0);
        CHECK(no_data != NULL);
        mcme_buffer_init(&spec);
        CHECK(!mcme_java_block_spec(no_data, 0, 0, 0, &spec));
        mcme_buffer_free(&spec);
        CHECK(!mcme_java_set_block(arena, no_data, 0, 0, 0,
                                   "minecraft:stone", NULL,
                                   &changed, &refusal));
        CHECK(refusal == MCME_WRITE_REFUSAL_NOT_READABLE);

        /* A data array too short for the palette and bits it
           carries: the block past its end has no answer, and the
           edit that would touch every position refuses too. */
        McmeTag *short_arr = two_block_chunk(arena, 0, 0, 1);
        CHECK(short_arr != NULL);
        mcme_buffer_init(&spec);
        CHECK(!mcme_java_block_spec(short_arr, 0, 0, 1, &spec));
        mcme_buffer_free(&spec);
        /* dirt at a position whose entry is stone: the edit that
           would touch every position finds the array short. */
        CHECK(!mcme_java_set_block(arena, short_arr, 0, 0, 0,
                                   "minecraft:dirt", NULL,
                                   &changed, &refusal));
        CHECK(refusal == MCME_WRITE_REFUSAL_NOT_READABLE);

        /* An index outside the palette it points into. */
        McmeTag *liar = two_block_chunk(arena, 0, 5, 256);
        CHECK(liar != NULL);
        mcme_buffer_init(&spec);
        CHECK(!mcme_java_block_spec(liar, 0, 0, 0, &spec));
        mcme_buffer_free(&spec);
    }

    free(golden);
    remove(dst);
    mcme_arena_free(arena);
}

static void test_java_fill(void)
{
    printf("  java fill (one repack per section, whole-fill refusals)\n");

    McmeArena *arena = mcme_arena_new();
    CHECK(arena != NULL);
    if (arena == NULL) return;

    bool changed;
    McmeWriteRefusal refusal;

    /* A box inside one section: only the slots it reaches move,
       corners in either order, and the target entry is reused the
       next time. */
    {
        McmeTag *root = two_block_chunk(arena, 0, 1, 256);
        CHECK(root != NULL);
        if (root != NULL) {
            McmeBox box = { 15, 0, 15, 8, 15, 8 };  /* reversed */
            changed = true; refusal = MCME_WRITE_REFUSAL_NONE;
            CHECK(mcme_java_fill_box(arena, root, 0, 0, &box,
                                     "minecraft:oak_planks", NULL,
                                     &changed, &refusal));
            CHECK(changed);
            check_java_block(root, 0, 0, 0, "minecraft:dirt");
            check_java_block(root, 7, 5, 7, "minecraft:stone");
            check_java_block(root, 15, 5, 7, "minecraft:stone");
            check_java_block(root, 8, 0, 8, "minecraft:oak_planks");
            check_java_block(root, 15, 15, 15,
                             "minecraft:oak_planks");
            const McmeTag *secs = mcme_nbt_get(root, "sections");
            const McmeTag *bs = NULL;
            if (secs != NULL && secs->list.len > 0) {
                bs = mcme_nbt_get(&secs->list.items[0],
                                  "block_states");
            }
            CHECK(bs != NULL && bs->compound.len > 0);
            const McmeTag *pal = bs != NULL
                ? mcme_nbt_get(bs, "palette") : NULL;
            CHECK(pal != NULL && pal->list.len == 3);

            /* the same box again: the block is already standing
               there, so nothing is edited and nothing to grow */
            changed = true;
            CHECK(mcme_java_fill_box(arena, root, 0, 0, &box,
                                     "minecraft:oak_planks", NULL,
                                     &changed, &refusal));
            CHECK(!changed);
            CHECK(pal != NULL && pal->list.len == 3);

            /* a second box with the same block reuses that entry:
               three stays, no fourth */
            McmeBox box2 = { 0, 0, 0, 3, 15, 3 };
            changed = false;
            CHECK(mcme_java_fill_box(arena, root, 0, 0, &box2,
                                     "minecraft:oak_planks", NULL,
                                     &changed, &refusal));
            CHECK(changed);
            check_java_block(root, 0, 0, 0, "minecraft:oak_planks");
            CHECK(pal != NULL && pal->list.len == 3);

            /* a box in another chunk's columns: this tree holds
               x 0..15, so a box at x 16..17 reaches nothing in it
               — the same slot in the next chunk is not this
               chunk's position */
            McmeBox elsewhere = { 16, 0, 8, 17, 15, 15 };
            changed = true;
            CHECK(mcme_java_fill_box(arena, root, 0, 0, &elsewhere,
                                     "minecraft:dirt", NULL, &changed,
                                     &refusal));
            CHECK(!changed);
            check_java_block(root, 0, 0, 9, "minecraft:stone");

            /* a box covering the whole section lands the format's
               own shorthand for one block: a single-entry palette
               and no data array, the stale one gone with the
               compound it was written in. The compound itself is
               replaced, so the tree is asked again. */
            McmeBox all = { -5, 0, -3, 20, 15, 18 };
            changed = false;
            CHECK(mcme_java_fill_box(arena, root, 0, 0, &all,
                                     "minecraft:stone", NULL,
                                     &changed, &refusal));
            CHECK(changed);
            secs = mcme_nbt_get(root, "sections");
            bs = (secs != NULL && secs->list.len > 0)
                ? mcme_nbt_get(&secs->list.items[0],
                               "block_states") : NULL;
            pal = bs != NULL ? mcme_nbt_get(bs, "palette") : NULL;
            CHECK(pal != NULL && pal->list.len == 1);
            CHECK(bs != NULL && mcme_nbt_get(bs, "data") == NULL);
            check_java_block(root, 0, 0, 0, "minecraft:stone");
            check_java_block(root, 15, 15, 15, "minecraft:stone");
            changed = true;
            CHECK(mcme_java_fill_box(arena, root, 0, 0, &all,
                                     "minecraft:stone", NULL,
                                     &changed, &refusal));
            CHECK(!changed);

            /* air through a section that holds blocks: still the
               shorthand, one entry, no data */
            changed = false;
            CHECK(mcme_java_fill_box(arena, root, 0, 0, &all,
                                     "minecraft:air", NULL,
                                     &changed, &refusal));
            CHECK(changed);
            secs = mcme_nbt_get(root, "sections");
            bs = (secs != NULL && secs->list.len > 0)
                ? mcme_nbt_get(&secs->list.items[0],
                               "block_states") : NULL;
            pal = bs != NULL ? mcme_nbt_get(bs, "palette") : NULL;
            CHECK(pal != NULL && pal->list.len == 1);
            CHECK(bs != NULL && mcme_nbt_get(bs, "data") == NULL);
            check_java_block(root, 8, 8, 8, "minecraft:air");
        }
    }

    /* Sections the chunk does not list: created, kept in Y order,
       and air where no section covers invents nothing. On the
       golden chunk, whose sections 0..15 are stored without
       block_states — the format's own air. */
    {
        char src[512];
        char dst[] = "build/jf.mca";
        snprintf(src, sizeof src, "%s/region/r.0.0.mca", kWorldDir);
        void *golden;
        size_t golden_len;
        if (!mcme_fs_read(src, &golden, &golden_len)) {
            printf("  SKIP java fill (cannot read %s)\n", src);
            mcme_arena_free(arena);
            return;
        }
        CHECK(mcme_fs_write(dst, golden, golden_len));
        free(golden);

        McmeRegion reg;
        CHECK(mcme_region_open(&reg, dst));
        McmeBuffer nbt;
        mcme_buffer_init(&nbt);
        CHECK(mcme_region_read_chunk(&reg, 0, 0, NULL, &nbt));
        mcme_region_close(&reg);
        McmeNbtDoc doc;
        CHECK(mcme_nbt_parse(&doc, arena, nbt.data, nbt.len, false));
        mcme_buffer_free(&nbt);
        McmeTag *root = doc.root;

        const McmeTag *secs = mcme_nbt_get(root, "sections");
        CHECK(secs != NULL && secs->list.len == 16);

        /* stone above the top: section 16 appended, in order.
           Creating a section replaces root's list with a grown
           one, so the tree is asked again after each fill. */
        McmeBox hi = { 0, 256, 0, 15, 271, 15 };
        changed = false;
        CHECK(mcme_java_fill_box(arena, root, 0, 0, &hi, "minecraft:stone",
                                 NULL, &changed, &refusal));
        CHECK(changed);
        secs = mcme_nbt_get(root, "sections");
        CHECK(secs != NULL && secs->list.len == 17);
        const McmeTag *top = (secs != NULL && secs->list.len == 17)
            ? &secs->list.items[16] : NULL;
        const McmeTag *ytag = top != NULL
            ? mcme_nbt_get(top, "Y") : NULL;
        CHECK(ytag != NULL && mcme_nbt_byte_or(ytag, -99) == 16);
        check_java_block(root, 0, 256, 0, "minecraft:stone");
        check_java_block(root, 0, 255, 0, "minecraft:air");

        /* stone below the bottom: section -1 goes to the front */
        McmeBox lo = { 0, -16, 0, 15, -1, 15 };
        changed = false;
        CHECK(mcme_java_fill_box(arena, root, 0, 0, &lo, "minecraft:stone",
                                 NULL, &changed, &refusal));
        CHECK(changed);
        secs = mcme_nbt_get(root, "sections");
        CHECK(secs != NULL && secs->list.len == 18);
        const McmeTag *first = (secs != NULL && secs->list.len == 18)
            ? &secs->list.items[0] : NULL;
        ytag = first != NULL ? mcme_nbt_get(first, "Y") : NULL;
        CHECK(ytag != NULL && mcme_nbt_byte_or(ytag, -99) == -1);
        check_java_block(root, 0, -1, 0, "minecraft:stone");

        /* air where no section covers: the column already says
           it, so no section is invented to hold it */
        McmeBox lower = { 0, -32, 0, 15, -17, 15 };
        changed = true;
        CHECK(mcme_java_fill_box(arena, root, 0, 0, &lower, "minecraft:air",
                                 NULL, &changed, &refusal));
        CHECK(!changed);
        CHECK(secs != NULL && secs->list.len == 18);
        check_java_block(root, 0, -32, 0, "minecraft:air");

        remove(dst);
    }

    /* The refusals a box can carry. */
    {
        McmeBox box = { 0, 0, 0, 1, 1, 1 };
        McmeTag *root = two_block_chunk(arena, 0, 1, 256);
        CHECK(root != NULL);
        McmeTag *not_root = mcme_nbt_new(arena, MCME_TAG_String);
        McmeTag *bad_states = mcme_nbt_new(arena, MCME_TAG_String);
        CHECK(not_root != NULL && bad_states != NULL);
        const char *s = "x";
        if (bad_states != NULL) {
            CHECK(mcme_nbt_set_string(arena, bad_states, s, 1));
        }

        changed = true; refusal = MCME_WRITE_REFUSAL_NONE;
        CHECK(!mcme_java_fill_box(NULL, root, 0, 0, &box,
                                  "minecraft:stone", NULL, &changed,
                                  &refusal));
        CHECK(refusal == MCME_WRITE_REFUSAL_USAGE);
        CHECK(!changed);          /* cleared before anything else */
        CHECK(!mcme_java_fill_box(arena, NULL, 0, 0, &box,
                                  "minecraft:stone", NULL, &changed,
                                  &refusal));
        CHECK(refusal == MCME_WRITE_REFUSAL_USAGE);
        CHECK(!mcme_java_fill_box(arena, root, 0, 0, NULL, "minecraft:stone",
                                  NULL, &changed, &refusal));
        CHECK(refusal == MCME_WRITE_REFUSAL_USAGE);
        CHECK(!mcme_java_fill_box(arena, root, 0, 0, &box, NULL, NULL,
                                  &changed, &refusal));
        CHECK(refusal == MCME_WRITE_REFUSAL_USAGE);
        CHECK(!mcme_java_fill_box(arena, root, 0, 0, &box, "minecraft:stone",
                                  bad_states, &changed, &refusal));
        CHECK(refusal == MCME_WRITE_REFUSAL_USAGE);
        CHECK(!mcme_java_fill_box(arena, not_root, 0, 0, &box,
                                  "minecraft:stone", NULL, &changed,
                                  &refusal));
        CHECK(refusal == MCME_WRITE_REFUSAL_NOT_READABLE);

        /* a box whose Y spans past the byte that keeps a section's
           Y — the CLI's range check answers this one with usage
           before the library sees it, so the library's own guard
           shows here: arithmetic past the format is internal */
        McmeBox deep = { 0, -5000, 0, 1, -4000, 1 };
        CHECK(!mcme_java_fill_box(arena, root, 0, 0, &deep,
                                  "minecraft:stone", NULL, &changed,
                                  &refusal));
        CHECK(refusal == MCME_WRITE_REFUSAL_INTERNAL);
        McmeBox sky = { 0, 3000, 0, 1, 3100, 1 };
        CHECK(!mcme_java_fill_box(arena, root, 0, 0, &sky,
                                  "minecraft:stone", NULL, &changed,
                                  &refusal));
        CHECK(refusal == MCME_WRITE_REFUSAL_INTERNAL);

        /* a data array too short for the palette it carries: the
           decode the box would repack does not hold */
        McmeTag *short_chunk = two_block_chunk(arena, 0, 1, 1);
        CHECK(short_chunk != NULL);
        CHECK(!mcme_java_fill_box(arena, short_chunk, 0, 0, &box,
                                  "minecraft:stone", NULL, &changed,
                                  &refusal));
        CHECK(refusal == MCME_WRITE_REFUSAL_NOT_READABLE);

        CHECK(!mcme_java_fill_check(NULL, 0, 0, 0, 0, &box,
                                    "minecraft:stone", NULL,
                                    &refusal));
        CHECK(refusal == MCME_WRITE_REFUSAL_USAGE);
    }

    /* The file-level fill on the golden chunk's region file: the
       check refuses the whole box before a byte is written, the
       fill writes only the chunks it changed, and a second run
       changes nothing at all. */
    {
        char src[512];
        char dst[] = "build/jf2.mca";
        snprintf(src, sizeof src, "%s/region/r.0.0.mca", kWorldDir);
        void *golden;
        size_t golden_len;
        CHECK(mcme_fs_read(src, &golden, &golden_len));
        CHECK(mcme_fs_write(dst, golden, golden_len));
        free(golden);

        /* The golden file holds chunks (0,0), (1,0), (0,1) and
           (31,31): a box reaching chunk (1,1) touches a chunk the
           region file does not have, and nothing may be written */
        void *before;
        size_t before_len;
        CHECK(mcme_fs_read(dst, &before, &before_len));
        CHECK(before_len == golden_len);

        McmeBox bad = { 0, 0, 0, 31, 15, 31 };
        refusal = MCME_WRITE_REFUSAL_NONE;
        CHECK(!mcme_java_fill_check(dst, 0, 1, 0, 1, &bad,
                                    "minecraft:stone", NULL,
                                    &refusal));
        CHECK(refusal == MCME_WRITE_REFUSAL_NOT_THERE);
        void *checked;
        size_t checked_len;
        CHECK(mcme_fs_read(dst, &checked, &checked_len));
        CHECK(checked_len == before_len
              && memcmp(checked, before, checked_len) == 0);

        /* the box the file can hold: chunks (0,0) and (1,0) */
        McmeBox box = { 0, 0, 0, 31, 15, 15 };
        changed = false;
        CHECK(mcme_java_fill(dst, 0, 1, 0, 0, &box, "minecraft:stone",
                             NULL, 1u, &changed, &refusal));
        CHECK(changed);
        {
            McmeArena *a2 = mcme_arena_new();
            CHECK(a2 != NULL);
            McmeRegion reg;
            CHECK(mcme_region_open(&reg, dst));
            McmeBuffer nbt;
            mcme_buffer_init(&nbt);
            CHECK(mcme_region_read_chunk(&reg, 1, 0, NULL, &nbt));
            mcme_region_close(&reg);
            McmeNbtDoc d2;
            CHECK(mcme_nbt_parse(&d2, a2, nbt.data, nbt.len, false));
            mcme_buffer_free(&nbt);
            check_java_block(d2.root, 20, 5, 3, "minecraft:stone");
            check_java_block(d2.root, 20, 16, 3, "minecraft:air");
            mcme_arena_free(a2);
        }

        /* the same fill again: no chunk says anything different,
           so the file keeps its bytes */
        void *after;
        size_t after_len;
        CHECK(mcme_fs_read(dst, &after, &after_len));
        changed = true;
        CHECK(mcme_java_fill(dst, 0, 1, 0, 0, &box, "minecraft:stone",
                             NULL, 2u, &changed, &refusal));
        CHECK(!changed);
        void *again;
        size_t again_len;
        CHECK(mcme_fs_read(dst, &again, &again_len));
        CHECK(again_len == after_len
              && memcmp(again, after, again_len) == 0);
        CHECK(after_len != before_len
              || memcmp(after, before, after_len) != 0);

        free(before);
        free(checked);
        free(after);
        free(again);
        remove(dst);
    }

    mcme_arena_free(arena);
}

/* Builds a chunk key the way a store holds one: x, z, [dimension], tag,
   [index]. Kept local so the test states the layout it expects rather than
   calling the code under test to build its own input. */
static size_t make_chunk_key(uint8_t *out, int32_t x, int32_t z,
                             int32_t dim, bool has_dim, uint8_t tag,
                             bool has_index, uint8_t index)
{
    size_t n = 0;
    for (int i = 0; i < 4; ++i) out[n++] = (uint8_t)((uint32_t)x >> (8 * i));
    for (int i = 0; i < 4; ++i) out[n++] = (uint8_t)((uint32_t)z >> (8 * i));
    if (has_dim) {
        for (int i = 0; i < 4; ++i) out[n++] = (uint8_t)((uint32_t)dim >> (8 * i));
    }
    out[n++] = tag;
    if (has_index) out[n++] = index;
    return n;
}

static void test_bedrock_keys(void)
{
    uint8_t key[16];
    McmeBedrockKey k;

    /* --- The overworld omission: no dimension bytes at all, and the reader
           must not mistake the spare index byte for the start of one. --- */
    {
        size_t n = make_chunk_key(key, 0, 0, 0, false,
                                  MCME_BEDROCK_TAG_SUBCHUNK_OLD, false, 0);
        CHECK(n == 9);
        CHECK(mcme_bedrock_key_parse(key, n, &k));
        CHECK(k.is_chunk);
        CHECK(!k.has_dimension);
        CHECK(k.dimension == 0);
        CHECK(k.x == 0 && k.z == 0);
        CHECK(k.tag == MCME_BEDROCK_TAG_SUBCHUNK_OLD);
    }

    /* --- 1.18+ subchunk prefix, with the subchunk index byte. --- */
    {
        size_t n = make_chunk_key(key, 4, 5, 0, false,
                                  MCME_BEDROCK_TAG_SUBCHUNK, true, 3);
        CHECK(n == 10);
        CHECK(mcme_bedrock_key_parse(key, n, &k));
        CHECK(k.is_chunk);
        CHECK(!k.has_dimension);
        CHECK(k.tag == MCME_BEDROCK_TAG_SUBCHUNK);
    }

    /* --- A dimension that is present and non-zero keeps its bytes. --- */
    {
        size_t n = make_chunk_key(key, 4, 4, 1, true,
                                  MCME_BEDROCK_TAG_CHUNK_VERSION, false, 0);
        CHECK(n == 13);
        CHECK(mcme_bedrock_key_parse(key, n, &k));
        CHECK(k.is_chunk);
        CHECK(k.has_dimension);
        CHECK(k.dimension == 1);
        CHECK(k.x == 4 && k.z == 4);
    }

    /* --- Negative coordinates survive: chunk (-1, -1) is the last slot in a
           region file, and a reader that masked rather than sign-extended
           would look for it at (4294967295, 4294967295). --- */
    {
        size_t n = make_chunk_key(key, -1, -1, 2, true,
                                  MCME_BEDROCK_TAG_SUBCHUNK, true, 1);
        CHECK(n == 14);
        CHECK(mcme_bedrock_key_parse(key, n, &k));
        CHECK(k.is_chunk);
        CHECK(k.dimension == 2);
        CHECK(k.x == -1 && k.z == -1);
    }

    /* --- A chunk-shaped key under a tag that does not identify a chunk.
           Parsed, named, and never counted. --- */
    {
        size_t n = make_chunk_key(key, 9, 9, 0, false, MCME_BEDROCK_TAG_BIOME,
                                  false, 0);
        CHECK(mcme_bedrock_key_parse(key, n, &k));
        CHECK(!k.is_chunk);
        CHECK(k.tag == MCME_BEDROCK_TAG_BIOME);
        CHECK(strcmp(mcme_bedrock_tag_name(k.tag), "biome") == 0);
    }

    /* --- Lengths that belong to no chunk key at all: a level record, an
           actor, a portallist. "Not a chunk", not an error. --- */
    mcme_clear_error();
    {
        uint8_t eight[8] = {0};
        CHECK(!mcme_bedrock_key_parse(eight, sizeof eight, &k));

        size_t n = make_chunk_key(key, 0, 0, 0, false,
                                  MCME_BEDROCK_TAG_SUBCHUNK_OLD, false, 0);
        CHECK(!mcme_bedrock_key_parse(key, n - 1, &k));   /* 8  */
        CHECK(!mcme_bedrock_key_parse(key, n + 2, &k));   /* 11 */
        CHECK(!mcme_bedrock_key_parse(key, n + 3, &k));   /* 12: 9 bytes with
                                                                a stray tail */
        /* "Not a chunk" must leave the error string alone: these keys are a
           normal part of any store, and an error here would surface as a
           failure to read a world that read fine. */
        CHECK(mcme_last_error()[0] == '\0');
        CHECK(!mcme_bedrock_key_parse(NULL, 9, &k));
    }

    /* --- Tag names, including the ones this build does not know. An
           unrecognised tag is what someone debugging a version mismatch needs
           to see, so it must come back NULL and not a made-up name. --- */
    CHECK(strcmp(mcme_bedrock_tag_name(MCME_BEDROCK_TAG_SUBCHUNK_OLD),
                 "subchunk-old") == 0);
    CHECK(strcmp(mcme_bedrock_tag_name(MCME_BEDROCK_TAG_SUBCHUNK),
                 "subchunk") == 0);
    CHECK(strcmp(mcme_bedrock_tag_name(MCME_BEDROCK_TAG_CHUNK_VERSION),
                 "version") == 0);
    CHECK(mcme_bedrock_tag_name(0x35) == NULL);

    /* --- The builder. Checked against hand-written bytes, not
           through the parser, so a builder bug cannot hide inside
           a parse that shares it. --- */
    {
        size_t n = 0;
        static const uint8_t want[] = {
            0xff, 0xff, 0xff, 0xff,   /* x = -1 */
            0x02, 0x00, 0x00, 0x00,   /* z =  2 */
            0x2f,                     /* 1.18+ subchunk prefix */
            0xfc,                     /* Y index -4 */
        };
        CHECK(mcme_bedrock_key_build(key, sizeof key, &n,
                                     -1, 2, 0,
                                     MCME_BEDROCK_TAG_SUBCHUNK,
                                     true, -4));
        CHECK(n == sizeof want);
        CHECK(memcmp(key, want, sizeof want) == 0);
        CHECK(mcme_bedrock_key_parse(key, n, &k));
        CHECK(k.is_chunk && !k.has_dimension && k.dimension == 0);
        CHECK(k.x == -1 && k.z == 2 && k.tag == MCME_BEDROCK_TAG_SUBCHUNK);
    }

    /* A nonzero dimension keeps its four bytes; a zero one is
       omitted, never written as four zeros. */
    {
        size_t n = 0;
        CHECK(mcme_bedrock_key_build(key, sizeof key, &n,
                                     0, 0, 1,
                                     MCME_BEDROCK_TAG_CHUNK_VERSION,
                                     false, 0));
        CHECK(n == 13);
        CHECK(mcme_bedrock_key_parse(key, n, &k));
        CHECK(k.has_dimension && k.dimension == 1);
        CHECK(k.tag == MCME_BEDROCK_TAG_CHUNK_VERSION);

        CHECK(mcme_bedrock_key_build(key, sizeof key, &n,
                                     0, 0, 0,
                                     MCME_BEDROCK_TAG_CHUNK_VERSION,
                                     false, 0));
        CHECK(n == 9);
        CHECK(mcme_bedrock_key_parse(key, n, &k));
        CHECK(!k.has_dimension);
    }

    /* An exact-fit buffer holds the key; a shorter one refuses.
       And an index byte on a record that has none — the version
       record, say — is refused before anything is written. */
    mcme_clear_error();
    {
        uint8_t exact[10];
        size_t n = 0;
        CHECK(mcme_bedrock_key_build(exact, sizeof exact, &n,
                                     0, 0, 0,
                                     MCME_BEDROCK_TAG_SUBCHUNK,
                                     true, 0));
        CHECK(n == sizeof exact);

        CHECK(!mcme_bedrock_key_build(key, sizeof key, &n,
                                      0, 0, 0,
                                      MCME_BEDROCK_TAG_CHUNK_VERSION,
                                      true, 0));
        CHECK(!mcme_bedrock_key_build(key, 8, &n,
                                      0, 0, 0,
                                      MCME_BEDROCK_TAG_SUBCHUNK,
                                      true, 0));
        CHECK(!mcme_bedrock_key_build(NULL, sizeof key, &n,
                                      0, 0, 0,
                                      MCME_BEDROCK_TAG_SUBCHUNK,
                                      false, 0));
        CHECK(mcme_last_error()[0] != '\0');
    }
}

/* ========================================================================
 * Bedrock subchunk payloads
 *
 * tools/ref_bedrock_subchunk.py forges payloads with amulet 0.10.64's own
 * paletted-storage encoder (the one that writes worlds Minecraft loads),
 * verifies each one back through amulet's decoder, and only then dumps it.
 * Each case ships three files: the payload (.bin), every layer's 4096
 * indices as raw little-endian uint16 in XZY order (.idx), and the
 * expectations (.txt) — version, layer shape, and every palette entry as the
 * compact SNBT the reference itself emits.
 *
 * The checks per case are the ones that would hide a bug if weakened: the
 * wrapper fields, each layer's stored width and palette length, every index
 * against the reference bytes (not a checksum — the whole point is landing
 * on THE bytes), and every palette entry re-serialised to compact SNBT and
 * required to match the reference character for character.
 *
 * The refusal matrix at the end is inline rather than golden, because a
 * payload no reference would write is a thing the test must state it expects
 * to be rejected, down to the byte.
 * ======================================================================== */

static const char kSubchunkDir[] = "tests/golden/subchunks";

/*
 * The inverse property, asserted against the reference's own payload bytes:
 * encode(decode(p)) must be p, exactly. A mismatch names the case and the
 * first differing offset — "differs" without a byte is a puzzle, not a
 * diagnosis.
 */
static void check_encode_roundtrip(const McmeBedrockSubchunk *sc,
                                   const uint8_t *want, size_t want_len,
                                   const char *what)
{
    McmeBuffer enc;
    mcme_buffer_init(&enc);
    bool ok = mcme_bedrock_subchunk_encode(sc, &enc);
    CHECK_MSG(ok, "%s: encode failed: %s", what, mcme_last_error());
    if (ok) {
        size_t at = 0;
        while (at < want_len && at < enc.len && enc.data[at] == want[at]) at++;
        CHECK_MSG(at == want_len && enc.len == want_len,
                  "%s: round trip differs at byte %zu (got %zu bytes, "
                  "want %zu)", what, at, enc.len, want_len);
    }
    mcme_buffer_free(&enc);
}

static void test_bedrock_subchunks(void)
{
    printf("  subchunk payloads (decode and byte-identical encode)\n");

    char manifest_path[512];
    snprintf(manifest_path, sizeof manifest_path, "%s/MANIFEST.txt",
             kSubchunkDir);
    FILE *fh = fopen(manifest_path, "r");
    if (fh == NULL) {
        printf("  SKIP subchunk payloads (no %s; run "
               "tools/ref_bedrock_subchunk.py)\n", manifest_path);
        return;
    }

    char names[64][64];
    size_t count = 0;
    char line[1024];
    while (count < sizeof names / sizeof names[0] &&
           fgets(line, sizeof line, fh) != NULL) {
        if (line[0] == '#' || line[0] == '\n') continue;
        if (sscanf(line, "%63s", names[count]) == 1) count++;
    }
    fclose(fh);

    if (count == 0) {
        printf("  SKIP subchunk payloads (empty %s)\n", manifest_path);
        return;
    }

    for (size_t c = 0; c < count; ++c) {
        char base[512];
        int sn = snprintf(base, sizeof base, "%s/%s", kSubchunkDir,
                          names[c]);
        CHECK(sn > 0 && (size_t)sn < sizeof base);

        char path[600];
        snprintf(path, sizeof path, "%s.bin", base);
        size_t bin_len = 0;
        uint8_t *bin = slurp(path, &bin_len);
        if (bin == NULL) {
            printf("  SKIP %s.bin\n", path);
            continue;
        }
        snprintf(path, sizeof path, "%s.idx", base);
        size_t idx_len = 0;
        uint8_t *idx = slurp(path, &idx_len);
        snprintf(path, sizeof path, "%s.txt", base);
        char *text = mcme_fs_read_text(path);

        McmeArena *arena = mcme_arena_new();
        McmeBedrockSubchunk sc;
        bool ok = mcme_bedrock_subchunk_decode(&sc, arena, bin, bin_len);
        CHECK_MSG(ok, "%s: decode failed: %s", names[c], mcme_last_error());

        if (ok && text != NULL) {
            size_t layer = 0;
            char *cursor = text;
            while (*cursor != '\0') {
                char *nl = strchr(cursor, '\n');
                if (nl != NULL) *nl = '\0';
                size_t len = strlen(cursor);
                if (len > 0 && cursor[len - 1] == '\r') cursor[len - 1] = '\0';

                unsigned v = 0, n = 0;
                int idx_byte = 0, rest = 0;
                size_t l = 0;
                unsigned bits = 0, pal = 0;
                unsigned long off = 0;
                if (sscanf(cursor, "version %u", &v) == 1) {
                    CHECK_MSG(sc.version == v,
                              "%s: version %u, expected %u",
                              names[c], sc.version, v);
                } else if (sscanf(cursor, "layers %u", &n) == 1) {
                    CHECK_MSG(sc.layer_count == n,
                              "%s: %zu layers, expected %u",
                              names[c], sc.layer_count, n);
                } else if (sscanf(cursor, "index %d", &idx_byte) == 1) {
                    CHECK_MSG(sc.has_index && sc.index == (int8_t)idx_byte,
                              "%s: Y index %d (has_index=%d), expected %d",
                              names[c], sc.index, (int)sc.has_index, idx_byte);
                } else if (sscanf(cursor, "layer %zu bits %u palette %u "
                                          "offset %lu",
                                  &l, &bits, &pal, &off) == 4) {
                    CHECK_MSG(l < sc.layer_count,
                              "%s: expectation for layer %zu but only %zu "
                              "decoded", names[c], l, sc.layer_count);
                    if (l >= sc.layer_count) break;
                    layer = l;
                    CHECK_MSG(sc.layers[l].bits == bits,
                              "%s layer %zu: %u bits, expected %u",
                              names[c], l, sc.layers[l].bits, bits);
                    CHECK_MSG(sc.layers[l].palette_len == pal,
                              "%s layer %zu: palette %zu, expected %u",
                              names[c], l, sc.layers[l].palette_len, pal);

                    /* Every index, against the reference's own bytes. */
                    if (idx != NULL && idx_len >= off + 8192) {
                        size_t u;
                        for (u = 0; u < MCME_BEDROCK_SUBCHUNK_BLOCKS; ++u) {
                            uint16_t want = (uint16_t)(idx[off + 2 * u]
                                         | (idx[off + 2 * u + 1] << 8));
                            if (sc.layers[l].indices[u] != want) break;
                        }
                        CHECK_MSG(u == MCME_BEDROCK_SUBCHUNK_BLOCKS,
                                  "%s layer %zu: index %zu is %u, "
                                  "reference says %u", names[c], l, u,
                                  u < MCME_BEDROCK_SUBCHUNK_BLOCKS
                                      ? sc.layers[l].indices[u] : 0,
                                  u < MCME_BEDROCK_SUBCHUNK_BLOCKS
                                      ? (unsigned)(idx[off + 2 * u]
                                         | (idx[off + 2 * u + 1] << 8)) : 0);
                    } else {
                        CHECK_MSG(false, "%s.idx too small for layer %zu "
                                  "at offset %lu", names[c], l, off);
                    }
                } else if (sscanf(cursor, "snbt %u %n", &n, &rest) == 1) {
                    /* `%n` records where the SNBT begins: the rest of the
                       line is the expected compact SNBT verbatim. */
                    const char *want = cursor + rest;
                    if (n < sc.layers[layer].palette_len) {
                        McmeBuffer got;
                        mcme_buffer_init(&got);
                        CHECK(mcme_nbt_write_snbt(
                                                  sc.layers[layer].palette[n].root, &got, 0));
                        CHECK_MSG(got.len == strlen(want) &&
                                  memcmp(got.data, want, got.len) == 0,
                                  "%s layer %zu palette %u SNBT differs\n"
                                  "    got  %.*s\n    want %s",
                                  names[c], layer, n,
                                  (int)got.len, (const char *)got.data, want);
                        mcme_buffer_free(&got);
                    } else {
                        CHECK_MSG(false, "%s layer %zu: snbt entry %u but "
                                  "palette has %zu", names[c], layer, n,
                                  sc.layers[layer].palette_len);
                    }
                }

                cursor = nl != NULL ? nl + 1 : cursor + len;
            }
        } else if (text == NULL) {
            printf("  SKIP %s.txt\n", path);
        }

        /* And the inverse: the encoder's output must BE the reference's
           payload, not a re-parse of it. */
        if (ok) {
            check_encode_roundtrip(&sc, bin, bin_len, names[c]);
        }

        mcme_arena_free(arena);
        free(bin);
        free(idx);
        free(text);
    }
}

static void test_subchunk_refusals(void)
{
    printf("  subchunk refusals\n");

    McmeArena *arena = mcme_arena_new();
    McmeBedrockSubchunk sc;

    /* Refusing must also SAY WHY — an error message is the only diagnosis
       someone gets from a world that will not open. */
#define REFUSE(bytes, len)                                                    \
    do {                                                                      \
        if (mcme_bedrock_subchunk_decode(&sc, arena, (bytes), (len))) {       \
            g_failed++;                                                       \
            printf("  FAIL %s:%d malformed payload decoded cleanly\n",        \
                   __FILE__, __LINE__);                                       \
        } else {                                                              \
            CHECK_MSG(mcme_last_error()[0] != '\0',                           \
                      "refusal with empty error message");                    \
        }                                                                     \
    } while (0)

    /* Empty and one byte short of a wrapper. */
    REFUSE("", 0);
    { static const uint8_t b[] = { 0x08 }; REFUSE(b, sizeof b); }
    { static const uint8_t b[] = { 0x09, 1 }; REFUSE(b, sizeof b); }

    /* Versions this build does not decode: the legacy flat format and
       anything invented after 9. */
    { static const uint8_t b[] = { 0x05, 0, 0 }; REFUSE(b, sizeof b); }
    { static const uint8_t b[] = { 0x0a, 0, 0 }; REFUSE(b, sizeof b); }

    /* Widths outside the game's enum: 7 sits in a gap the game never writes,
       127 is the biome-only "copy palette" header whose meaning for
       subchunks even mcbe-leveldb flags as unverified. */
    { static const uint8_t b[] = { 0x08, 1, 0x0e }; REFUSE(b, sizeof b); }
    { static const uint8_t b[] = { 0x08, 1, 0xfe }; REFUSE(b, sizeof b); }

    /* Runtime palettes are network data; their "palette" is runtime IDs,
       not NBT, and a world file never holds one. */
    { static const uint8_t b[] = { 0x08, 1, 0x03 }; REFUSE(b, sizeof b); }

    /* A bits-1 layer needs 512 bytes of index words (32 indices per word,
       128 words). One byte short must fail rather than read past. */
    {
        static uint8_t b[3 + 511];
        memset(b, 0, sizeof b);
        b[0] = 0x08; b[1] = 1; b[2] = 2;      /* header: bits 1, persistent */
        REFUSE(b, sizeof b);
    }

    /* Full words, then a palette length that cannot fit — refused before it
       can drive an allocation. */
    {
        static uint8_t b[3 + 512 + 4];
        memset(b, 0, sizeof b);
        b[0] = 0x08; b[1] = 1; b[2] = 2;
        b[3 + 512 + 3] = 0xff;                /* palette length 0xffffffff */
        REFUSE(b, sizeof b);
    }

    /* Length 0 with indices that need a palette. */
    {
        static uint8_t b[3 + 512 + 4];
        memset(b, 0, sizeof b);
        b[0] = 0x08; b[1] = 1; b[2] = 2;
        REFUSE(b, sizeof b);
    }

    /* Length 1, but the NBT that follows is not NBT. */
    {
        static uint8_t b[3 + 512 + 4 + 1];
        memset(b, 0, sizeof b);
        b[0] = 0x08; b[1] = 1; b[2] = 2;
        b[3 + 512 + 4] = 0xff;                /* tag type 255 does not exist */
        REFUSE(b, sizeof b);
    }

    /* A byte after the last layer: silently dropped by a tolerant decoder,
       which is exactly how a tail disappears on the way back out. */
    { static const uint8_t b[] = { 0x08, 0, 0 }; REFUSE(b, sizeof b); }

    /* --- Accepted shapes, stated as precisely as the refusals. --- */

    /* Layer count 0: the legal all-air marker, zero layers, nothing else. */
    {
        static const uint8_t b[] = { 0x08, 0 };
        CHECK(mcme_bedrock_subchunk_decode(&sc, arena, b, sizeof b));
        CHECK(sc.version == 8 && sc.layer_count == 0);
        CHECK(!sc.has_index);
        check_encode_roundtrip(&sc, b, sizeof b, "all-air marker");
    }

    /* Version 1: no count byte, one layer. Here a bits-0 layer whose only
       entry is an empty compound — the smallest legal payload that still
       carries a palette. */
    {
        static const uint8_t b[] = { 0x01, 0x00, 0x0a, 0x00, 0x00, 0x00 };
        CHECK(mcme_bedrock_subchunk_decode(&sc, arena, b, sizeof b));
        CHECK(sc.version == 1 && sc.layer_count == 1);
        CHECK(sc.layers[0].bits == 0 && sc.layers[0].palette_len == 1);
        CHECK(sc.layers[0].palette[0].root != NULL);
        bool all_zero = true;
        for (size_t u = 0; u < MCME_BEDROCK_SUBCHUNK_BLOCKS; ++u) {
            if (sc.layers[0].indices[u] != 0) { all_zero = false; break; }
        }
        CHECK(all_zero);   /* a bits-0 layer materialises 4096 zeros */
        check_encode_roundtrip(&sc, b, sizeof b, "version 1 minimal");
    }

    /* A version-9 wrapper carries its subchunk Y index, sign-extended. */
    {
        static const uint8_t b[] = { 0x09, 1, 0xfc, 0x00, 0x0a,
                                     0x00, 0x00, 0x00 };
        CHECK(mcme_bedrock_subchunk_decode(&sc, arena, b, sizeof b));
        CHECK(sc.version == 9 && sc.has_index && sc.index == -4);
        check_encode_roundtrip(&sc, b, sizeof b, "version 9 negative Y");
    }

    /* --- Encode: a model built from scratch lands on known bytes. --- */
    printf("  subchunk encode from scratch\n");
    {
        static const uint8_t want[] = { 0x08, 0x01, 0x00,
                                        0x0a, 0x00, 0x00, 0x00 };
        static const uint8_t empty_compound[] = { 0x0a, 0x00, 0x00, 0x00 };
        static uint16_t zeros[MCME_BEDROCK_SUBCHUNK_BLOCKS];
        McmeNbtDoc doc;
        CHECK(mcme_nbt_parse(&doc, arena, empty_compound,
                             sizeof empty_compound, true /* little endian */));
        memset(zeros, 0, sizeof zeros);

        McmeBedrockLayer layer = { 0 };
        layer.bits = 0;
        layer.indices = zeros;
        layer.palette_len = 1;
        layer.palette = &doc;
        McmeBedrockSubchunk built = { 0 };
        built.version = 8;
        built.layer_count = 1;
        built.layers = &layer;

        McmeBuffer enc;
        mcme_buffer_init(&enc);
        CHECK(mcme_bedrock_subchunk_encode(&built, &enc));
        CHECK_MSG(enc.len == sizeof want &&
                  memcmp(enc.data, want, sizeof want) == 0,
                  "built-from-scratch encode produced %zu bytes, want %zu",
                  enc.len, sizeof want);
        mcme_buffer_free(&enc);
    }

    /* --- Encode refuses what its wrappers cannot carry. --- */
    printf("  subchunk encode refusals\n");
    {
        static const uint8_t empty_compound[] = { 0x0a, 0x00, 0x00, 0x00 };
        static uint16_t zeros[MCME_BEDROCK_SUBCHUNK_BLOCKS];
        static uint16_t stray[MCME_BEDROCK_SUBCHUNK_BLOCKS];
        McmeNbtDoc doc;
        CHECK(mcme_nbt_parse(&doc, arena, empty_compound,
                             sizeof empty_compound, true));
        memset(zeros, 0, sizeof zeros);
        memset(stray, 0, sizeof stray);

        McmeBuffer enc;
        mcme_buffer_init(&enc);

        /* The two-phase contract: a refusal must leave the buffer empty,
           and must say why. */
#define REFUSE_ENCODE(scp)                                                     \
    do {                                                                       \
        if (mcme_bedrock_subchunk_encode((scp), &enc)) {                       \
            g_failed++;                                                        \
            printf("  FAIL %s:%d unwritable model encoded cleanly\n",          \
                   __FILE__, __LINE__);                                        \
            mcme_buffer_free(&enc);                                            \
            mcme_buffer_init(&enc);                                            \
        } else {                                                               \
            CHECK_MSG(enc.len == 0,                                            \
                      "refused encode appended %zu bytes", enc.len);           \
            CHECK_MSG(mcme_last_error()[0] != '\0',                            \
                      "refusal with empty error message");                     \
        }                                                                      \
    } while (0)

        McmeBedrockLayer layer = { 0 };
        layer.bits = 1;
        layer.indices = zeros;
        layer.palette_len = 1;
        layer.palette = &doc;
        McmeBedrockSubchunk m = { 0 };
        m.version = 8;
        m.layer_count = 1;
        m.layers = &layer;

        CHECK(!mcme_bedrock_subchunk_encode(&m, NULL));

        m.version = 5;  REFUSE_ENCODE(&m);   /* outside {1, 8, 9} */
        m.version = 0;  REFUSE_ENCODE(&m);
        m.version = 10; REFUSE_ENCODE(&m);

        m.version = 1;                        /* version 1 wants exactly 1 */
        m.layer_count = 2;
        REFUSE_ENCODE(&m);
        m.layer_count = 1;
        m.has_index = true;                   /* and has no Y index byte */
        REFUSE_ENCODE(&m);
        m.has_index = false;

        m.version = 8;                        /* version 8 has none either */
        m.has_index = true;
        REFUSE_ENCODE(&m);
        m.has_index = false;
        m.layer_count = 256;                  /* count wider than its byte */
        REFUSE_ENCODE(&m);
        m.layer_count = 1;

        m.version = 9;                        /* version 9 requires one */
        REFUSE_ENCODE(&m);
        m.has_index = true;
        m.layer_count = 256;
        REFUSE_ENCODE(&m);
        m.layer_count = 1;

        m.version = 8;                        /* layers, but no array */
        m.layer_count = 2;
        m.layers = NULL;
        REFUSE_ENCODE(&m);
        m.layer_count = 1;
        m.layers = &layer;

        layer.bits = 7;  REFUSE_ENCODE(&m);   /* width outside the enum */
        layer.bits = 127; REFUSE_ENCODE(&m);
        layer.bits = 1;

        layer.palette_len = 0; REFUSE_ENCODE(&m);   /* empty palette */
        layer.palette_len = ((size_t)1 << 33);      /* wider than u32 */
        REFUSE_ENCODE(&m);
        layer.palette_len = 1;

        layer.palette = NULL; REFUSE_ENCODE(&m);    /* palette missing */
        layer.palette = &doc;

        {                                          /* entry with no root */
            McmeNbtDoc hollow = { 0 };
            layer.palette = &hollow;
            REFUSE_ENCODE(&m);
            layer.palette = &doc;
        }

        layer.indices = NULL; REFUSE_ENCODE(&m);    /* indices missing */
        layer.indices = zeros;

        layer.bits = 0;                            /* 0-bit wants one entry */
        layer.palette_len = 2;
        REFUSE_ENCODE(&m);
        layer.palette_len = 1;
        stray[5] = 1;                              /* and nothing but 0 */
        layer.indices = stray;
        REFUSE_ENCODE(&m);
        layer.indices = zeros;

        layer.bits = 1;                            /* 3 does not fit a bit */
        stray[5] = 3;
        layer.indices = stray;
        REFUSE_ENCODE(&m);
        layer.indices = zeros;

#undef REFUSE_ENCODE
        mcme_buffer_free(&enc);
    }

#undef REFUSE
    mcme_arena_free(arena);
}

/* ========================================================================
 * The Data3D biome record: decode, and every refusal
 * ======================================================================== */

static void test_biome_record(void)
{
    printf("  biome record (Data3D: heightmap + 3D biomes)\n");
    McmeArena *arena = mcme_arena_new();
    CHECK(arena != NULL);
    if (arena == NULL) return;

    /* --- Shorter than the heightmap every record starts with. */
    {
        uint8_t b[511];
        memset(b, 0, sizeof b);
        McmeBedrockBiomes m;
        mcme_clear_error();
        CHECK(!mcme_bedrock_biome_decode(&m, arena, b, sizeof b, -4));
        CHECK(mcme_last_error()[0] != '\0');
    }

    /* --- The width-0 form: one id through the whole 4x4x4, at the
       floor, then the game's 0xFF tail walked off a byte at a time
       the way the reference walks it. The heightmap's little-endian
       int16s come across signed. */
    {
        uint8_t b[512 + 5 + 15];
        memset(b, 0, sizeof b);
        b[0] = 0x34; b[1] = 0x12;          /* column 0:     4660 */
        b[2] = 0x00; b[3] = 0x80;          /* column 1:   -32768 */
        b[512] = 0x00;                     /* header: width 0    */
        b[513] = 3;                        /* id 3, little-endian */
        memset(b + 517, 0xff, 15);
        McmeBedrockBiomes m;
        CHECK(mcme_bedrock_biome_decode(&m, arena, b, sizeof b, -4));
        CHECK(m.heightmap[0] == 4660);
        CHECK(m.heightmap[1] == -32768);
        CHECK(m.count == 1);
        CHECK(m.arrays[0].cy == -4);
        bool all3 = true;
        for (size_t i = 0; i < MCME_BEDROCK_BIOME_CELLS; ++i) {
            if (m.arrays[0].cells[i] != 3) all3 = false;
        }
        CHECK(all3);
    }

    /* --- The packed form: the subchunk storage's own word layout at
       width 2, palette (1, 6, 194), sampled to the 4x4x4 — the cells
       whose storage slot is 4, 64 or 1024 hold the marked values,
       every other cell the palette's first. */
    {
        static uint8_t b[512 + 1 + 1024 + 4 + 12];
        memset(b, 0, sizeof b);
        b[512] = 0x05;                     /* header: width 2, palette */
        b[513 + 4 * 0 + 1] = 0x02;         /* word 0,  bits 8: value 2  */
        b[513 + 4 * 4] = 0x01;             /* word 4,  bits 0: value 1  */
        b[513 + 4 * 64] = 0x01;            /* word 64, bits 0: value 1  */
        b[513 + 1024] = 3;                 /* palette length            */
        b[513 + 1024 + 4] = 1;
        b[513 + 1024 + 8] = 6;
        b[513 + 1024 + 12] = (uint8_t)194;
        McmeBedrockBiomes m;
        CHECK(mcme_bedrock_biome_decode(&m, arena, b, sizeof b, -4));
        CHECK(m.count == 1);
        CHECK(m.arrays[0].cy == -4);
        CHECK(m.arrays[0].cells[0] == 1);    /* (x0, y0, z0)             */
        CHECK(m.arrays[0].cells[1] == 6);    /* (x4, y0, z0)             */
        CHECK(m.arrays[0].cells[4] == 6);    /* (x0, y0, z4)             */
        CHECK(m.arrays[0].cells[16] == 194); /* (x0, y4, z0)             */
        CHECK(m.arrays[0].cells[63] == 1);   /* (x12, y12, z12)          */
    }

    /* --- A width past the format's widest: header bits 33. */
    {
        uint8_t b[512 + 1];
        memset(b, 0, sizeof b);
        b[512] = (uint8_t)((33u << 1) | 1u);
        McmeBedrockBiomes m;
        mcme_clear_error();
        CHECK(!mcme_bedrock_biome_decode(&m, arena, b, sizeof b, -4));
        CHECK(mcme_last_error()[0] != '\0');
    }

    /* --- Packed words that stop before the 4096 values they promise. */
    {
        uint8_t b[512 + 1 + 100];
        memset(b, 0, sizeof b);
        b[512] = 0x03;                    /* width 1 wants 512 bytes */
        McmeBedrockBiomes m;
        mcme_clear_error();
        CHECK(!mcme_bedrock_biome_decode(&m, arena, b, sizeof b, -4));
    }

    /* --- A width whose palette length reads zero. */
    {
        static uint8_t b[512 + 1 + 512 + 4];
        memset(b, 0, sizeof b);
        b[512] = 0x03;
        McmeBedrockBiomes m;
        mcme_clear_error();
        CHECK(!mcme_bedrock_biome_decode(&m, arena, b, sizeof b, -4));
    }

    /* --- A cell index past the palette that follows it: width 2,
       first value 3, palette of two. Refused, never clamped. */
    {
        static uint8_t b[512 + 1 + 1024 + 4 + 8];
        memset(b, 0, sizeof b);
        b[512] = 0x05;
        b[513] = 0x03;                    /* word 0: value 3 */
        b[513 + 1024] = 2;
        b[513 + 1024 + 4] = 1;
        b[513 + 1024 + 8] = 6;
        McmeBedrockBiomes m;
        mcme_clear_error();
        CHECK(!mcme_bedrock_biome_decode(&m, arena, b, sizeof b, -4));
    }

    /* --- A 25th array: one world height is what a record may hold,
       and the count is checked before the extra array's own bytes
       are asked for. */
    {
        static uint8_t b[512 + 24 * 5 + 1];
        memset(b, 0, sizeof b);
        b[512 + 24 * 5] = 0x00;            /* one array too many */
        McmeBedrockBiomes m;
        mcme_clear_error();
        CHECK(!mcme_bedrock_biome_decode(&m, arena, b, sizeof b, -4));
    }

    /* --- A record that is only the game's tail: it decodes and holds
       no arrays — nothing to carry, nothing to count. */
    {
        uint8_t b[512 + 15];
        memset(b, 0, 512);
        memset(b + 512, 0xff, 15);
        McmeBedrockBiomes m;
        CHECK(mcme_bedrock_biome_decode(&m, arena, b, sizeof b, -4));
        CHECK(m.count == 0);
    }

    mcme_arena_free(arena);
}

/* ========================================================================
 * Chunk inventory over the forged store
 * ======================================================================== */

typedef struct {
    size_t keys;
    size_t chunks;                       /* distinct (dim, x, z), any marker */
    size_t modern_only;                  /* chunks only tag 0x2f knows       */
    size_t by_dim[4];                    /* overworld / nether / end         */
    size_t n_coords;
    struct { int32_t dim, x, z; } coords[64];
} Inventory;

/* Mirrors the counting rule in src/app/main.c: a chunk is present if any
   *marker* key marks it, so several keys on one chunk count once. Tag 0x2d
   belongs to a chunk without identifying one and must never be counted. */
static bool inventory_key(void *ud, const McmeLevelEntry *e)
{
    Inventory *inv = (Inventory *)ud;
    inv->keys++;

    McmeBedrockKey k;
    if (!mcme_bedrock_key_parse(e->key, e->key_len, &k) || !k.is_chunk) {
        return true;
    }

    bool seen = false;
    for (size_t i = 0; i < inv->n_coords; ++i) {
        if (inv->coords[i].dim == k.dimension &&
            inv->coords[i].x == k.x && inv->coords[i].z == k.z) {
            seen = true;
            break;
        }
    }
    if (seen) return true;

    if (inv->n_coords < sizeof inv->coords / sizeof inv->coords[0]) {
        inv->coords[inv->n_coords].dim = k.dimension;
        inv->coords[inv->n_coords].x = k.x;
        inv->coords[inv->n_coords].z = k.z;
        inv->n_coords++;
    }
    inv->chunks++;
    if (k.tag == MCME_BEDROCK_TAG_SUBCHUNK) inv->modern_only++;
    if (k.dimension >= 0 && k.dimension < 4) inv->by_dim[k.dimension]++;
    return true;
}

/*
 * The forged fixture is the only store whose chunk set is designed, so it is
 * the one this test can assert exact numbers against. tools/ref_leveldb_table.py
 * writes nine chunks over three dimensions; two of them exist only under tag
 * 0x2f, which Amulet 0.10.64 does not know and reports as absent (its count
 * is pinned in the golden dump's header). Counting keys rather than distinct
 * coordinates would report 24, and counting the 0x2d biome records as chunks
 * would double several of them — both are the bugs this test exists to catch.
 */
static void test_bedrock_inventory(void)
{
    if (!mcme_fs_exists(kForgeWorld)) {
        printf("  SKIP forged store (%s not found; run "
               "tools/ref_leveldb_table.py)\n", kForgeWorld);
        return;
    }

    /* The probe must hand the reader the db/ directory itself, not the world
       folder above it — the mistake callers make most often. */
    McmeWorldInfo info;
    char want_db[640];
    snprintf(want_db, sizeof want_db, "%s/db", kForgeWorld);
    CHECK(mcme_world_probe(&info, kForgeWorld));
    CHECK(info.kind == MCME_WORLD_BEDROCK);
    CHECK(info.has_db);
    CHECK_MSG(info.db_path != NULL && strcmp(info.db_path, want_db) == 0,
              "db_path is '%s', want '%s'",
              info.db_path ? info.db_path : "(null)", want_db);
    if (info.db_path == NULL) {
        mcme_world_info_free(&info);
        return;
    }
    char db_path[512];
    snprintf(db_path, sizeof db_path, "%s", info.db_path);
    mcme_world_info_free(&info);

    McmeLevelDb *db = NULL;
    if (!mcme_leveldb_open(&db, db_path)) {
        CHECK_MSG(false, "cannot open %s: %s", db_path, mcme_last_error());
        return;
    }

    Inventory inv;
    memset(&inv, 0, sizeof inv);
    mcme_leveldb_iterate(db, inventory_key, &inv);
    mcme_leveldb_close(db);

    CHECK_MSG(inv.keys == 23, "store holds %zu live keys, want 23", inv.keys);
    CHECK_MSG(inv.chunks == 9, "%zu chunks, want 9", inv.chunks);
    CHECK_MSG(inv.by_dim[0] == 5, "%zu overworld chunks, want 5", inv.by_dim[0]);
    CHECK_MSG(inv.by_dim[1] == 2, "%zu nether chunks, want 2", inv.by_dim[1]);
    CHECK_MSG(inv.by_dim[2] == 2, "%zu end chunks, want 2", inv.by_dim[2]);

    /* The two modern-only chunks are exactly what Amulet 0.10.64 cannot see:
       seven rather than nine. Both counts are pinned in the golden dump. */
    CHECK_MSG(inv.modern_only == 2,
              "%zu chunks marked only by 0x2f, want 2", inv.modern_only);
}

/* ========================================================================
 * Writing: one block, through the store
 * ======================================================================== */

static const char kChunksWorld[] = "tests/golden/worlds/bedrock-chunks";
static const char kEntitiesWorld[] = "tests/golden/worlds/bedrock-entities";

/* Copies a whole db/ directory to `dst` so an edit
   lands on a copy and never on the fixture. Only
   regular files; the store layout is flat. */
static bool copy_store(const char *src, const char *dst)
{
    if (!mcme_fs_mkdir_p(dst)) return false;

    DIR *d = opendir(src);
    if (d == NULL) return false;

    struct dirent *e;
    bool ok = true;
    while (ok && (e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.') continue;

        char from[4096], to[4096];
        snprintf(from, sizeof from, "%s/%s", src, e->d_name);
        snprintf(to, sizeof to, "%s/%s", dst, e->d_name);

        void *data = NULL;
        size_t len = 0;
        if (!mcme_fs_read(from, &data, &len)) { ok = false; break; }
        ok = mcme_fs_write(to, data, len);
        free(data);
    }
    closedir(d);
    return ok;
}

static void remove_store(const char *dir)
{
    DIR *d = opendir(dir);
    if (d == NULL) return;

    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.') continue;
        char path[4096];
        snprintf(path, sizeof path, "%s/%s", dir, e->d_name);
        remove(path);
    }
    closedir(d);
    rmdir(dir);
}

/* The same block is the same block exactly when the
   reference's bytes for it are: serialise both the
   way the store carries them and compare. */
static bool docs_equal(const McmeNbtDoc *a, const McmeNbtDoc *b)
{
    McmeBuffer x, y;
    mcme_buffer_init(&x);
    mcme_buffer_init(&y);
    bool ok = mcme_nbt_write_binary_as(a, &x, true)
              && mcme_nbt_write_binary_as(b, &y, true)
              && x.len == y.len
              && memcmp(x.data, y.data, x.len) == 0;
    mcme_buffer_free(&x);
    mcme_buffer_free(&y);
    return ok;
}

/* Decodes the subchunk payload a fixture ships, for
   the expectations an edit is checked against. */
static bool decode_fixture(const char *case_name,
                           McmeArena *arena,
                           McmeBedrockSubchunk *out)
{
    char path[512];
    snprintf(path, sizeof path,
             "tests/golden/subchunks/%s.bin", case_name);
    void *data = NULL;
    size_t size = 0;
    if (!mcme_fs_read(path, &data, &size)) return false;
    bool ok = mcme_bedrock_subchunk_decode(out, arena,
                                           data, size);
    free(data);
    return ok;
}

/* Reads the subchunk at (cx, cz, dimension, cy) from
   an open store and decodes it. */
static bool load_subchunk(McmeLevelDb *db, McmeArena *arena,
                          int32_t cx, int32_t cz,
                          int32_t dimension, int8_t cy,
                          McmeBedrockSubchunk *out)
{
    uint8_t key[14];
    size_t key_len = 0;
    if (!mcme_bedrock_key_build(key, sizeof key, &key_len,
                                cx, cz, dimension,
                                MCME_BEDROCK_TAG_SUBCHUNK,
                                true, cy)) {
        return false;
    }
    bool deleted = false;
    McmeBuffer payload;
    mcme_buffer_init(&payload);
    if (!mcme_leveldb_get(db, key, key_len, &payload, &deleted)
            || deleted) {
        mcme_buffer_free(&payload);
        return false;
    }
    bool ok = mcme_bedrock_subchunk_decode(
                                           out, arena, payload.data, payload.len);
    mcme_buffer_free(&payload);
    return ok;
}

/* Reads the raw value at a subchunk key, for byte
   comparisons against a fixture payload. */
static bool raw_subchunk(McmeLevelDb *db, int32_t cx, int32_t cz,
                         int32_t dimension, int8_t cy,
                         McmeBuffer *out)
{
    uint8_t key[14];
    size_t key_len = 0;
    if (!mcme_bedrock_key_build(key, sizeof key, &key_len,
                                cx, cz, dimension,
                                MCME_BEDROCK_TAG_SUBCHUNK,
                                true, cy)) {
        return false;
    }
    bool deleted = false;
    return mcme_leveldb_get(db, key, key_len, out, &deleted)
               && !deleted;
}

/* mkdtemp writes the name it made over the pattern,
   so every case starts from the pattern again. */
static bool fresh_store_dir(char *tmp, size_t cap)
{
    snprintf(tmp, cap, "/tmp/opencode/mcme_write_XXXXXX");
    return mkdtemp(tmp) != NULL;
}

/* The tally row whose spec is exactly these `block_len` bytes,
   or -1. Specs are arena-rooted and not terminated. */
static int tally_find(const McmeBlockCount *tally, size_t len,
                      const char *block, size_t block_len)
{
    for (size_t i = 0; i < len; ++i) {
        if (strncmp(tally[i].block, block, block_len) == 0
                && tally[i].block[block_len] == '\0') {
            return (int)i;
        }
    }
    return -1;
}

/* Sorted the way count_box promises: most frequent first, ties
   by spec. */
static int block_count_cmp_ok(const McmeBlockCount *tally, size_t len)
{
    for (size_t i = 1; i < len; ++i) {
        if (tally[i - 1].count > tally[i].count) continue;
        if (tally[i - 1].count == tally[i].count
                && strcmp(tally[i - 1].block, tally[i].block) < 0) {
            continue;
        }
        return 0;
    }
    return 1;
}

static void test_bedrock_write(void)
{
    char src[512];
    snprintf(src, sizeof src, "%s/db", kChunksWorld);
    if (!mcme_fs_exists(src)) {
        printf("  SKIP chunk write tests (%s not found; run "
               "tools/ref_bedrock_store.py)\n", src);
        return;
    }

    McmeArena *arena = mcme_arena_new();
    if (arena == NULL) {
        CHECK_MSG(false, "out of memory");
        return;
    }

    /* Every edit runs on a fresh copy of the fixture,
       so the cases stay independent of one another. */
    char tmp[64];
    char dbdir[512];
    McmeBedrockSubchunk a, b;
    McmeNbtDoc placed;
    McmeLevelDb *db = NULL;

    /* --- A new block joins a wide palette. --- */
    {
        CHECK(fresh_store_dir(tmp, sizeof tmp));
        snprintf(dbdir, sizeof dbdir, "%s/db", tmp);
        CHECK(copy_store(src, dbdir));
        CHECK(decode_fixture("v8-bits4", arena, &a));
        CHECK(mcme_bedrock_block_entry(arena, "mcme:placed",
                                       NULL, &placed));

        CHECK(mcme_bedrock_set_block(dbdir, 0, 0, 0, 0, &placed, NULL));
        CHECK(mcme_leveldb_open(&db, dbdir));
        CHECK(load_subchunk(db, arena, 0, 0, 0, 0, &b));

        CHECK(b.version == 8 && b.layer_count == 1);
        CHECK(b.layers[0].palette_len
              == a.layers[0].palette_len + 1);
        for (size_t j = 0; j < a.layers[0].palette_len; ++j) {
            CHECK_MSG(docs_equal(&b.layers[0].palette[j],
                                 &a.layers[0].palette[j]),
                      "palette entry %zu changed", j);
        }
        CHECK(docs_equal(
                         &b.layers[0].palette[a.layers[0].palette_len],
                         &placed));
        CHECK(b.layers[0].indices[0] == a.layers[0].palette_len);
        for (size_t v = 1; v < MCME_BEDROCK_SUBCHUNK_BLOCKS; ++v) {
            CHECK_MSG(b.layers[0].indices[v]
                          == a.layers[0].indices[v],
                      "index %zu changed", v);
        }
        /* Seventeen entries outgrow four bits. */
        CHECK(b.layers[0].bits == 5);

        /* The neighbouring chunk is untouched, byte
           for byte. */
        McmeBuffer raw;
        mcme_buffer_init(&raw);
        CHECK(raw_subchunk(db, 1, -2, 0, 0, &raw));
        void *want = NULL;
        size_t want_len = 0;
        CHECK(mcme_fs_read("tests/golden/subchunks/v8-bits16.bin",
                           &want, &want_len));
        CHECK_MSG(raw.len == want_len
                      && memcmp(raw.data, want, want_len) == 0,
                  "neighbouring chunk changed");
        free(want);
        mcme_buffer_free(&raw);
        mcme_leveldb_close(db);
        remove_store(dbdir);
        rmdir(tmp);
    }

    /* --- Setting a block to a block the palette
           already holds reuses its index and nothing
           else. --- */
    {
        CHECK(fresh_store_dir(tmp, sizeof tmp));
        snprintf(dbdir, sizeof dbdir, "%s/db", tmp);
        CHECK(copy_store(src, dbdir));
        CHECK(decode_fixture("v8-bits4", arena, &a));

        /* Pick the palette entry after the one already
           there, so the edit is provably a change. */
        size_t u = 1 * 256;             /* block (1, 0, 0) */
        size_t want = (a.layers[0].indices[u] + 1)
                      % a.layers[0].palette_len;
        CHECK(mcme_bedrock_set_block(dbdir, 1, 0, 0, 0,
                                     &a.layers[0].palette[want], NULL));
        CHECK(mcme_leveldb_open(&db, dbdir));
        CHECK(load_subchunk(db, arena, 0, 0, 0, 0, &b));

        CHECK(b.layers[0].palette_len == a.layers[0].palette_len);
        for (size_t j = 0; j < a.layers[0].palette_len; ++j) {
            CHECK(docs_equal(&b.layers[0].palette[j],
                             &a.layers[0].palette[j]));
        }
        CHECK(b.layers[0].indices[u] == want);
        for (size_t v = 0; v < MCME_BEDROCK_SUBCHUNK_BLOCKS; ++v) {
            if (v == u) continue;
            CHECK(b.layers[0].indices[v] == a.layers[0].indices[v]);
        }
        CHECK(b.layers[0].bits == 4);
        mcme_leveldb_close(db);
        remove_store(dbdir);
        rmdir(tmp);
    }

    /* --- Setting a block to the value it already
           holds changes nothing: the payload comes
           back byte for byte. --- */
    {
        CHECK(fresh_store_dir(tmp, sizeof tmp));
        snprintf(dbdir, sizeof dbdir, "%s/db", tmp);
        CHECK(copy_store(src, dbdir));
        CHECK(decode_fixture("v8-bits4", arena, &a));

        size_t u = 2 * 256 + 4 * 16 + 3;   /* block (2, 3, 4) */
        CHECK(mcme_bedrock_set_block(
                                     dbdir, 2, 3, 4, 0,
                                     &a.layers[0].palette[a.layers[0].indices[u]], NULL));
        CHECK(mcme_leveldb_open(&db, dbdir));

        McmeBuffer raw;
        mcme_buffer_init(&raw);
        CHECK(raw_subchunk(db, 0, 0, 0, 0, &raw));
        void *want = NULL;
        size_t want_len = 0;
        CHECK(mcme_fs_read("tests/golden/subchunks/v8-bits4.bin",
                           &want, &want_len));
        CHECK_MSG(raw.len == want_len
                      && memcmp(raw.data, want, want_len) == 0,
                  "a no-op edit rewrote the payload");
        free(want);
        mcme_buffer_free(&raw);
        mcme_leveldb_close(db);
        remove_store(dbdir);
        rmdir(tmp);
    }

    /* --- A waterlogged subchunk: the block layer
           gains the entry and the fluid layer is
           untouched, width included (four entries
           still fit the stored two bits). --- */
    {
        CHECK(fresh_store_dir(tmp, sizeof tmp));
        snprintf(dbdir, sizeof dbdir, "%s/db", tmp);
        CHECK(copy_store(src, dbdir));
        CHECK(decode_fixture("v8-waterlogged", arena, &a));
        CHECK(mcme_bedrock_block_entry(arena, "mcme:placed",
                                       NULL, &placed));

        size_t u = 5 * 256 + 7 * 16 + 6;   /* block (5, 22, 7) */
        CHECK(mcme_bedrock_set_block(dbdir, 5, 22, 7, 0, &placed, NULL));
        CHECK(mcme_leveldb_open(&db, dbdir));
        CHECK(load_subchunk(db, arena, 0, 0, 0, 1, &b));

        CHECK(b.version == 8 && b.layer_count == 2);
        CHECK(b.layers[0].palette_len
              == a.layers[0].palette_len + 1);
        for (size_t j = 0; j < a.layers[0].palette_len; ++j) {
            CHECK(docs_equal(&b.layers[0].palette[j],
                             &a.layers[0].palette[j]));
        }
        CHECK(docs_equal(
                         &b.layers[0].palette[a.layers[0].palette_len],
                         &placed));
        CHECK(b.layers[0].indices[u] == a.layers[0].palette_len);
        CHECK(b.layers[0].bits == 2);

        McmeBedrockLayer *fluid = &b.layers[1];
        McmeBedrockLayer *fluid_a = &a.layers[1];
        CHECK(fluid->bits == fluid_a->bits);
        CHECK(fluid->palette_len == fluid_a->palette_len);
        for (size_t j = 0; j < fluid_a->palette_len; ++j) {
            CHECK(docs_equal(&fluid->palette[j],
                             &fluid_a->palette[j]));
        }
        for (size_t v = 0; v < MCME_BEDROCK_SUBCHUNK_BLOCKS; ++v) {
            CHECK(fluid->indices[v] == fluid_a->indices[v]);
        }
        mcme_leveldb_close(db);
        remove_store(dbdir);
        rmdir(tmp);
    }

    /* --- The compact bits==0 form: a second entry
           leaves it for the width the format
           carries. --- */
    {
        CHECK(fresh_store_dir(tmp, sizeof tmp));
        snprintf(dbdir, sizeof dbdir, "%s/db", tmp);
        CHECK(copy_store(src, dbdir));
        CHECK(decode_fixture("v8-bpb0", arena, &a));
        CHECK(mcme_bedrock_block_entry(arena, "mcme:placed",
                                       NULL, &placed));

        size_t u = 15;                     /* block (0, -1, 0) */
        CHECK(mcme_bedrock_set_block(dbdir, 0, -1, 0, 0, &placed, NULL));
        CHECK(mcme_leveldb_open(&db, dbdir));
        CHECK(load_subchunk(db, arena, 0, 0, 0, -1, &b));

        CHECK(b.version == 8 && b.layer_count == 1);
        CHECK(b.layers[0].palette_len == 2);
        CHECK(docs_equal(&b.layers[0].palette[0],
                         &a.layers[0].palette[0]));
        CHECK(docs_equal(&b.layers[0].palette[1], &placed));
        CHECK(b.layers[0].indices[u] == 1);
        for (size_t v = 0; v < MCME_BEDROCK_SUBCHUNK_BLOCKS; ++v) {
            if (v == u) continue;
            CHECK(b.layers[0].indices[v] == 0);
        }
        CHECK(b.layers[0].bits == 1);
        mcme_leveldb_close(db);
        remove_store(dbdir);
        rmdir(tmp);
    }

    /* --- The version 9 wrapper keeps its version
           and its Y index through the edit. --- */
    {
        CHECK(fresh_store_dir(tmp, sizeof tmp));
        snprintf(dbdir, sizeof dbdir, "%s/db", tmp);
        CHECK(copy_store(src, dbdir));
        CHECK(decode_fixture("v9-cy4", arena, &a));
        CHECK(mcme_bedrock_block_entry(arena, "mcme:placed",
                                       NULL, &placed));

        size_t u = 4;                      /* block (0, 68, 0) */
        CHECK(mcme_bedrock_set_block(dbdir, 0, 68, 0, 0, &placed, NULL));
        CHECK(mcme_leveldb_open(&db, dbdir));
        CHECK(load_subchunk(db, arena, 0, 0, 0, 4, &b));

        CHECK(b.version == 9 && b.has_index && b.index == 4);
        CHECK(b.layers[0].palette_len
              == a.layers[0].palette_len + 1);
        for (size_t j = 0; j < a.layers[0].palette_len; ++j) {
            CHECK(docs_equal(&b.layers[0].palette[j],
                             &a.layers[0].palette[j]));
        }
        CHECK(docs_equal(
                         &b.layers[0].palette[a.layers[0].palette_len],
                         &placed));
        CHECK(b.layers[0].indices[u] == a.layers[0].palette_len);
        for (size_t v = 0; v < MCME_BEDROCK_SUBCHUNK_BLOCKS; ++v) {
            if (v == u) continue;
            CHECK(b.layers[0].indices[v] == a.layers[0].indices[v]);
        }
        /* Five entries outgrow two bits. */
        CHECK(b.layers[0].bits == 3);
        mcme_leveldb_close(db);
        remove_store(dbdir);
        rmdir(tmp);
    }

    /* --- A non-zero dimension is written into the
           key, and the edit lands on it. --- */
    {
        CHECK(fresh_store_dir(tmp, sizeof tmp));
        snprintf(dbdir, sizeof dbdir, "%s/db", tmp);
        CHECK(copy_store(src, dbdir));
        CHECK(decode_fixture("v8-bits1", arena, &a));
        CHECK(mcme_bedrock_block_entry(arena, "mcme:placed",
                                       NULL, &placed));

        CHECK(mcme_bedrock_set_block(dbdir, 0, 0, 0, 1, &placed, NULL));
        CHECK(mcme_leveldb_open(&db, dbdir));
        CHECK(load_subchunk(db, arena, 0, 0, 1, 0, &b));

        CHECK(b.version == 8 && b.layer_count == 1);
        CHECK(b.layers[0].palette_len
              == a.layers[0].palette_len + 1);
        CHECK(docs_equal(
                         &b.layers[0].palette[a.layers[0].palette_len],
                         &placed));
        CHECK(b.layers[0].indices[0] == a.layers[0].palette_len);
        /* Three entries outgrow one bit. */
        CHECK(b.layers[0].bits == 2);
        mcme_leveldb_close(db);
        remove_store(dbdir);
        rmdir(tmp);
    }

    /* --- A subchunk the world has not stored is created:
           the edit reaches into ground that was never
           saved, and the direct API still says what it
           was handed. --- */
    {
        CHECK(fresh_store_dir(tmp, sizeof tmp));
        snprintf(dbdir, sizeof dbdir, "%s/db", tmp);
        CHECK(copy_store(src, dbdir));
        CHECK(decode_fixture("v8-bits4", arena, &a));
        CHECK(mcme_bedrock_block_entry(arena, "mcme:placed",
                                       NULL, &placed));
        McmeNbtDoc air;
        CHECK(mcme_bedrock_block_entry(arena, "minecraft:air",
                                       NULL, &air));

        /* Block (0, 48, 0) is subchunk (0, 0) cy 3, never
           stored. Setting it creates the subchunk — in the
           version its neighbour cy 4 uses, so version 9 with
           the Y index — and the rest of it is air. */
        CHECK(mcme_bedrock_set_block(dbdir, 0, 3 * 16, 0, 0,
                                     &placed, NULL));
        {
            CHECK(mcme_leveldb_open(&db, dbdir));
            McmeBedrockSubchunk c;
            CHECK(load_subchunk(db, arena, 0, 0, 0, 3, &c));
            CHECK(c.version == 9);
            CHECK(c.has_index && c.index == 3);
            CHECK(c.layer_count == 1);
            CHECK(c.layers[0].palette_len == 2);
            CHECK(docs_equal(&c.layers[0].palette[0], &air));
            CHECK(docs_equal(&c.layers[0].palette[1], &placed));
            CHECK(c.layers[0].indices[0] == 1);   /* x,y,z = 0 */
            CHECK(c.layers[0].indices[4095] == 0); /* all air  */
            mcme_leveldb_close(db);
            db = NULL;
        }

        /* Block (32, 0, 0) is subchunk (2, 0) cy 0, with no
           neighbour at all: version 8, the default every
           version of the game reads. */
        CHECK(mcme_bedrock_set_block(dbdir, 32, 0, 0, 0,
                                     &placed, NULL));
        {
            CHECK(mcme_leveldb_open(&db, dbdir));
            McmeBedrockSubchunk c;
            CHECK(load_subchunk(db, arena, 2, 0, 0, 0, &c));
            CHECK(c.version == 8);
            CHECK(!c.has_index);
            CHECK(c.layer_count == 1);
            CHECK(c.layers[0].palette_len == 2);
            CHECK(c.layers[0].indices[0] == 1);
            mcme_leveldb_close(db);
            db = NULL;
        }

        /* A payload the store will not decode is a format
           problem: the write refuses it and says which class
           of failure it was. */
        uint8_t junk[8] = {8, 250, 'x', 'x', 'x', 'x', 'x', 'x'};
        uint8_t skey[14];
        size_t skey_len = 0;
        CHECK(mcme_bedrock_key_build(skey, sizeof skey, &skey_len,
                                     0, 0, 0,
                                     MCME_BEDROCK_TAG_SUBCHUNK,
                                     true, 5));
        CHECK(mcme_leveldb_open_rw(&db, dbdir));
        CHECK(mcme_leveldb_put(db, skey, skey_len, junk,
                               sizeof junk));
        mcme_leveldb_close(db);
        db = NULL;

        mcme_clear_error();
        McmeWriteRefusal refusal = MCME_WRITE_REFUSAL_NOT_THERE;
        CHECK(!mcme_bedrock_set_block(dbdir, 0, 5 * 16, 0, 0,
                                      &placed, &refusal));
        CHECK(refusal == MCME_WRITE_REFUSAL_NOT_READABLE);
        CHECK(mcme_last_error()[0] != '\0');

        /* Subchunk Y past the key's signed byte, and the two
           null arguments, are usage problems. */
        refusal = MCME_WRITE_REFUSAL_NOT_THERE;
        CHECK(!mcme_bedrock_set_block(dbdir, 0, 2048, 0, 0,
                                      &placed, &refusal));
        CHECK(refusal == MCME_WRITE_REFUSAL_USAGE);
        CHECK(mcme_last_error()[0] != '\0');
        refusal = MCME_WRITE_REFUSAL_NOT_THERE;
        CHECK(!mcme_bedrock_set_block(NULL, 0, 0, 0, 0, &placed,
                                      &refusal));
        CHECK(refusal == MCME_WRITE_REFUSAL_USAGE);
        refusal = MCME_WRITE_REFUSAL_NOT_THERE;
        CHECK(!mcme_bedrock_set_block(dbdir, 0, 0, 0, 0, NULL,
                                      &refusal));
        CHECK(refusal == MCME_WRITE_REFUSAL_USAGE);
        CHECK(mcme_last_error()[0] != '\0');

        /* Coordinates outside the subchunk, an
           all-air subchunk, a block that is not a
           compound, and a palette entry that carries
           a root name. */
        CHECK(!mcme_bedrock_subchunk_set(&a, arena, 16, 0, 0,
                                         &placed));
        CHECK(!mcme_bedrock_subchunk_set(&a, arena, 0, -1, 0,
                                         &placed));
        CHECK(mcme_last_error()[0] != '\0');

        McmeBedrockSubchunk empty;
        memset(&empty, 0, sizeof empty);
        CHECK(!mcme_bedrock_subchunk_set(&empty, arena, 0, 0, 0,
                                         &placed));

        McmeTag *str = mcme_arena_alloc(arena, sizeof *str);
        str->type = MCME_TAG_String;
        str->string.data = (char *)"x";
        str->string.len = 1;
        McmeNbtDoc not_compound = {
            .arena = arena, .root = str,
            .root_name = {NULL, 0}, .little_endian = true,
        };
        CHECK(!mcme_bedrock_subchunk_set(&a, arena, 0, 0, 0,
                                         &not_compound));

        McmeNbtDoc named = placed;
        named.root_name.data = (char *)"root";
        named.root_name.len = 4;
        CHECK(!mcme_bedrock_subchunk_set(&a, arena, 0, 0, 0,
                                         &named));

        /* Building an entry refuses an empty name and
           states that are not a compound. */
        CHECK(!mcme_bedrock_block_entry(NULL, "minecraft:stone",
                                        NULL, &placed));
        CHECK(!mcme_bedrock_block_entry(arena, NULL, NULL, &placed));
        CHECK(!mcme_bedrock_block_entry(arena, "minecraft:stone",
                                        NULL, NULL));
        CHECK(!mcme_bedrock_block_entry(arena, "", NULL, &placed));
        CHECK(!mcme_bedrock_block_entry(arena, "minecraft:stone",
                                        str, &placed));
        CHECK(mcme_last_error()[0] != '\0');

        remove_store(dbdir);
        rmdir(tmp);
    }

    /* --- A fill spans subchunks, takes the corners in any
       order, and leaves the rest of every subchunk
       alone. --- */
    {
        CHECK(fresh_store_dir(tmp, sizeof tmp));
        snprintf(dbdir, sizeof dbdir, "%s/db", tmp);
        CHECK(copy_store(src, dbdir));
        CHECK(decode_fixture("v8-bits4", arena, &a));
        CHECK(mcme_bedrock_block_entry(arena, "mcme:placed",
                                       NULL, &placed));

        McmeBox box = {2, 31, 1, 0, 0, 0};   /* reversed */
        CHECK(mcme_leveldb_open_rw(&db, dbdir));
        CHECK(mcme_bedrock_fill_check(db, &box, 0, NULL));
        CHECK(mcme_bedrock_fill(db, &box, 0, &placed, NULL));
        mcme_leveldb_close(db);
        db = NULL;

        CHECK(mcme_leveldb_open(&db, dbdir));
        CHECK(load_subchunk(db, arena, 0, 0, 0, 0, &b));
        CHECK(b.version == a.version && b.layer_count == 1);
        /* The new entry joined the palette; the old
           entries stayed. */
        size_t placed_idx = (size_t)-1;
        for (size_t j = 0; j < b.layers[0].palette_len; ++j) {
            if (docs_equal(&b.layers[0].palette[j], &placed)) {
                placed_idx = j;
            }
        }
        CHECK(placed_idx == a.layers[0].palette_len);
        CHECK(b.layers[0].palette_len
              == a.layers[0].palette_len + 1);
        for (size_t j = 0; j < a.layers[0].palette_len; ++j) {
            CHECK(docs_equal(&b.layers[0].palette[j],
                             &a.layers[0].palette[j]));
        }
        /* Every position the box covers holds the new
           block; every position outside it did not
           move. */
        for (int32_t lz = 0; lz < 16; ++lz) {
            for (int32_t ly = 0; ly < 16; ++ly) {
                for (int32_t lx = 0; lx < 16; ++lx) {
                    size_t u = (size_t)lx * 256
                             + (size_t)lz * 16 + (size_t)ly;
                    if (lx <= 2 && lz <= 1) {
                        CHECK(b.layers[0].indices[u]
                              == placed_idx);
                    } else {
                        CHECK(b.layers[0].indices[u]
                              == a.layers[0].indices[u]);
                    }
                }
            }
        }
        mcme_leveldb_close(db);
        db = NULL;

        /* The subchunk the box reaches into keeps its
           fluid layer. */
        CHECK(decode_fixture("v8-waterlogged", arena, &a));
        CHECK(mcme_leveldb_open(&db, dbdir));
        CHECK(load_subchunk(db, arena, 0, 0, 0, 1, &b));
        CHECK(b.layer_count == 2);
        CHECK(b.layers[1].bits == a.layers[1].bits);
        for (size_t u = 0; u < 4096; ++u) {
            CHECK(b.layers[1].indices[u]
                  == a.layers[1].indices[u]);
        }
        for (size_t j = 0; j < a.layers[1].palette_len; ++j) {
            CHECK(docs_equal(&b.layers[1].palette[j],
                             &a.layers[1].palette[j]));
        }
        mcme_leveldb_close(db);
        db = NULL;

        /* A subchunk the box does not reach is byte for
           byte the fixture. */
        {
            void *want = NULL;
            size_t want_len = 0;
            CHECK(mcme_fs_read("tests/golden/subchunks/v1-legacy.bin",
                               &want, &want_len));
            McmeBuffer raw;
            mcme_buffer_init(&raw);
            CHECK(mcme_leveldb_open(&db, dbdir));
            CHECK(raw_subchunk(db, 0, 0, 0, 2, &raw));
            CHECK(raw.len == want_len
                  && memcmp(raw.data, want, want_len) == 0);
            mcme_leveldb_close(db);
            db = NULL;
            mcme_buffer_free(&raw);
            free(want);
        }

        remove_store(dbdir);
        rmdir(tmp);
    }

    /* --- A fill whose box reaches into ground that was
       never stored creates those subchunks; the stored
       ground around them is untouched. --- */
    {
        CHECK(fresh_store_dir(tmp, sizeof tmp));
        snprintf(dbdir, sizeof dbdir, "%s/db", tmp);
        CHECK(copy_store(src, dbdir));
        CHECK(mcme_bedrock_block_entry(arena, "mcme:placed",
                                       NULL, &placed));
        McmeNbtDoc air;
        CHECK(mcme_bedrock_block_entry(arena, "minecraft:air",
                                       NULL, &air));

        /* The whole of cy3, which the world never stored. */
        McmeBox hole = {0, 48, 0, 15, 63, 15};
        CHECK(mcme_leveldb_open_rw(&db, dbdir));
        CHECK(mcme_bedrock_fill_check(db, &hole, 0, NULL));
        CHECK(mcme_bedrock_fill(db, &hole, 0, &placed, NULL));
        mcme_leveldb_close(db);
        db = NULL;

        /* Nothing outside the box moved: cy0 is still the
           fixture, byte for byte. */
        {
            void *want = NULL;
            size_t want_len = 0;
            CHECK(mcme_fs_read("tests/golden/subchunks/v8-bits4.bin",
                               &want, &want_len));
            McmeBuffer raw;
            mcme_buffer_init(&raw);
            CHECK(mcme_leveldb_open(&db, dbdir));
            CHECK(raw_subchunk(db, 0, 0, 0, 0, &raw));
            CHECK(raw.len == want_len
                  && memcmp(raw.data, want, want_len) == 0);

            /* cy3 did not exist; it does now, entirely
               filled, in the version its neighbour cy4
               uses. */
            McmeBedrockSubchunk c;
            CHECK(load_subchunk(db, arena, 0, 0, 0, 3, &c));
            CHECK(c.version == 9);
            CHECK(c.has_index && c.index == 3);
            CHECK(c.layer_count == 1);
            CHECK(c.layers[0].palette_len == 2);
            CHECK(docs_equal(&c.layers[0].palette[0], &air));
            CHECK(docs_equal(&c.layers[0].palette[1], &placed));
            for (size_t u = 0; u < 4096; ++u) {
                CHECK(c.layers[0].indices[u] == 1);
            }
            mcme_leveldb_close(db);
            db = NULL;
            mcme_buffer_free(&raw);
            free(want);
        }

        /* A box reaching past the key's signed byte is
           refused as unfindable, and null arguments are
           refused; each says which class of failure it
           was, so the caller can pick its exit code. */
        CHECK(mcme_leveldb_open_rw(&db, dbdir));
        McmeBox deep = {0, -3000, 0, 0, -2000, 0};
        McmeWriteRefusal refusal = MCME_WRITE_REFUSAL_NOT_THERE;
        CHECK(!mcme_bedrock_fill_check(db, &deep, 0, &refusal));
        CHECK(refusal == MCME_WRITE_REFUSAL_USAGE);
        CHECK(!mcme_bedrock_fill(db, &deep, 0, &placed, NULL));
        refusal = MCME_WRITE_REFUSAL_NOT_THERE;
        CHECK(!mcme_bedrock_fill_check(NULL, &deep, 0, &refusal));
        CHECK(refusal == MCME_WRITE_REFUSAL_USAGE);
        refusal = MCME_WRITE_REFUSAL_NOT_THERE;
        CHECK(!mcme_bedrock_fill_check(db, NULL, 0, &refusal));
        CHECK(refusal == MCME_WRITE_REFUSAL_USAGE);
        CHECK(!mcme_bedrock_fill(db, NULL, 0, &placed, NULL));
        CHECK(!mcme_bedrock_fill(db, &deep, 0, NULL, NULL));
        CHECK(mcme_last_error()[0] != '\0');
        mcme_leveldb_close(db);
        db = NULL;

        remove_store(dbdir);
        rmdir(tmp);
    }

    /* --- stats counts what a box is made of: the tally of a
       subchunk's own indices, air for a subchunk that is not
       stored, refusal for one that does not decode. --- */
    {
        CHECK(fresh_store_dir(tmp, sizeof tmp));
        snprintf(dbdir, sizeof dbdir, "%s/db", tmp);
        CHECK(copy_store(src, dbdir));
        CHECK(decode_fixture("v8-bits4", arena, &a));
        CHECK(mcme_leveldb_open_rw(&db, dbdir));

        /* A payload that does not decode must fail the count.
           Plant it at cy5 before any counting happens. */
        uint8_t junk[8] = {8, 250, 'x', 'x', 'x', 'x', 'x', 'x'};
        uint8_t skey[14];
        size_t skey_len = 0;
        CHECK(mcme_bedrock_key_build(skey, sizeof skey, &skey_len,
                                     0, 0, 0,
                                     MCME_BEDROCK_TAG_SUBCHUNK,
                                     true, 5));
        CHECK(mcme_leveldb_put(db, skey, skey_len, junk,
                               sizeof junk));
        /* The same unreadable payload refuses a fill before it
           writes anything, and the refusal says "not readable"
           rather than "not there". */
        McmeWriteRefusal refusal = MCME_WRITE_REFUSAL_NOT_THERE;
        McmeBox unreadable = {0, 80, 0, 0, 80, 0};  /* cy5: junk */
        CHECK(!mcme_bedrock_fill_check(db, &unreadable, 0,
                                       &refusal));
        CHECK(refusal == MCME_WRITE_REFUSAL_NOT_READABLE);
        CHECK(mcme_last_error()[0] != '\0');

        /* The tally of a box inside one subchunk, against the
           fixture's own indices counted by hand. */
        McmeBox box = {0, 0, 0, 3, 15, 3};   /* 4x16x4 of cy0 */
        McmeBlockCount *tally = NULL;
        size_t tally_len = 0;
        int64_t missing = -1;
        CHECK(mcme_bedrock_count_box(db, &box, 0, arena, &tally,
                                     &tally_len, &missing));
        CHECK_MSG(missing == 0, "stats: nothing missing, got %lld",
                  (long long)missing);
        CHECK(tally != NULL && tally_len > 0);
        CHECK(block_count_cmp_ok(tally, tally_len));

        size_t pal_len = a.layers[0].palette_len;
        int64_t *want = mcme_arena_alloc(
                                         arena, pal_len * sizeof *want);
        CHECK(want != NULL);
        memset(want, 0, pal_len * sizeof *want);
        for (int lx = 0; lx <= 3; ++lx) {
            for (int lz = 0; lz <= 3; ++lz) {
                for (int ly = 0; ly <= 15; ++ly) {
                    size_t u = (size_t)lx * 256
                             + (size_t)lz * 16 + (size_t)ly;
                    want[a.layers[0].indices[u]] += 1;
                }
            }
        }
        int64_t sum = 0;
        for (size_t i = 0; i < tally_len; ++i) {
            sum += tally[i].count;
        }
        CHECK_MSG(sum == 256, "stats: box volume, got %lld",
                  (long long)sum);

        size_t matched = 0;
        for (size_t j = 0; j < pal_len; ++j) {
            if (want[j] == 0) continue;
            McmeBuffer spec;
            mcme_buffer_init(&spec);
            CHECK(mcme_bedrock_block_spec(a.layers[0].palette[j].root, &spec));
            int at = tally_find(tally, tally_len,
                                (const char *)spec.data, spec.len);
            CHECK(at >= 0);
            CHECK(tally[at].count == want[j]);
            matched += 1;
            mcme_buffer_free(&spec);
        }
        CHECK_MSG(matched == tally_len,
                     "stats: every row accounted, %zu of %zu",
                  matched, tally_len);

        /* The fixture's own pattern makes stone 16 of the 256. */
        int stone = tally_find(tally, tally_len,
                               "minecraft:stone", 15);
        CHECK(stone >= 0);
        CHECK_MSG(tally[stone].count == 16,
                     "stats: stone count, got %lld",
                  (long long)tally[stone].count);

        /* A box whose subchunk is not stored counts as air and
           is reported, but is not an error. */
        McmeBox hole = {0, 0, 0, 0, 48, 0};  /* y48 in cy3 */
        tally = NULL;
        tally_len = 0;
        missing = -1;
        CHECK(mcme_bedrock_count_box(db, &hole, 0, arena, &tally,
                                     &tally_len, &missing));
        CHECK_MSG(missing == 1, "stats: one missing, got %lld",
                  (long long)missing);
        CHECK(block_count_cmp_ok(tally, tally_len));
        int air = tally_find(tally, tally_len, "minecraft:air", 13);
        CHECK(air >= 0);
        CHECK_MSG(tally[air].count == 1, "stats: air count, got %lld",
                  (long long)tally[air].count);
        sum = 0;
        for (size_t i = 0; i < tally_len; ++i) {
            sum += tally[i].count;
        }
        CHECK_MSG(sum == 49, "stats: hole volume, got %lld",
                  (long long)sum);

        /* Undecodable world data fails; so do null arguments and
           a Y the key cannot name. */
        McmeBox bad = {0, 80, 0, 0, 80, 0};  /* cy5: junk */
        CHECK(!mcme_bedrock_count_box(db, &bad, 0, arena, &tally,
                                      &tally_len, &missing));
        CHECK(mcme_last_error()[0] != '\0');
        CHECK(!mcme_bedrock_count_box(NULL, &hole, 0, arena,
                                      &tally, &tally_len, &missing));
        CHECK(!mcme_bedrock_count_box(db, &hole, 0, NULL,
                                      &tally, &tally_len, &missing));
        CHECK(!mcme_bedrock_count_box(db, &hole, 0, arena,
                                      NULL, &tally_len, &missing));
        CHECK(!mcme_bedrock_count_box(db, &hole, 0, arena,
                                      &tally, &tally_len, NULL));
        McmeBox deep = {0, 3000, 0, 0, 3200, 0};
        CHECK(!mcme_bedrock_count_box(db, &deep, 0, arena, &tally,
                                      &tally_len, &missing));
        McmeBuffer nope;
        mcme_buffer_init(&nope);
        CHECK(!mcme_bedrock_block_spec(NULL, &nope));
        CHECK(mcme_last_error()[0] != '\0');
        mcme_buffer_free(&nope);

        mcme_leveldb_close(db);
        db = NULL;

        remove_store(dbdir);
        rmdir(tmp);
    }

    mcme_arena_free(arena);
}

/* ------------------------------------------------------------------------
 * Column height
 *
 * What a planner asks before placing anything: which subchunks this column
 * holds and how high its ground is. The fixture's chunk (0, 0) spans six
 * subchunks — negative Y, both wrapper versions, and a version-1 payload —
 * so one column exercises the range, and the answers are checked against
 * the fixture's own index pattern: at local (0, 0) the block of subchunk
 * cy is palette[(cy * 16 + ly) ...], the pattern (lx*7 + ly*29 + lz*13 +
 * seed) mod palette_len evaluated for lx = lz = 0.
 * ---------------------------------------------------------------------- */

static void test_bedrock_column(void)
{
    char src[512];
    snprintf(src, sizeof src, "%s/db", kChunksWorld);
    if (!mcme_fs_exists(src)) {
        printf("  SKIP column height tests (%s not found; run "
               "tools/ref_bedrock_store.py)\n", src);
        return;
    }

    McmeArena *arena = mcme_arena_new();
    if (arena == NULL) {
        CHECK_MSG(false, "out of memory");
        return;
    }

    char tmp[64];
    char dbdir[512];
    McmeLevelDb *db = NULL;
    McmeColumnHeight col;
    McmeNbtDoc placed;

    /* --- The fixture's own column: six subchunks, ground at both
       ends of the range. --- */
    {
        CHECK(fresh_store_dir(tmp, sizeof tmp));
        snprintf(dbdir, sizeof dbdir, "%s/db", tmp);
        CHECK(copy_store(src, dbdir));
        CHECK(mcme_leveldb_open(&db, dbdir));

        CHECK(mcme_bedrock_column_height(db, 0, 0, 0, arena, &col));
        CHECK(col.subchunk_count == 6);
        if (col.subchunk_count == 6) {
            const int8_t want[] = { -4, -1, 0, 1, 2, 4 };
            for (size_t i = 0; i < 6; ++i) {
                CHECK_MSG(col.subchunks[i] == want[i],
                          "subchunk %zu is %d, want %d", i,
                          (int)col.subchunks[i], (int)want[i]);
            }
        }
        /* The top of the column is cy 4's highest block, which the
           pattern puts at palette index 3 — oak_log. */
        CHECK(col.has_surface && col.surface_y == 79);
        if (col.has_surface) {
            CHECK(strcmp(col.surface_block,
                         "minecraft:oak_log{pillar_axis: \"x\"}")
                  == 0);
        }
        /* The bottom is cy -4's lowest block: index 41 mod 8 = 1,
           the second entry of its palette, dirt. */
        CHECK(col.has_floor && col.floor_y == -64);
        if (col.has_floor) {
            CHECK(strcmp(col.floor_block, "minecraft:dirt") == 0);
        }

        /* A second column of the same world answers from its own
           chunk: one subchunk, its own two ends. */
        CHECK(mcme_bedrock_column_height(db, 16, -32, 0, arena,
                                         &col));
        CHECK(col.subchunk_count == 1);
        CHECK(col.has_surface && col.surface_y == 15);
        if (col.has_surface) {
            CHECK(strcmp(col.surface_block,
                         "mcme:fixture_194{variant: 194}") == 0);
        }
        CHECK(col.has_floor && col.floor_y == 0);
        if (col.has_floor) {
            CHECK(strcmp(col.floor_block,
                         "mcme:fixture_016{variant: 16}") == 0);
        }

        /* The nether key is the same coordinates in another
           dimension, and answers on its own. */
        CHECK(mcme_bedrock_column_height(db, 0, 0, 1, arena,
                                         &col));
        CHECK(col.subchunk_count == 1);
        CHECK(col.has_surface && col.surface_y == 15);
        if (col.has_surface) {
            CHECK(strcmp(col.surface_block, "minecraft:stone")
                  == 0);
        }
        CHECK(col.has_floor && col.floor_y == 0);
        if (col.has_floor) {
            CHECK(strcmp(col.floor_block, "minecraft:dirt") == 0);
        }

        /* Ground the world never stored: no subchunk, so nothing
           to measure — which is not the same as a column of air,
           and is what the CLI reports as exit 2. */
        CHECK(mcme_bedrock_column_height(db, 500, 500, 0, arena,
                                         &col));
        CHECK(col.subchunk_count == 0);
        CHECK(!col.has_surface && !col.has_floor);

        /* Usage faults do not touch the store. */
        CHECK(!mcme_bedrock_column_height(NULL, 0, 0, 0, arena,
                                          &col));
        CHECK(!mcme_bedrock_column_height(db, 0, 0, 0, NULL,
                                          &col));
        CHECK(!mcme_bedrock_column_height(db, 0, 0, 0, arena,
                                          NULL));

        mcme_leveldb_close(db);
        db = NULL;

        remove_store(dbdir);
        rmdir(tmp);
    }

    /* --- The store decides: delete the top subchunk and the
       surface drops to the one below it. --- */
    {
        uint8_t key[14];
        size_t key_len = 0;
        CHECK(fresh_store_dir(tmp, sizeof tmp));
        snprintf(dbdir, sizeof dbdir, "%s/db", tmp);
        CHECK(copy_store(src, dbdir));
        CHECK(mcme_bedrock_key_build(key, sizeof key, &key_len,
                                     0, 0, 0,
                                     MCME_BEDROCK_TAG_SUBCHUNK,
                                     true, 4));
        CHECK(mcme_leveldb_open_rw(&db, dbdir));
        CHECK(mcme_leveldb_delete(db, key, key_len));
        mcme_leveldb_close(db);
        db = NULL;

        CHECK(mcme_leveldb_open(&db, dbdir));
        CHECK(mcme_bedrock_column_height(db, 0, 0, 0, arena, &col));
        CHECK(col.subchunk_count == 5);
        /* cy 2 is the version-1 payload, whose top block the
           pattern puts at palette index 3: grass. */
        CHECK(col.has_surface && col.surface_y == 47);
        if (col.has_surface) {
            CHECK(strcmp(col.surface_block, "minecraft:grass")
                  == 0);
        }
        CHECK(col.has_floor && col.floor_y == -64);
        mcme_leveldb_close(db);
        db = NULL;

        remove_store(dbdir);
        rmdir(tmp);
    }

    /* --- A payload this build does not read fails the whole
       column: that is data mcme cannot measure, not air. --- */
    {
        uint8_t key[14];
        size_t key_len = 0;
        CHECK(fresh_store_dir(tmp, sizeof tmp));
        snprintf(dbdir, sizeof dbdir, "%s/db", tmp);
        CHECK(copy_store(src, dbdir));
        CHECK(mcme_bedrock_key_build(key, sizeof key, &key_len,
                                     0, 0, 0,
                                     MCME_BEDROCK_TAG_SUBCHUNK,
                                     true, 8));
        CHECK(mcme_leveldb_open_rw(&db, dbdir));
        CHECK(mcme_leveldb_put(db, key, key_len, "\x08\xfa"
                               "xxxxxx", 8));
        mcme_leveldb_close(db);
        db = NULL;

        CHECK(mcme_leveldb_open(&db, dbdir));
        CHECK(!mcme_bedrock_column_height(db, 0, 0, 0, arena,
                                          &col));
        CHECK(mcme_last_error()[0] != '\0');
        mcme_leveldb_close(db);
        db = NULL;

        remove_store(dbdir);
        rmdir(tmp);
    }

    /* --- Two stored columns that hold no ground: the all-air
       marker the game writes, and a subchunk mcme filled with
       air. Both answer, and neither has a surface. --- */
    {
        uint8_t key[14];
        size_t key_len = 0;
        CHECK(fresh_store_dir(tmp, sizeof tmp));
        snprintf(dbdir, sizeof dbdir, "%s/db", tmp);
        CHECK(copy_store(src, dbdir));
        CHECK(mcme_bedrock_key_build(key, sizeof key, &key_len,
                                     300 / 16, 300 / 16, 0,
                                     MCME_BEDROCK_TAG_SUBCHUNK,
                                     true, 8));
        CHECK(mcme_leveldb_open_rw(&db, dbdir));
        /* Version 8, zero storage layers: the marker. */
        CHECK(mcme_leveldb_put(db, key, key_len, "\x08\x00", 2));
        mcme_leveldb_close(db);
        db = NULL;

        CHECK(mcme_leveldb_open(&db, dbdir));
        CHECK(mcme_bedrock_column_height(db, 300, 300, 0, arena,
                                         &col));
        CHECK(col.subchunk_count == 1 && col.subchunks[0] == 8);
        CHECK(!col.has_surface && !col.has_floor);
        mcme_leveldb_close(db);
        db = NULL;

        remove_store(dbdir);
        rmdir(tmp);
    }

    /* --- A write into ground that was never saved is visible to
       the next height read: the subchunk it created, and the one
       block it placed. --- */
    {
        CHECK(fresh_store_dir(tmp, sizeof tmp));
        snprintf(dbdir, sizeof dbdir, "%s/db", tmp);
        CHECK(copy_store(src, dbdir));
        CHECK(mcme_bedrock_block_entry(arena, "mcme:placed",
                                       NULL, &placed));
        CHECK(mcme_bedrock_set_block(dbdir, 200, 40, 200, 0,
                                     &placed, NULL));

        CHECK(mcme_leveldb_open(&db, dbdir));
        CHECK(mcme_bedrock_column_height(db, 200, 200, 0, arena,
                                         &col));
        CHECK(col.subchunk_count == 1 && col.subchunks[0] == 2);
        CHECK(col.has_surface && col.surface_y == 40);
        if (col.has_surface) {
            CHECK(strcmp(col.surface_block, "mcme:placed") == 0);
        }
        CHECK(col.has_floor && col.floor_y == 40);
        if (col.has_floor) {
            CHECK(strcmp(col.floor_block, "mcme:placed") == 0);
        }
        mcme_leveldb_close(db);
        db = NULL;

        remove_store(dbdir);
        rmdir(tmp);
    }

    mcme_arena_free(arena);
}

/* ------------------------------------------------------------------------
 * Block entities
 *
 * A chunk's entities are one record of concatenated documents, so reading
 * one is a search by the coordinates the document carries. The fixture
 * holds three documents in one chunk (a chest, a sign, and one with no
 * coordinates at all), a furnace in a second chunk, and a chest in the
 * nether — the cases that make the search, the chunk addressing and the
 * dimension key each prove themselves.
 * ---------------------------------------------------------------------- */

static void test_bedrock_entity(void)
{
    char src[512];
    snprintf(src, sizeof src, "%s/db", kEntitiesWorld);
    if (!mcme_fs_exists(src)) {
        printf("  SKIP block entity tests (%s not found; run "
               "tools/ref_bedrock_entities.py)\n", src);
        return;
    }

    McmeArena *arena = mcme_arena_new();
    if (arena == NULL) {
        CHECK_MSG(false, "out of memory");
        return;
    }

    char tmp[64];
    char dbdir[512];
    McmeLevelDb *db = NULL;
    McmeNbtDoc doc;

    CHECK(fresh_store_dir(tmp, sizeof tmp));
    snprintf(dbdir, sizeof dbdir, "%s/db", tmp);
    CHECK(copy_store(src, dbdir));
    CHECK(mcme_leveldb_open(&db, dbdir));

    /* --- The chest: the first of three documents in one record, so
       the read had to pass the sign and the coordinateless one to
       find it. --- */
    CHECK(mcme_bedrock_block_entity(db, 12, 64, 3, 0, arena,
                                    &doc)
          == MCME_ENTITY_OK);
    const McmeTag *id = mcme_nbt_path(doc.root, "id");
    CHECK(id != NULL && id->type == MCME_TAG_String
          && strcmp(id->string.data, "minecraft:chest") == 0);
    const McmeTag *items = mcme_nbt_path(doc.root, "Items");
    CHECK(items != NULL && items->type == MCME_TAG_List
          && items->list.len == 2);
    /* The item's own fields, reached through the list index the
       path lookup understands. */
    const McmeTag *first = mcme_nbt_path(doc.root, "Items.0.id");
    CHECK(first != NULL && first->type == MCME_TAG_String
          && strcmp(first->string.data, "minecraft:diamond") == 0);
    const McmeTag *count = mcme_nbt_path(doc.root, "Items.0.count");
    CHECK(count != NULL && count->type == MCME_TAG_Byte
          && count->as_byte == 3);

    /* --- The sign is the second document of the same record. --- */
    CHECK(mcme_bedrock_block_entity(db, 0, 65, 0, 0, arena, &doc)
          == MCME_ENTITY_OK);
    const McmeTag *text = mcme_nbt_path(doc.root, "Text");
    CHECK(text != NULL && text->type == MCME_TAG_String
          && strcmp(text->string.data, "mcme fixture") == 0);

    /* --- A furnace in another chunk: the record is read from the
       chunk the coordinates fall in, floor division included. --- */
    CHECK(mcme_bedrock_block_entity(db, 16, 8, -32, 0, arena,
                                    &doc)
          == MCME_ENTITY_OK);
    id = mcme_nbt_path(doc.root, "id");
    CHECK(id != NULL && strcmp(id->string.data, "minecraft:furnace")
          == 0);
    const McmeTag *burn = mcme_nbt_path(doc.root, "BurnTime");
    CHECK(burn != NULL && burn->type == MCME_TAG_Short
          && burn->as_short == 100);

    /* --- Same local coordinates, one chunk over: no record there,
       so no entity — the record belongs to its chunk. --- */
    CHECK(mcme_bedrock_block_entity(db, 12, 64, 19, 0, arena,
                                    &doc)
          == MCME_ENTITY_ABSENT);

    /* --- A negative chunk coordinate is still one chunk. --- */
    CHECK(mcme_bedrock_block_entity(db, 16, 8, -32, 1, arena,
                                    &doc)
          == MCME_ENTITY_ABSENT);
    CHECK(mcme_bedrock_block_entity(db, 0, 5, 0, 1, arena, &doc)
          == MCME_ENTITY_OK);
    id = mcme_nbt_path(doc.root, "id");
    CHECK(id != NULL && strcmp(id->string.data, "minecraft:chest")
          == 0);

    /* --- Nothing at that block: the coordinateless document is not
       matched to a position, which is what its absence pins. --- */
    CHECK(mcme_bedrock_block_entity(db, 12, 64, 4, 0, arena, &doc)
          == MCME_ENTITY_ABSENT);
    CHECK(mcme_bedrock_block_entity(db, 5, 5, 5, 0, arena, &doc)
          == MCME_ENTITY_ABSENT);

    /* --- Usage faults do not touch the store. --- */
    CHECK(mcme_bedrock_block_entity(NULL, 0, 0, 0, 0, arena,
                                    &doc)
          == MCME_ENTITY_USAGE);
    CHECK(mcme_bedrock_block_entity(db, 0, 0, 0, 0, NULL, &doc)
          == MCME_ENTITY_USAGE);
    CHECK(mcme_bedrock_block_entity(db, 0, 0, 0, 0, arena, NULL)
          == MCME_ENTITY_USAGE);

    mcme_leveldb_close(db);
    db = NULL;

    remove_store(dbdir);
    rmdir(tmp);

    /* --- The store decides: a record the game deleted is an
       absence, and one that will not decode is exit 3's class. --- */
    {
        uint8_t key[14];
        size_t key_len = 0;
        CHECK(fresh_store_dir(tmp, sizeof tmp));
        snprintf(dbdir, sizeof dbdir, "%s/db", tmp);
        CHECK(copy_store(src, dbdir));
        CHECK(mcme_bedrock_key_build(key, sizeof key, &key_len, 0,
                                     0, 0,
                                     MCME_BEDROCK_TAG_BLOCK_ENTITY,
                                     false, 0));
        CHECK(mcme_leveldb_open_rw(&db, dbdir));
        CHECK(mcme_leveldb_delete(db, key, key_len));
        mcme_leveldb_close(db);
        db = NULL;

        CHECK(mcme_leveldb_open(&db, dbdir));
        CHECK(mcme_bedrock_block_entity(db, 12, 64, 3, 0, arena,
                                        &doc)
              == MCME_ENTITY_ABSENT);
        mcme_leveldb_close(db);
        db = NULL;

        remove_store(dbdir);
        rmdir(tmp);
    }

    {
        uint8_t key[14];
        size_t key_len = 0;
        CHECK(fresh_store_dir(tmp, sizeof tmp));
        snprintf(dbdir, sizeof dbdir, "%s/db", tmp);
        CHECK(copy_store(src, dbdir));
        CHECK(mcme_bedrock_key_build(key, sizeof key, &key_len, 0,
                                     0, 0,
                                     MCME_BEDROCK_TAG_BLOCK_ENTITY,
                                     false, 0));
        CHECK(mcme_leveldb_open_rw(&db, dbdir));
        /* A document that ends mid-tag: the record will not
           decode, and that is data mcme cannot read. */
        CHECK(mcme_leveldb_put(db, key, key_len,
                               "\x0a\x00\x03id\x08\x00\x04xx", 11));
        mcme_leveldb_close(db);
        db = NULL;

        CHECK(mcme_leveldb_open(&db, dbdir));
        CHECK(mcme_bedrock_block_entity(db, 12, 64, 3, 0, arena,
                                        &doc)
              == MCME_ENTITY_NOT_READABLE);
        CHECK(mcme_last_error()[0] != '\0');
        mcme_leveldb_close(db);
        db = NULL;

        remove_store(dbdir);
        rmdir(tmp);
    }

    mcme_arena_free(arena);
}

/* ------------------------------------------------------------------------
 * Block entity writes
 *
 * The record is spliced: only the document being changed is encoded,
 * and the rest goes back as the bytes it arrived as. "The sign beside
 * it is untouched" is a claim about bytes, not fields, so the splice
 * is asserted byte for byte — before's prefix, this document, before's
 * suffix IS the store's new value — with the fields checked after.
 * ---------------------------------------------------------------------- */

/* Where each document of a record sits, by the same walk the reader
   does. Returns the count, or -1 when any part will not decode. */
static int record_extents(const uint8_t *data, size_t len,
                          McmeArena *arena, size_t *offs,
                          size_t *lens, int max)
{
    int n = 0;
    size_t at = 0;
    while (at < len) {
        McmeNbtDoc doc;
        size_t consumed = 0;
        if (!mcme_nbt_parse_prefix(&doc, arena, data + at, len - at,
                                   true, &consumed)
                || consumed == 0) {
            return -1;
        }
        if (n < max) {
            offs[n] = at;
            lens[n] = consumed;
        }
        n++;
        at += consumed;
    }
    return n;
}

/* The raw value the store holds for one chunk's entity record, fresh
   in `out` (mcme_leveldb_get appends, so a caller's leftovers must not
   become part of the answer). Returns false when the key is absent;
   `deleted` says tombstone. */
static bool raw_record(const char *db_dir, int32_t cx, int32_t cz,
                       int32_t dimension, McmeBuffer *out,
                       bool *deleted)
{
    uint8_t key[14];
    size_t key_len = 0;
    if (!mcme_bedrock_key_build(key, sizeof key, &key_len, cx, cz,
                                dimension,
                                MCME_BEDROCK_TAG_BLOCK_ENTITY,
                                false, 0)) {
        return false;
    }
    mcme_buffer_free(out);
    mcme_buffer_init(out);
    *deleted = false;
    McmeLevelDb *db = NULL;
    if (!mcme_leveldb_open(&db, db_dir)) return false;
    bool got = mcme_leveldb_get(db, key, key_len, out, deleted);
    mcme_leveldb_close(db);
    return got;
}

/* One question, one handle: a write takes the LOCK, so no handle may
   be held across one. */
static McmeEntityStatus ask(const char *db_dir, int32_t x, int32_t y,
                            int32_t z, int32_t dimension,
                            McmeArena *arena, McmeNbtDoc *out)
{
    McmeLevelDb *db = NULL;
    if (!mcme_leveldb_open(&db, db_dir)) return MCME_ENTITY_STORE;
    McmeEntityStatus status =
        mcme_bedrock_block_entity(db, x, y, z, dimension, arena, out);
    mcme_leveldb_close(db);
    return status;
}

static void test_bedrock_entity_write(void)
{
    char src[512];
    snprintf(src, sizeof src, "%s/db", kEntitiesWorld);
    if (!mcme_fs_exists(src)) {
        printf("  SKIP block entity write tests (%s not found; run "
               "tools/ref_bedrock_entities.py)\n", src);
        return;
    }

    McmeArena *arena = mcme_arena_new();
    if (arena == NULL) {
        CHECK_MSG(false, "out of memory");
        return;
    }

    char tmp[64];
    char dbdir[512];
    CHECK(fresh_store_dir(tmp, sizeof tmp));
    snprintf(dbdir, sizeof dbdir, "%s/db", tmp);
    CHECK(copy_store(src, dbdir));

    McmeWriteRefusal refusal = MCME_WRITE_REFUSAL_NONE;
    McmeBuffer before, after, docbytes, want;
    mcme_buffer_init(&before);
    mcme_buffer_init(&after);
    mcme_buffer_init(&docbytes);
    mcme_buffer_init(&want);
    bool deleted = false;
    McmeNbtDoc got;

    /* --- Replace the chest, with a document that carries no
       coordinates: the position is the argument, so they are set
       from it. The record must still hold its three documents
       before the edit, or the splice proves nothing. --- */
    const char *chest = "{id: \"minecraft:chest\", "
                        "CustomName: \"stores\"}";
    McmeTag *entity = NULL;
    CHECK(mcme_nbt_parse_snbt(arena, chest, strlen(chest), &entity));
    CHECK(raw_record(dbdir, 0, 0, 0, &before, &deleted));
    CHECK(!deleted && before.len > 0);

    size_t offs[8], lens[8];
    int n = record_extents(before.data, before.len, arena, offs,
                           lens, 8);
    CHECK_MSG(n == 3, "the fixture record holds %d documents, want 3",
              n);

    CHECK(mcme_bedrock_put_block_entity(dbdir, 12, 64, 3, 0, entity,
                                        arena, &refusal));
    const McmeTag *gy = mcme_nbt_path(entity, "y");
    CHECK(gy != NULL && gy->type == MCME_TAG_Int && gy->as_int == 64);

    /* --- The neighbours are byte for byte: prefix, this document
       as written, suffix — all of before, with only the chest's
       extent exchanged. --- */
    McmeNbtDoc written;
    written.arena = arena;
    written.root = entity;
    written.root_name.data = (char *)"";
    written.root_name.len = 0;
    written.little_endian = true;
    CHECK(mcme_nbt_write_binary(&written, &docbytes));
    size_t tail = offs[0] + lens[0];
    bool grew = mcme_buffer_append(&want, before.data, offs[0]);
    grew = grew
        && mcme_buffer_append(&want, docbytes.data, docbytes.len);
    grew = grew
        && mcme_buffer_append(&want, before.data + tail,
                              before.len - tail);
    CHECK(grew);
    CHECK(raw_record(dbdir, 0, 0, 0, &after, &deleted));
    CHECK_MSG(after.len == want.len,
              "the record is %zu bytes, want the splice's %zu",
              after.len, want.len);
    if (after.len == want.len) {
        CHECK(memcmp(after.data, want.data, want.len) == 0);
    }

    /* --- The new document reads back as written, and the two
       documents behind it are as they were. --- */
    CHECK(ask(dbdir, 12, 64, 3, 0, arena, &got)
          == MCME_ENTITY_OK);
    const McmeTag *field = mcme_nbt_path(got.root, "CustomName");
    CHECK(field != NULL && field->type == MCME_TAG_String
          && strcmp(field->string.data, "stores") == 0);
    CHECK(ask(dbdir, 0, 65, 0, 0, arena, &got)
          == MCME_ENTITY_OK);
    field = mcme_nbt_path(got.root, "Text");
    CHECK(field != NULL && field->type == MCME_TAG_String
          && strcmp(field->string.data, "mcme fixture") == 0);
    CHECK(ask(dbdir, 12, 64, 4, 0, arena, &got)
          == MCME_ENTITY_ABSENT);

    /* --- A chunk that holds no record at all gets one — writes
       create, the way chunk set creates a subchunk. --- */
    const char *sign = "{id: \"minecraft:sign\", "
                       "Text: \"written\"}";
    McmeTag *fresh = NULL;
    CHECK(mcme_nbt_parse_snbt(arena, sign, strlen(sign), &fresh));
    CHECK(mcme_bedrock_put_block_entity(dbdir, 40, 10, 40, 0, fresh,
                                        arena, &refusal));
    CHECK(ask(dbdir, 40, 10, 40, 0, arena, &got)
          == MCME_ENTITY_OK);
    field = mcme_nbt_path(got.root, "Text");
    CHECK(field != NULL && strcmp(field->string.data, "written") == 0);

    /* --- Deleting one document leaves the others: the sign goes,
       the chest and the coordinateless document stay. --- */
    CHECK(mcme_bedrock_del_block_entity(dbdir, 0, 65, 0, 0,
                                        &refusal));
    CHECK(ask(dbdir, 0, 65, 0, 0, arena, &got)
          == MCME_ENTITY_ABSENT);
    CHECK(ask(dbdir, 12, 64, 3, 0, arena, &got)
          == MCME_ENTITY_OK);
    mcme_buffer_free(&before);
    mcme_buffer_init(&before);
    CHECK(raw_record(dbdir, 0, 0, 0, &before, &deleted));
    n = record_extents(before.data, before.len, arena, offs, lens, 8);
    CHECK_MSG(n == 2, "record holds %d documents after a deletion, "
              "want 2", n);

    /* --- The last entity's record goes with it: the key is a
       tombstone, which is the store's "no value", not a record
       holding nothing. --- */
    CHECK(mcme_bedrock_del_block_entity(dbdir, 40, 10, 40, 0,
                                        &refusal));
    CHECK(raw_record(dbdir, 2, 2, 0, &after, &deleted));
    CHECK(deleted && after.len == 0);
    CHECK(ask(dbdir, 40, 10, 40, 0, arena, &got)
          == MCME_ENTITY_ABSENT);

    /* --- Nothing there is a refusal that says so, not the store's
       "key not found". --- */
    CHECK(!mcme_bedrock_del_block_entity(dbdir, 5, 5, 5, 0,
                                         &refusal));
    CHECK(refusal == MCME_WRITE_REFUSAL_NOT_THERE);
    CHECK(strstr(mcme_last_error(), "no block entity") != NULL);
    CHECK(!mcme_bedrock_del_block_entity(dbdir, 40, 10, 40, 0,
                                         &refusal));
    CHECK(refusal == MCME_WRITE_REFUSAL_NOT_THERE);

    /* --- What mcme will not write: not an id in either spelling
       the game has. Each refusal leaves the record as it was. --- */
    mcme_buffer_free(&before);
    mcme_buffer_init(&before);
    CHECK(raw_record(dbdir, 0, 0, 0, &before, &deleted));

    McmeTag *bad = NULL;
    const char *no_id = "{CustomName: \"stores\"}";
    CHECK(mcme_nbt_parse_snbt(arena, no_id, strlen(no_id), &bad));
    CHECK(!mcme_bedrock_put_block_entity(dbdir, 12, 64, 3, 0, bad,
                                         arena, &refusal));
    CHECK(refusal == MCME_WRITE_REFUSAL_USAGE);
    CHECK(strstr(mcme_last_error(), "\"id\"") != NULL);

    const char *empty_name = "{id: \"minecraft:\"}";
    CHECK(mcme_nbt_parse_snbt(arena, empty_name, strlen(empty_name),
                              &bad));
    CHECK(!mcme_bedrock_put_block_entity(dbdir, 12, 64, 3, 0, bad,
                                         arena, &refusal));
    CHECK(refusal == MCME_WRITE_REFUSAL_USAGE);

    const char *stray_space = "{id: \"not an id\"}";
    CHECK(mcme_nbt_parse_snbt(arena, stray_space, strlen(stray_space),
                              &bad));
    CHECK(!mcme_bedrock_put_block_entity(dbdir, 12, 64, 3, 0, bad,
                                         arena, &refusal));
    CHECK(refusal == MCME_WRITE_REFUSAL_USAGE);

    McmeTag *number = NULL;
    CHECK(mcme_nbt_parse_snbt(arena, "42", 2, &number));
    CHECK(!mcme_bedrock_put_block_entity(dbdir, 12, 64, 3, 0, number,
                                         arena, &refusal));
    CHECK(refusal == MCME_WRITE_REFUSAL_USAGE);

    CHECK(!mcme_bedrock_put_block_entity(NULL, 12, 64, 3, 0, entity,
                                         arena, &refusal));
    CHECK(refusal == MCME_WRITE_REFUSAL_USAGE);
    CHECK(!mcme_bedrock_put_block_entity(dbdir, 12, 64, 3, 0, NULL,
                                         arena, &refusal));
    CHECK(refusal == MCME_WRITE_REFUSAL_USAGE);
    CHECK(!mcme_bedrock_del_block_entity(NULL, 0, 0, 0, 0, &refusal));
    CHECK(refusal == MCME_WRITE_REFUSAL_USAGE);

    /* --- The game's own spelling writes: a bare savegame id,
       stored as given and read back as given — the position
       argument still sets the coordinates. --- */
    const char *bare_snbt = "{id: \"Chest\", CustomName: \"bare\"}";
    McmeTag *bare = NULL;
    CHECK(mcme_nbt_parse_snbt(arena, bare_snbt, strlen(bare_snbt),
                              &bare));
    CHECK(mcme_bedrock_put_block_entity(dbdir, 13, 64, 3, 0, bare,
                                        arena, &refusal));
    CHECK(refusal == MCME_WRITE_REFUSAL_NONE);
    McmeLevelDb *read_db = NULL;
    CHECK(mcme_leveldb_open(&read_db, dbdir));
    if (read_db != NULL) {
        CHECK(mcme_bedrock_block_entity(read_db, 13, 64, 3, 0, arena,
                                        &got)
              == MCME_ENTITY_OK);
        const McmeTag *bid = mcme_nbt_path(got.root, "id");
        CHECK(bid != NULL && bid->type == MCME_TAG_String
              && strcmp(bid->string.data, "Chest") == 0);
        const McmeTag *bname = mcme_nbt_path(got.root, "CustomName");
        CHECK(bname != NULL && bname->type == MCME_TAG_String
              && strcmp(bname->string.data, "bare") == 0);
        CHECK(mcme_nbt_int_or(mcme_nbt_path(got.root, "x"), -1) == 13);
        mcme_leveldb_close(read_db);
    }

    /* --- And what it will not rewrite: a record with a tail that
       will not read. Both refusals come before any byte is
       written. --- */
    static const uint8_t junk_tail[] = {
        0x0a, 0x00, 0x03, 'i', 'd', 0x08, 0x00, 0x04, 'x', 'x'
    };
    McmeBuffer junk;
    mcme_buffer_init(&junk);
    CHECK(mcme_buffer_append(&junk, before.data, before.len));
    CHECK(mcme_buffer_append(&junk, junk_tail, sizeof junk_tail));
    {
        uint8_t key[14];
        size_t key_len = 0;
        CHECK(mcme_bedrock_key_build(key, sizeof key, &key_len, 0,
                                     0, 0,
                                     MCME_BEDROCK_TAG_BLOCK_ENTITY,
                                     false, 0));
        McmeLevelDb *db = NULL;
        CHECK(mcme_leveldb_open_rw(&db, dbdir));
        CHECK(mcme_leveldb_put(db, key, key_len, junk.data, junk.len));
        mcme_leveldb_close(db);
    }

    CHECK(!mcme_bedrock_put_block_entity(dbdir, 12, 64, 3, 0, entity,
                                         arena, &refusal));
    CHECK(refusal == MCME_WRITE_REFUSAL_NOT_READABLE);
    CHECK(!mcme_bedrock_del_block_entity(dbdir, 12, 64, 3, 0,
                                         &refusal));
    CHECK(refusal == MCME_WRITE_REFUSAL_NOT_READABLE);

    CHECK(raw_record(dbdir, 0, 0, 0, &after, &deleted));
    CHECK_MSG(after.len == junk.len,
              "a refused write changed the record: %zu bytes, want %zu",
              after.len, junk.len);
    if (after.len == junk.len) {
        CHECK(memcmp(after.data, junk.data, junk.len) == 0);
    }

    mcme_buffer_free(&junk);
    mcme_buffer_free(&docbytes);
    mcme_buffer_free(&want);
    mcme_buffer_free(&before);
    mcme_buffer_free(&after);
    mcme_arena_free(arena);
    remove_store(dbdir);
    rmdir(tmp);
}

/* ========================================================================
 * Translate — the Bedrock to Java block map
 * ======================================================================== */

/* The translation of one palette entry, as SNBT both ways. `want`
   NULL means the map must refuse the entry (legacy, or nameless). */
static void check_translate(McmeArena *arena, const char *in,
                            const char *want)
{
    McmeTag *entry = NULL;
    if (!mcme_nbt_parse_snbt(arena, in, strlen(in), &entry)) {
        CHECK_MSG(false, "translate input does not parse: %s", in);
        return;
    }
    McmeTag *out = mcme_translate_block(arena, entry);
    if (want == NULL) {
        CHECK_MSG(out == NULL, "translate should refuse '%s' (got '%s')",
                  in, out != NULL ? "an entry" : "nothing");
        if (out != NULL) mcme_clear_error();
        return;
    }
    if (out == NULL) {
        CHECK_MSG(false, "translate of '%s' failed: %s", in,
                  mcme_last_error());
        return;
    }
    McmeBuffer got;
    mcme_buffer_init(&got);
    CHECK(mcme_nbt_write_snbt(out, &got, 0));
    CHECK_MSG(got.len == strlen(want)
              && memcmp(got.data, want, got.len) == 0,
              "translate of %s = '%.*s', want '%s'", in, (int)got.len,
              got.data != NULL ? (const char *)got.data : "", want);
    mcme_buffer_free(&got);
}

static void test_translate(void)
{
    printf("  translate (bedrock -> java block map)\n");
    McmeArena *arena = mcme_arena_new();
    CHECK(arena != NULL);
    if (arena == NULL) return;

    /* Both editions spell it the same: through, value tag untouched. */
    check_translate(arena,
        "{name:\"minecraft:stone\",states:{},version:1}",
        "{Name: \"minecraft:stone\"}");
    check_translate(arena,
        "{name:\"minecraft:grass_block\","
        "states:{snowy:\"false\"},version:1}",
        "{Name: \"minecraft:grass_block\", "
        "Properties: {snowy: \"false\"}}");

    /* The axis rename every log, wood, stem and hyphae spells. */
    check_translate(arena,
        "{name:\"minecraft:oak_log\",states:{pillar_axis:\"x\"},version:1}",
        "{Name: \"minecraft:oak_log\", Properties: {axis: \"x\"}}");
    check_translate(arena,
        "{name:\"minecraft:birch_log\",states:{pillar_axis:\"y\"},version:1}",
        "{Name: \"minecraft:birch_log\", Properties: {axis: \"y\"}}");
    /* Java's mushroom stem takes no properties at all, so its
       pillar_axis is an unknown state like any other. */
    check_translate(arena,
        "{name:\"minecraft:mushroom_stem\","
        "states:{pillar_axis:\"z\"},version:1}",
        "{Name: \"minecraft:mushroom_stem\"}");

    /* A kept state with a byte value keeps the byte; a dropped one
       takes the Java default. */
    check_translate(arena,
        "{name:\"minecraft:furnace\","
        "states:{facing_direction:2,lit:0b},version:1}",
        "{Name: \"minecraft:furnace\", Properties: {lit: 0b}}");

    /* Red sand is a name, not a state, on the Java side. */
    check_translate(arena,
        "{name:\"minecraft:sand\",states:{sand_type:\"red\"},version:1}",
        "{Name: \"minecraft:red_sand\"}");
    check_translate(arena,
        "{name:\"minecraft:sand\",states:{},version:1}",
        "{Name: \"minecraft:sand\"}");

    /* The name table's wholesale renames: names the real world holds
       that Java spells apart, with no state deciding anything. The
       water's liquid_depth is an unknown state like any other, so
       the name changes and the state goes to the Java default. */
    check_translate(arena,
        "{name:\"minecraft:flowing_water\","
        "states:{liquid_depth:3},version:1}",
        "{Name: \"minecraft:water\"}");
    check_translate(arena,
        "{name:\"minecraft:magma\",states:{},version:1}",
        "{Name: \"minecraft:magma_block\"}");
    check_translate(arena,
        "{name:\"minecraft:snow_layer\",states:{height:4},version:1}",
        "{Name: \"minecraft:snow\"}");
    /* The blocks Java 1.20 does not have answer as air. */
    check_translate(arena,
        "{name:\"minecraft:trial_spawner\",states:{},version:1}",
        "{Name: \"minecraft:air\"}");

    /* A rename that hangs off one state value: a log's species, a
       plant's kind, a torch against a wall. The axis family is
       keyed on the Java name, so spruce_log still carries axis. */
    check_translate(arena,
        "{name:\"minecraft:log\","
        "states:{old_log_type:\"spruce\",pillar_axis:\"x\"},version:1}",
        "{Name: \"minecraft:spruce_log\", Properties: {axis: \"x\"}}");
    check_translate(arena,
        "{name:\"minecraft:log\",states:{},version:1}",
        "{Name: \"minecraft:oak_log\"}");
    check_translate(arena,
        "{name:\"minecraft:double_plant\","
        "states:{double_plant_type:\"fern\"},version:1}",
        "{Name: \"minecraft:large_fern\"}");
    check_translate(arena,
        "{name:\"minecraft:double_plant\",states:{},version:1}",
        "{Name: \"minecraft:sunflower\"}");
    check_translate(arena,
        "{name:\"minecraft:torch\","
        "states:{torch_facing_direction:\"west\"},version:1}",
        "{Name: \"minecraft:wall_torch\"}");
    /* Numeric conditions take any numeric tag. */
    check_translate(arena,
        "{name:\"minecraft:brown_mushroom_block\","
        "states:{huge_mushroom_bits:10},version:1}",
        "{Name: \"minecraft:mushroom_stem\"}");

    /* Renames that hang off a pair: a dead coral's colour, and a
       cauldron's liquid only once it holds something. */
    check_translate(arena,
        "{name:\"minecraft:coral_block\","
        "states:{coral_color:\"red\",dead_bit:0b},version:1}",
        "{Name: \"minecraft:fire_coral_block\"}");
    check_translate(arena,
        "{name:\"minecraft:coral_block\","
        "states:{coral_color:\"red\",dead_bit:1b},version:1}",
        "{Name: \"minecraft:dead_fire_coral_block\"}");
    check_translate(arena,
        "{name:\"minecraft:cauldron\","
        "states:{cauldron_liquid:\"lava\",fill_level:0},version:1}",
        "{Name: \"minecraft:cauldron\"}");
    check_translate(arena,
        "{name:\"minecraft:cauldron\","
        "states:{cauldron_liquid:\"lava\",fill_level:3},version:1}",
        "{Name: \"minecraft:lava_cauldron\"}");
    check_translate(arena,
        "{name:\"minecraft:cauldron\","
        "states:{cauldron_liquid:\"water\",fill_level:6},version:1}",
        "{Name: \"minecraft:water_cauldron\"}");

    /* The reference answers every Bedrock wire the same — four side
       connections and power 0 — and so does the map. */
    check_translate(arena,
        "{name:\"minecraft:redstone_wire\","
        "states:{north:\"side\",south:\"side\",powered:1b},version:1}",
        "{Name: \"minecraft:redstone_wire\", "
        "Properties: {east: \"side\", north: \"side\", power: \"0\", "
        "south: \"side\", west: \"side\"}}");

    /* States the map does not know take the Java default — the
       reference's own answer — and a legacy entry has no Java
       spelling at all. */
    check_translate(arena,
        "{name:\"minecraft:torch\","
        "states:{torch_facing_direction:1},version:1}",
        "{Name: \"minecraft:torch\"}");
    check_translate(arena,
        "{name:\"minecraft:farmland\",states:{moisture:7},version:1}",
        "{Name: \"minecraft:farmland\"}");
    check_translate(arena,
        "{name:\"minecraft:air\",states:{},version:1}",
        "{Name: \"minecraft:air\"}");
    check_translate(arena, "{name:\"minecraft:log\",val:4}", NULL);
    check_translate(arena, "{states:{},version:1}", NULL);
    check_translate(arena, "\"minecraft:stone\"", NULL);

    mcme_clear_error();
    mcme_arena_free(arena);
}

/* ========================================================================
 * Convert — a Bedrock world, rewritten as Java
 * ======================================================================== */

/* The output chunk's tree, read back through the region layer. */
static McmeTag *convert_chunk_root(McmeArena *arena, const char *out,
                                   int32_t dim, int cx, int cz,
                                   McmeNbtDoc *doc)
{
    char file[512];
    char dimpath[16] = "";
    if (dim != 0) {
        const char *dd = NULL;
        if (mcme_java_dimension_dir(dim, &dd)) {
            snprintf(dimpath, sizeof dimpath, "%s/", dd);
        }
    }
    snprintf(file, sizeof file, "%s/%sregion/r.%d.%d.mca", out,
             dimpath, cx >> 5, cz >> 5);
    McmeRegion reg;
    if (!mcme_region_open(&reg, file)) return NULL;
    McmeBuffer nbt;
    mcme_buffer_init(&nbt);
    bool ok = mcme_region_chunk_present(&reg, cx, cz)
        && mcme_region_read_chunk(&reg, cx, cz, NULL, &nbt);
    mcme_region_close(&reg);
    if (!ok) {
        mcme_buffer_free(&nbt);
        return NULL;
    }
    bool parsed = mcme_nbt_parse(doc, arena, nbt.data, nbt.len, false);
    mcme_buffer_free(&nbt);
    return parsed ? doc->root : NULL;
}

/* One fabricated tick record: SNBT in, little-endian bytes out, the
   chunk's own key in its own dimension (D21 — 13 bytes where the
   dimension speaks). The fixture worlds ship no tick records —
   real ones are everywhere (the census counted 2,235), so D18's test
   writes them. */
static bool put_tick(McmeArena *arena, McmeLevelDb *db, int32_t cx,
                     int32_t cz, int32_t dim, uint8_t tag,
                     const char *snbt)
{
    uint8_t key[13];
    size_t klen = 0;
    McmeTag *root = NULL;
    McmeBuffer bin;
    mcme_buffer_init(&bin);
    bool ok = db != NULL
        && mcme_bedrock_key_build(key, sizeof key, &klen, cx, cz, dim,
                                  tag, false, 0)
        && mcme_nbt_parse_snbt(arena, snbt, strlen(snbt), &root)
        && root != NULL;
    if (ok) {
        McmeNbtDoc doc;
        doc.arena = arena;
        doc.root = root;
        doc.root_name.data = NULL;
        doc.root_name.len = 0;
        doc.little_endian = true;
        ok = mcme_nbt_write_binary_as(&doc, &bin, true)
            && mcme_leveldb_put(db, key, klen, bin.data, bin.len);
    }
    mcme_buffer_free(&bin);
    return ok;
}

/* One carried tick entry: the fields Java spells, i through z. */
static void check_tick(const McmeTag *list, size_t i, const char *name,
                       int32_t t, int32_t x, int32_t y, int32_t z)
{
    if (list == NULL || i >= list->list.len) {
        CHECK_MSG(false, "tick list holds no entry %zu", i);
        return;
    }
    const McmeTag *e = &list->list.items[i];
    CHECK(e->type == MCME_TAG_Compound);
    if (e->type != MCME_TAG_Compound) return;
    const McmeTag *nm = mcme_nbt_get(e, "i");
    CHECK(nm != NULL && nm->type == MCME_TAG_String
          && strcmp(nm->string.data, name) == 0);
    CHECK(mcme_nbt_int_or(mcme_nbt_get(e, "p"), INT32_MIN) == 0);
    CHECK(mcme_nbt_int_or(mcme_nbt_get(e, "t"), INT32_MIN) == t);
    CHECK(mcme_nbt_int_or(mcme_nbt_get(e, "x"), INT32_MIN) == x);
    CHECK(mcme_nbt_int_or(mcme_nbt_get(e, "y"), INT32_MIN) == y);
    CHECK(mcme_nbt_int_or(mcme_nbt_get(e, "z"), INT32_MIN) == z);
}

/* One fabricated Data3D record: bytes in, the chunk's own 9-byte key.
   The fixture worlds ship none — a real store's copy holds 36,848 —
   so D19's test writes them, as binary rather than SNBT because the
   record is not NBT: a heightmap, then the arrays, then padding. */
static bool put_biome(McmeLevelDb *db, int32_t cx, int32_t cz,
                      int32_t dim, const void *payload, size_t len)
{
    uint8_t key[13];   /* the record's own dimension's key (D21)   */
    size_t klen = 0;
    return mcme_bedrock_key_build(key, sizeof key, &klen, cx, cz, dim,
                                  MCME_BEDROCK_TAG_BIOME_3D, false, 0)
        && mcme_leveldb_put(db, key, klen, payload, len);
}

/* The section with this Y in the chunk's sections list, or NULL. */
static McmeTag *java_section(const McmeTag *root, int8_t y)
{
    const McmeTag *secs = root == NULL ? NULL
                                       : mcme_nbt_get(root, "sections");
    if (secs == NULL || secs->type != MCME_TAG_List) return NULL;
    for (size_t i = 0; i < secs->list.len; ++i) {
        const McmeTag *s = &secs->list.items[i];
        if (s->type != MCME_TAG_Compound) continue;
        const McmeTag *yt = mcme_nbt_get(s, "Y");
        if (yt != NULL && mcme_nbt_int_or(yt, 999) == y) {
            return (McmeTag *)s;
        }
    }
    return NULL;
}

/* The section's biomes container held to its content: the palette's
   names in order, and the packed data — absent, as the format's
   one-name shorthand, when the caller passes none. */
/* The converted chunk's heightmap, held to the fabricated record's
   columns: the packing pinned by hand — 7 values to a long, 9 bits
   to a value, none spanning two, 37 longs — and by formula, column i
   reading 100 + i under MOTION_BLOCKING (D20). */
/* The record's map as the chunk packs it: column i reads base + i,
   nine bits apart, seven to the long, none straddling — and the
   literals pin the packing itself, whatever the base. The overworld's
   own records spell 100 + i; the nether's, whose world starts at 0,
   spells i (D21). */
static void check_heightmap(const McmeTag *root, int32_t base)
{
    const McmeTag *hm = root == NULL ? NULL
                                     : mcme_nbt_get(root, "Heightmaps");
    CHECK_MSG(hm != NULL, "the chunk holds no Heightmaps");
    if (hm == NULL || hm->type != MCME_TAG_Compound) return;
    const McmeTag *mb = mcme_nbt_get(hm, "MOTION_BLOCKING");
    CHECK_MSG(mb != NULL, "Heightmaps holds no MOTION_BLOCKING");
    if (mb == NULL || mb->type != MCME_TAG_Long_Array) return;
    CHECK_MSG(mb->longs.len == 37,
              "MOTION_BLOCKING spells %zu longs, want 37",
              mb->longs.len);
    if (mb->longs.len != 37) return;
    const int64_t *v = mb->longs.data;
    /* word 0: columns 0..6, nine bits apart. */
    CHECK(v[0] == (int64_t)((uint64_t)base
                            | (uint64_t)(base + 1) << 9
                            | (uint64_t)(base + 2) << 18
                            | (uint64_t)(base + 3) << 27
                            | (uint64_t)(base + 4) << 36
                            | (uint64_t)(base + 5) << 45
                            | (uint64_t)(base + 6) << 54));
    /* word 1 starts at column 7 — the wrap is the seven-per-long. */
    CHECK(v[1] == (int64_t)((uint64_t)(base + 7)
                            | (uint64_t)(base + 8) << 9
                            | (uint64_t)(base + 9) << 18
                            | (uint64_t)(base + 10) << 27
                            | (uint64_t)(base + 11) << 36
                            | (uint64_t)(base + 12) << 45
                            | (uint64_t)(base + 13) << 54));
    /* word 36 holds the last four columns alone. */
    CHECK(v[36] == (int64_t)((uint64_t)(base + 252)
                             | (uint64_t)(base + 253) << 9
                             | (uint64_t)(base + 254) << 18
                             | (uint64_t)(base + 255) << 27));
    for (size_t w = 0; w < 37; ++w) {
        for (size_t j = 0; j < 7; ++j) {
            size_t p = w * 7 + j;
            if (p >= 256) break;
            CHECK_MSG(((uint64_t)v[w] >> (j * 9) & 511)
                          == (uint64_t)(base + (int32_t)p),
                      "column %zu of long %zu reads %llu, want %zu",
                      j, w,
                      (unsigned long long)((uint64_t)v[w] >> (j * 9)
                                           & 511),
                      (size_t)(base + (int32_t)p));
        }
    }
}

static void check_biomes(const McmeTag *root, int8_t y,
                         const char *const *names, size_t nnames,
                         const int64_t *data, size_t ndata)
{
    McmeTag *sec = java_section(root, y);
    CHECK_MSG(sec != NULL, "the chunk has no section at Y %d", (int)y);
    if (sec == NULL) return;
    const McmeTag *bio = mcme_nbt_get(sec, "biomes");
    CHECK_MSG(bio != NULL, "the section at Y %d has no biomes", (int)y);
    if (bio == NULL || bio->type != MCME_TAG_Compound) return;
    const McmeTag *pal = mcme_nbt_get(bio, "palette");
    if (pal == NULL || pal->type != MCME_TAG_List) {
        CHECK_MSG(false, "the section at Y %d has no biome palette",
                  (int)y);
        return;
    }
    CHECK_MSG(pal->list.len == nnames,
              "the section at Y %d spells a palette of %zu, want %zu",
              (int)y, pal->list.len, nnames);
    if (pal->list.len != nnames) return;
    for (size_t i = 0; i < nnames; ++i) {
        const McmeTag *s = &pal->list.items[i];
        CHECK_MSG(s->type == MCME_TAG_String
                  && strcmp(s->string.data, names[i]) == 0,
                  "the section at Y %d palette[%zu] is %s, want %s",
                  (int)y, i, s->type == MCME_TAG_String
                             ? s->string.data : "(not a string)",
                  names[i]);
    }
    const McmeTag *d = mcme_nbt_get(bio, "data");
    if (ndata == 0) {
        CHECK_MSG(d == NULL,
                  "the section at Y %d packs data its one-name "
                  "palette does not need", (int)y);
        return;
    }
    if (d == NULL || d->type != MCME_TAG_Long_Array) {
        CHECK_MSG(false, "the section at Y %d has no packed data",
                  (int)y);
        return;
    }
    CHECK_MSG(d->longs.len == ndata,
              "the section at Y %d packs %zu longs, want %zu",
              (int)y, d->longs.len, ndata);
    if (d->longs.len != ndata) return;
    for (size_t i = 0; i < ndata; ++i) {
        CHECK_MSG(d->longs.data[i] == data[i],
                  "the section at Y %d long %zu is %lld, want %lld",
                  (int)y, i, (long long)d->longs.data[i],
                  (long long)data[i]);
    }
}

static void test_convert(void)
{
    printf("  convert (bedrock -> java/1.20)\n");
    char src[512];
    snprintf(src, sizeof src, "%s/db", kChunksWorld);
    const char *lvl_src = "tests/golden/worlds/bedrock/level.dat";
    if (!mcme_fs_exists(src) || !mcme_fs_exists(lvl_src)) {
        printf("  SKIP convert (%s or %s not found)\n", src, lvl_src);
        return;
    }

    /* The fixture chunks world ships without a level.dat, so the
       fixture world's own level.dat is carried onto the copy: both
       are fixtures, both copied, neither written. */
    char in[64], out[96], dbdir[512], lvl[512];
    CHECK(fresh_store_dir(in, sizeof in));
    snprintf(dbdir, sizeof dbdir, "%s/db", in);
    CHECK(copy_store(src, dbdir));
    void *raw = NULL;
    size_t raw_len = 0;
    CHECK(mcme_fs_read(lvl_src, &raw, &raw_len));
    snprintf(lvl, sizeof lvl, "%s/level.dat", in);
    CHECK(mcme_fs_write(lvl, raw, raw_len));
    free(raw);

    /* Four tick records and four Data3D records, fabricated on the
       copy — the fixture ships none of either. The ticks: a
       scheduled record whose entries go through the same map the
       palettes use and split Java's two lists by content (with one
       entry missing its z, which must drop whole), a random-tick
       record beside it, an orphan whose chunk holds no subchunks
       (its entries count, never carry), and a payload that is not
       NBT (the record counts once). The biome records: chunk (0,0)'s
       covers six heights from the floor — the id-3 rename in the
       single form, two strata with no subchunk to stand in (their
       cells count, never carry), the uniform id 1 under them, a
       mixed array whose third id the map does not know (the cell
       holding it becomes the reference's plains fallback and
       counts), and the uniform id 6 on top — ending in the game's
       0xFF tail; chunk (1,-2)'s map-only record (no arrays, and one
       column past the world's top, so its map drops and counts while
       the chunk keeps no invented containers); an
       orphan's two arrays (counted, never carried); and a payload
       that is not a record (it counts once). */
    {
        McmeArena *ta = mcme_arena_new();
        McmeLevelDb *tdb = NULL;
        CHECK(ta != NULL);
        CHECK(ta != NULL && mcme_leveldb_open_rw(&tdb, dbdir));
        if (ta != NULL && tdb != NULL) {
            CHECK(put_tick(ta, tdb, 0, 0, 0,
                MCME_BEDROCK_TAG_PENDING_TICKS,
                "{currentTick:100,tickList:["
                "{blockState:{name:\"minecraft:flowing_water\","
                "states:{liquid_depth:3},version:17959425},"
                "time:100L,x:5,y:65,z:5},"
                "{blockState:{name:\"minecraft:sand\","
                "states:{sand_type:\"red\"},version:17959425},"
                "time:95L,x:6,y:64,z:6},"
                "{blockState:{name:\"minecraft:stone\","
                "states:{},version:17959425},time:100L,x:7,y:64}]}"));
            CHECK(put_tick(ta, tdb, 0, 0, 0,
                MCME_BEDROCK_TAG_RANDOM_TICKS,
                "{currentTick:100,tickList:["
                "{blockState:{name:\"minecraft:magma\",states:{},"
                "version:17959425},time:120L,x:7,y:64,z:7}]}"));
            CHECK(put_tick(ta, tdb, 5, 5, 0,
                MCME_BEDROCK_TAG_PENDING_TICKS,
                "{currentTick:50,tickList:["
                "{blockState:{name:\"minecraft:gravel\",states:{},"
                "version:17959425},time:60L,x:81,y:64,z:81},"
                "{blockState:{name:\"minecraft:sand\",states:{},"
                "version:17959425},time:61L,x:82,y:64,z:82}]}"));
            uint8_t gkey[9];
            size_t gklen = 0;
            CHECK(mcme_bedrock_key_build(gkey, sizeof gkey, &gklen,
                                         6, 6, 0,
                                         MCME_BEDROCK_TAG_PENDING_TICKS,
                                         false, 0));
            CHECK(mcme_leveldb_put(tdb, gkey, gklen,
                                   "not NBT, only bytes", 19));

            /* The Data3D records: a heightmap whose columns spell
               their own coordinates — column i (i = z*16 + x) reads
               100 + i, inside the world's height — so any slip in
               the record's order or Java's packing shows as a wrong
               number; then the arrays byte by byte. */
            static uint8_t bp[512 + 25 + 1041 + 5 + 15];
            memset(bp, 0, sizeof bp);
            for (size_t i = 0; i < 256; ++i) {
                int32_t v = 100 + (int32_t)i;
                bp[i * 2] = (uint8_t)(v & 0xff);
                bp[i * 2 + 1] = (uint8_t)(v >> 8);
            }
            size_t q = 512;
            bp[q] = 0x00; bp[q + 1] = 3; q += 5;   /* cy -4: id 3   */
            bp[q] = 0x00; bp[q + 1] = 1; q += 5;   /* cy -3: id 1   */
            bp[q] = 0x00; bp[q + 1] = 1; q += 5;   /* cy -2: id 1   */
            bp[q] = 0x00; bp[q + 1] = 1; q += 5;   /* cy -1: id 1   */
            bp[q] = 0x05;                          /* cy  0: width 2 */
            size_t w = q + 1;
            bp[w + 4 * 0 + 1] = 0x02;   /* word 0,  bits 8: -> id 194 */
            bp[w + 4 * 4] = 0x01;       /* word 4,  bits 0: -> id 6   */
            bp[w + 4 * 64] = 0x01;      /* word 64, bits 0: -> id 6   */
            size_t pl = w + 1024;
            bp[pl] = 3;                 /* palette: 1, 6, 194         */
            bp[pl + 4] = 1;
            bp[pl + 8] = 6;
            bp[pl + 12] = (uint8_t)194;
            q = pl + 16;
            bp[q] = 0x00; bp[q + 1] = 6; q += 5;   /* cy  1: id 6   */
            memset(bp + q, 0xff, 15); q += 15;     /* the game's tail */
            CHECK(put_biome(tdb, 0, 0, 0, bp, q));

            /* The map the world's height cannot spell: chunk (1,-2)
               holds subchunks but no record, and this record holds
               no arrays (a record may hold none) and one column past
               the world's top — 400 above the floor, where the
               overworld's 384 ends. The map drops and counts; the
               chunk's containers keep the shape of no biome data
               (D20). */
            static uint8_t mp[512];
            for (size_t i = 0; i < 256; ++i) {
                int32_t v = 100 + (int32_t)i;
                mp[i * 2] = (uint8_t)(v & 0xff);
                mp[i * 2 + 1] = (uint8_t)(v >> 8);
            }
            mp[(3 * 16 + 5) * 2] = (uint8_t)(400 & 0xff);
            mp[(3 * 16 + 5) * 2 + 1] = (uint8_t)(400 >> 8);
            CHECK(put_biome(tdb, 1, -2, 0, mp, sizeof mp));

            /* The orphan: two arrays for a chunk with no subchunks.
               */
            static uint8_t op[512 + 10];
            memset(op, 0, sizeof op);
            op[512] = 0x00; op[513] = 1;
            op[517] = 0x00; op[518] = 6;
            CHECK(put_biome(tdb, 5, 5, 0, op, sizeof op));

            /* A payload that is not a record: 24 bytes, no
               heightmap to even begin with. */
            CHECK(put_biome(tdb, 6, 6, 0, "not a record, only bytes",
                            sizeof "not a record, only bytes" - 1));

            /* D21's own half of the store: the fixture's nether key
               at (0,0) now converts, and beside it two fabricated
               nether chunks, one end chunk, and the keys with
               nowhere to go. The nether record at (0,0): columns
               0..255 sit inside the nether's 256 and cross, its
               floor stratum stands in the section at Y0, and the one
               above stands where no section exists (counted, never
               carried). */
            static uint8_t np[512 + 10 + 15];
            memset(np, 0, sizeof np);
            for (size_t i = 0; i < 256; ++i) {
                np[i * 2] = (uint8_t)i;   /* column i reads i       */
            }
            np[512] = 0x00; np[513] = 1;  /* cy 0: id 1             */
            np[517] = 0x00; np[518] = 1;  /* cy 1: id 1             */
            memset(np + 522, 0xff, 15);   /* the game's own tail    */
            CHECK(put_biome(tdb, 0, 0, 1, np, sizeof np));

            /* Two more subchunks from the fixture's own nether
               payload: the nether's chunk at (1,-2) and the end's at
               the same coordinates - the sort must hold three
               dimensions apart even where their chunks share a
               place. */
            uint8_t skey[14];
            size_t slen = 0;
            McmeBuffer got;
            mcme_buffer_init(&got);
            bool was_del = false;
            CHECK(mcme_bedrock_key_build(skey, sizeof skey, &slen,
                                         0, 0, 1, 0x2f, true, 0));
            CHECK(mcme_leveldb_get(tdb, skey, slen, &got, &was_del));
            CHECK(!was_del && got.len > 0);
            if (got.len > 0) {
                CHECK(mcme_bedrock_key_build(skey, sizeof skey, &slen,
                                             1, -2, 1, 0x2f, true, 0));
                CHECK(mcme_leveldb_put(tdb, skey, slen, got.data,
                                       got.len));
                CHECK(mcme_bedrock_key_build(skey, sizeof skey, &slen,
                                             1, -2, 2, 0x2f, true, 0));
                CHECK(mcme_leveldb_put(tdb, skey, slen, got.data,
                                       got.len));
            }
            mcme_buffer_free(&got);

            /* The nether record whose one column 300 fits the
               overworld's 384 and not the nether's 256 (D21's own
               height): the map drops, its floor stratum's cells
               still cross beside it (D20's halves, per dimension). */
            static uint8_t rp[512 + 5];
            memset(rp, 0, sizeof rp);
            for (size_t i = 0; i < 256; ++i) {
                rp[i * 2] = (uint8_t)i;
            }
            rp[(3 * 16 + 5) * 2] = (uint8_t)(300 & 0xff);
            rp[(3 * 16 + 5) * 2 + 1] = (uint8_t)(300 >> 8);
            rp[512] = 0x00; rp[513] = 1;  /* cy 0: id 1             */
            CHECK(put_biome(tdb, 1, -2, 1, rp, sizeof rp));

            /* The nether orphan at (7,7): a record whose chunk holds
               no subchunks - its cells and its map count in their
               own dimension's half of the report. */
            static uint8_t nop[512 + 5];
            memset(nop, 0, sizeof nop);
            for (size_t i = 0; i < 256; ++i) {
                nop[i * 2] = (uint8_t)i;
            }
            nop[512] = 0x00; nop[513] = 1;
            CHECK(put_biome(tdb, 7, 7, 1, nop, sizeof nop));

            /* The nether's tick records: one on the chunk that
               converts (carried, its entries its own - the
               overworld's records at the same coordinates must
               never reach it), one orphan whose entries count. */
            CHECK(put_tick(ta, tdb, 0, 0, 1,
                MCME_BEDROCK_TAG_PENDING_TICKS,
                "{currentTick:50,tickList:["
                "{blockState:{name:\"minecraft:magma\",states:{},"
                "version:17959425},time:110L,x:11,y:64,z:12}]}"));
            CHECK(put_tick(ta, tdb, 6, 6, 1,
                MCME_BEDROCK_TAG_PENDING_TICKS,
                "{currentTick:20,tickList:["
                "{blockState:{name:\"minecraft:stone\",states:{},"
                "version:17959425},time:30L,x:101,y:64,z:101}]}"));

            /* The keys with nowhere to go, by the rules their own
               dimensions answer to (D21): a number beyond the
               game's three counts `other`; its chunk version key
               counts `chunk_data`. */
            uint8_t jkey[19];   /* the longest key here: an actor */
            size_t jl = 0;
            CHECK(mcme_bedrock_key_build(jkey, sizeof jkey, &jl,
                                         99, 99, 99, 0x2f, true, 0));
            CHECK(mcme_leveldb_put(tdb, jkey, jl, "nowhere", 7));
            /* The nether's entity list of bytes no document
               follows: read since D23, and a record that will not
               decode counts once — the same number it made as an
               unread key, now for the reason the record gives. */
            CHECK(mcme_bedrock_key_build(jkey, sizeof jkey, &jl,
                                         0, 0, 1, 0x32, false, 0));
            CHECK(mcme_leveldb_put(tdb, jkey, jl, "entities", 8));
            CHECK(mcme_bedrock_key_build(jkey, sizeof jkey, &jl,
                                         0, 0, 1, 0x36, false, 0));
            CHECK(mcme_leveldb_put(tdb, jkey, jl, "chunkver", 8));

            /* The lists themselves (D23): two documents under a
               chunk no subchunk ever names — nothing crosses for
               entities, so documents count wherever the record
               sits — and a record holding nothing, whose count is
               zero: empty is not one. */
            static const char cowsnbt[] =
                "{identifier: \"minecraft:cow\", "
                "Pos: [0.0f, 64.0f, 0.0f]}";
            static const char pigsnbt[] =
                "{identifier: \"minecraft:pig\", "
                "Pos: [1.0f, 64.0f, 1.0f]}";
            McmeTag *cow = NULL;
            McmeTag *pig = NULL;
            CHECK(mcme_nbt_parse_snbt(ta, cowsnbt, strlen(cowsnbt),
                                      &cow));
            CHECK(mcme_nbt_parse_snbt(ta, pigsnbt, strlen(pigsnbt),
                                      &pig));
            CHECK(cow != NULL && pig != NULL);
            McmeNbtDoc d1 = { .arena = ta, .root = cow,
                              .root_name = {NULL, 0},
                              .little_endian = true };
            McmeNbtDoc d2 = { .arena = ta, .root = pig,
                              .root_name = {NULL, 0},
                              .little_endian = true };
            McmeBuffer entpay;
            mcme_buffer_init(&entpay);
            CHECK(mcme_nbt_write_binary(&d1, &entpay));
            CHECK(mcme_nbt_write_binary(&d2, &entpay));
            CHECK(mcme_bedrock_key_build(jkey, sizeof jkey, &jl,
                                         7, 7, 0, 0x32, false, 0));
            CHECK(mcme_leveldb_put(tdb, jkey, jl, entpay.data,
                                   entpay.len));
            CHECK(mcme_bedrock_key_build(jkey, sizeof jkey, &jl,
                                         8, 8, 0, 0x32, false, 0));
            CHECK(mcme_leveldb_put(tdb, jkey, jl, "", 0));
            mcme_buffer_free(&entpay);

            /* The modern spelling beside it (D23): one actor
               record and the digp index that names it — the record
               counts where it stands, the index nowhere (its
               document IS the record; counting both would count
               one entity twice), and neither may reach `other`. */
            static const uint8_t actor_id[8] =
                {1, 2, 3, 4, 5, 6, 7, 8};
            CHECK(mcme_bedrock_actor_key_build(jkey, sizeof jkey, &jl,
                                               actor_id));
            CHECK(mcme_leveldb_put(tdb, jkey, jl, "actor", 5));
            CHECK(mcme_bedrock_digp_key_build(jkey, sizeof jkey, &jl,
                                              7, 7, 0));
            CHECK(mcme_leveldb_put(tdb, jkey, jl, actor_id, 8));
            mcme_leveldb_close(tdb);

            /* The nether's two block-entity documents, put through
               the write API in dimension 1: one whose chunk
               converts (carried into the nether's own list), one
               whose chunk holds no subchunks (counted). The
               overworld's lists at the same coordinates must stay
               empty. */
            McmeTag *chest = NULL;
            McmeTag *away = NULL;
            McmeWriteRefusal beref = MCME_WRITE_REFUSAL_NONE;
            CHECK(mcme_nbt_parse_snbt(ta, "{id: \"minecraft:chest\"}",
                                      strlen("{id: \"minecraft:chest\"}"),
                                      &chest));
            CHECK(mcme_nbt_parse_snbt(ta, "{id: \"minecraft:chest\"}",
                                      strlen("{id: \"minecraft:chest\"}"),
                                      &away));
            CHECK(chest != NULL && away != NULL);
            CHECK(mcme_bedrock_put_block_entity(dbdir, 3, 70, 4, 1,
                                                chest, ta, &beref));
            CHECK(beref == MCME_WRITE_REFUSAL_NONE);
            CHECK(mcme_bedrock_put_block_entity(dbdir, 99, 70, 100, 1,
                                                away, ta, &beref));
            CHECK(beref == MCME_WRITE_REFUSAL_NONE);
        }
        mcme_arena_free(ta);
    }

    snprintf(out, sizeof out, "%s_java", in);
    CHECK(!mcme_fs_exists(out));

    /* The fixture's own shape, counted: the two overworld chunks,
       the fixture's nether key's chunk beside them (D21 - that key
       counted `other` before the drop), and the nether and end
       chunks fabricated beside it; the v1 legacy subchunk dropped;
       the second layer's water over stone, dirt and grass - none of
       them blocks Java gives a waterlogged property - counted
       position by position (3280); the key of a dimension number
       beyond the game's three among the keys with nowhere to go. */
    McmeConvertReport rep;
    McmeWriteRefusal refusal = MCME_WRITE_REFUSAL_NONE;
    if (!mcme_convert_bedrock_java(in, out, &rep, &refusal)) {
        CHECK_MSG(false, "convert failed: %s", mcme_last_error());
        remove_store(dbdir);
        remove(lvl);
        rmdir(in);
        return;
    }
    CHECK(refusal == MCME_WRITE_REFUSAL_NONE);
    CHECK(rep.chunks == 5);
    CHECK(rep.subchunks == 10);
    CHECK(rep.blocks == 36864);
    CHECK(rep.legacy_subchunks == 1);
    CHECK(rep.water_positions == 3280);
    CHECK(rep.other == 1);
    CHECK(rep.block_entities == 1);    /* the nether orphan        */
    CHECK(rep.block_entities_carried == 1);
    CHECK(rep.items == 0);
    /* The entity lists, read since D23: the nether record that
       will not decode counts once, the two documents of the record
       at (7,7) count two, the record at (8,8) holds nothing and
       counts zero — and the actor record counts where it stands
       while the digp index beside it counts nowhere, both out of
       `other`. */
    CHECK_MSG(rep.entities == 3,
              "entities is %zu, want 3", rep.entities);
    CHECK(rep.actors == 1);          /* the fabricated actor       */
    CHECK(rep.chunk_data == 1);        /* its chunk version key    */
    /* The fabricated records, counted their own way: four entries
       carried - the overworld's three, the nether chunk's own
       beside them - and five dropped - the entry with no z (1), the
       overworld orphan's two entries (2), the record that is not
       NBT (1, a record counts once), the nether orphan's entry (1)
       read from its own dimension's key. */
    CHECK(rep.ticks_carried == 4);
    CHECK(rep.ticks == 5);
    /* The fabricated biome records, counted their own way: the
       overworld's four sections minus its fallback cell - 255 -
       plus the nether floor strata the sections of their own
       dimension took (2 x 64) - 383 - and the drops: the two
       strata with no section (128), the overworld orphan's arrays
       (128), the record that is not a record (1, once), the
       fallback cell (1), the nether stratum above its floor with no
       section (64), and the nether orphan's array (64) - 386. */
    CHECK_MSG(rep.biomes_carried == 383,
              "biomes_carried is %zu, want 383", rep.biomes_carried);
    CHECK_MSG(rep.biomes == 386, "biomes is %zu, want 386",
              rep.biomes);

    /* The maps beside them (D20): (0,0)'s record spells every
       column inside the world's height and crosses; (1,-2)'s one
       column past the top drops its map; the orphan's and the
       unreadable record's maps are lost with the chunks they belong
       to. The nether's half (D21): its record's columns 0..255 fit
       the nether's own 256 and cross, the second record's one
       column at 300 - inside the overworld's 384, past the nether's
       256 - drops, the nether orphan's map counts, and the end's
       chunk, which holds no record, counts its own. */
    CHECK_MSG(rep.heightmaps_carried == 2,
              "heightmaps_carried is %zu, want 2",
              rep.heightmaps_carried);
    CHECK_MSG(rep.heightmaps == 6, "heightmaps is %zu, want 6",
              rep.heightmaps);

    /* The output is a whole world: the scaffold, and the probe's
       verdict on it. */
    char file[512];
    snprintf(file, sizeof file, "%s/region/r.0.0.mca", out);
    CHECK(mcme_fs_exists(file));
    snprintf(file, sizeof file, "%s/region/r.0.-1.mca", out);
    CHECK(mcme_fs_exists(file));
    /* Each dimension arrives where Java reads it: the nether in
       DIM-1, the end in DIM1 - the game's own layout, per
       dimension (D21). */
    snprintf(file, sizeof file, "%s/DIM-1/region/r.0.0.mca", out);
    CHECK(mcme_fs_exists(file));
    snprintf(file, sizeof file, "%s/DIM-1/region/r.0.-1.mca", out);
    CHECK(mcme_fs_exists(file));
    snprintf(file, sizeof file, "%s/DIM1/region/r.0.-1.mca", out);
    CHECK(mcme_fs_exists(file));
    snprintf(file, sizeof file, "%s/level.dat", out);
    CHECK(mcme_fs_exists(file));
    McmeWorldInfo info;
    CHECK(mcme_world_probe(&info, out));
    CHECK(info.kind == MCME_WORLD_JAVA);
    CHECK(info.data_version == 3463);
    CHECK(info.chunks == 2);
    mcme_world_info_free(&info);

    McmeArena *arena = mcme_arena_new();
    CHECK(arena != NULL);
    if (arena != NULL) {
        /* The level.dat carries the name and the seed across. */
        McmeLevelDatFrame frame;
        char out_lvl[512];
        snprintf(out_lvl, sizeof out_lvl, "%s/level.dat", out);
        McmeNbtDoc lvldoc, indoc;
        CHECK(mcme_level_dat_parse(&lvldoc, arena, out_lvl, &frame));
        CHECK(frame == MCME_LEVELDAT_GZIP);
        CHECK(mcme_level_dat_parse(&indoc, arena, lvl, &frame));
        const McmeTag *data = mcme_nbt_get(lvldoc.root, "Data");
        CHECK(data != NULL);
        CHECK(mcme_nbt_int_or(mcme_nbt_get(data, "DataVersion"), 0)
              == 3463);
        const McmeTag *scope = mcme_nbt_get(indoc.root, "Data");
        if (scope == NULL || scope->type != MCME_TAG_Compound) {
            scope = indoc.root;
        }
        const McmeTag *wgs = mcme_nbt_get(data, "WorldGenSettings");
        CHECK(wgs != NULL);
        /* The fixture carries no RandomSeed; both sides then read
           0, and a missing seed on either side fails the pair. */
        CHECK(mcme_nbt_long_or(mcme_nbt_get(wgs, "seed"), -1)
              == mcme_nbt_long_or(mcme_nbt_get(scope, "RandomSeed"),
                                  0));
        const McmeTag *name_in = mcme_nbt_get(scope, "LevelName");
        const McmeTag *name_out = mcme_nbt_get(data, "LevelName");
        CHECK(name_in != NULL && name_out != NULL
              && name_in->type == MCME_TAG_String
              && name_out->type == MCME_TAG_String
              && name_in->string.len == name_out->string.len
              && memcmp(name_in->string.data, name_out->string.data,
                        name_in->string.len) == 0);
        /* The spawn travels under the Java field names the probe
           reads; the fixture records none, so the documented
           defaults stand. */
        CHECK(mcme_nbt_int_or(mcme_nbt_get(data, "SpawnX"), -1)
              == mcme_nbt_int_or(mcme_nbt_get(scope, "SpawnX"), 0));
        CHECK(mcme_nbt_int_or(mcme_nbt_get(data, "SpawnY"), -1)
              == mcme_nbt_int_or(mcme_nbt_get(scope, "SpawnY"), 64));
        CHECK(mcme_nbt_int_or(mcme_nbt_get(data, "SpawnZ"), -1)
              == mcme_nbt_int_or(mcme_nbt_get(scope, "SpawnZ"), 0));

        /* The blocks, through the map, section by section. */
        McmeNbtDoc chunk;
        McmeTag *root = convert_chunk_root(arena, out, 0, 0, 0, &chunk);
        CHECK(root != NULL);
        if (root != NULL) {
            /* The overworld keeps its own floor (D21): yPos is the
               floor section, -4 for y -64. */
            CHECK(mcme_nbt_int_or(mcme_nbt_get(root, "yPos"), 99)
                  == -4);
            check_java_block(root, 0, 64, 0, "minecraft:stone");
            check_java_block(root, 0, 66, 0,
                "minecraft:grass_block{snowy: \"false\"}");
            check_java_block(root, 0, 67, 0,
                "minecraft:oak_log{axis: \"x\"}");
            check_java_block(root, 0, -60, 0,
                "minecraft:furnace{lit: 0b}");
            check_java_block(root, 0, -50, 0, "minecraft:red_sand");
            check_java_block(root, 0, 0, 0,
                "minecraft:redstone_wire{east: \"side\", "
                "north: \"side\", power: \"0\", south: \"side\", "
                "west: \"side\"}");
            check_java_block(root, 0, 10, 0, "minecraft:water");
            /* The v1 legacy subchunk's blocks: air, counted. */
            check_java_block(root, 0, 34, 0, "minecraft:air");

            /* The tick lists, both keys standing in this chunk: the
               water the map answered as `water` under fluid_ticks
               (t = time − currentTick = 0), the two blocks Java
               spells block ticks (t = −5 overdue, t = 20 pending).
               The entry with no z is nowhere — dropped whole. */
            const McmeTag *ft = mcme_nbt_get(root, "fluid_ticks");
            const McmeTag *bt = mcme_nbt_get(root, "block_ticks");
            CHECK(ft != NULL && ft->type == MCME_TAG_List
                  && ft->list.len == 1);
            CHECK(bt != NULL && bt->type == MCME_TAG_List
                  && bt->list.len == 2);
            check_tick(ft, 0, "minecraft:water", 0, 5, 65, 5);
            check_tick(bt, 0, "minecraft:red_sand", -5, 6, 64, 6);
            check_tick(bt, 1, "minecraft:magma_block", 20, 7, 64, 7);

            /* The biomes, section by section: id 3's rename in the
               one-name shorthand at the furnace floor, the uniform
               plain beside the empty strata, the mixed array
               packed (swamps at cells 1 and 4, the fallback cell
               16 reading plains like every other), the uniform
               swamp on top, and — the shape the reference writes —
               no key at all on the ground section, whose Y the
               fabricated record's range never reaches. */
            static const char *const hills[] =
                {"minecraft:windswept_hills"};
            check_biomes(root, -4, hills, 1, NULL, 0);
            static const char *const plains[] =
                {"minecraft:plains"};
            check_biomes(root, -1, plains, 1, NULL, 0);
            static const char *const mixed[] =
                {"minecraft:plains", "minecraft:swamp"};
            int64_t mixed_long[] = {18};   /* bits 1 and 4 set      */
            check_biomes(root, 0, mixed, 2, mixed_long, 1);
            static const char *const swamp[] =
                {"minecraft:swamp"};
            check_biomes(root, 1, swamp, 1, NULL, 0);
            McmeTag *ground = java_section(root, 4);
            CHECK(ground != NULL);
            CHECK(ground != NULL
                  && mcme_nbt_get(ground, "biomes") == NULL);
            /* And the record's other half stands beside the
               containers: the map, packed (D20). */
            check_heightmap(root, 100);
        }
        McmeNbtDoc chunk2;
        McmeTag *root2 = convert_chunk_root(arena, out, 0, 1, -2,
                                         &chunk2);
        CHECK(root2 != NULL);
        if (root2 != NULL) {
            check_java_block(root2, 16, 0, -17, "mcme:fixture_211");
            /* The chunk whose record spelled no map the world could
               hold gets no Heightmaps at all (D20), while both tick
               lists still stand — empty, never absent. */
            CHECK(mcme_nbt_get(root2, "Heightmaps") == NULL);
            const McmeTag *ft2 = mcme_nbt_get(root2, "fluid_ticks");
            const McmeTag *bt2 = mcme_nbt_get(root2, "block_ticks");
            CHECK(ft2 != NULL && ft2->type == MCME_TAG_List
                  && ft2->list.len == 0);
            CHECK(bt2 != NULL && bt2->type == MCME_TAG_List
                  && bt2->list.len == 0);
            /* And the chunk whose record held no arrays keeps no
               invented container — the reference's own shape for
               a store that held no biome data here. */
            McmeTag *s2 = java_section(root2, 0);
            CHECK(s2 != NULL);
            CHECK(s2 != NULL
                  && mcme_nbt_get(s2, "biomes") == NULL);
        }

        /* The nether's chunk at (0,0): its floor is the
           dimension's own - yPos 0, one section at Y0 where the
           store held one subchunk, its own map (columns 0..255,
           nine bits, seven per long), its own tick entry, its own
           block entity - and none of the overworld's data at the
           same coordinates, none of it reaching the overworld's
           lists (the sort and the pairing hold dimensions apart). */
        McmeNbtDoc nchunk;
        McmeTag *nroot = convert_chunk_root(arena, out, 1, 0, 0,
                                            &nchunk);
        CHECK(nroot != NULL);
        if (nroot != NULL) {
            CHECK(mcme_nbt_int_or(mcme_nbt_get(nroot, "yPos"), 99)
                  == 0);
            CHECK(java_section(nroot, 0) != NULL);
            CHECK(java_section(nroot, -1) == NULL);
            CHECK(java_section(nroot, 1) == NULL);
            /* The fixture's nether payload, parity: dirt where
               (x+y+z) is even, stone where odd - every cell. */
            check_java_block(nroot, 0, 0, 0, "minecraft:dirt");
            check_java_block(nroot, 1, 0, 0, "minecraft:stone");
            check_java_block(nroot, 0, 1, 0, "minecraft:stone");
            check_java_block(nroot, 15, 15, 15, "minecraft:stone");
            check_heightmap(nroot, 0);
            const McmeTag *nfluid = mcme_nbt_get(nroot, "fluid_ticks");
            const McmeTag *nblockt = mcme_nbt_get(nroot, "block_ticks");
            CHECK(nfluid != NULL && nfluid->type == MCME_TAG_List
                  && nfluid->list.len == 0);
            CHECK(nblockt != NULL && nblockt->type == MCME_TAG_List
                  && nblockt->list.len == 1);
            if (nblockt != NULL && nblockt->type == MCME_TAG_List
                    && nblockt->list.len == 1) {
                /* its own entry, from its own record (D21) */
                check_tick(nblockt, 0, "minecraft:magma_block", 60,
                           11, 64, 12);
            }
            const McmeTag *nbe = mcme_nbt_get(nroot, "block_entities");
            CHECK(nbe != NULL && nbe->type == MCME_TAG_List
                  && nbe->list.len == 1);
            /* The floor stratum's cells, plains like the id the map
               answers with; the record's next stratum stands where
               the nether holds no section. */
            static const char *const nplains[] = {"minecraft:plains"};
            check_biomes(nroot, 0, nplains, 1, NULL, 0);
        }

        /* The nether's other chunk: the refused map leaves no
           Heightmaps, while its cells still cross into the floor
           section (D20's halves, in the nether's own dimension). */
        McmeNbtDoc n12chunk;
        McmeTag *n12 = convert_chunk_root(arena, out, 1, 1, -2,
                                          &n12chunk);
        CHECK(n12 != NULL);
        if (n12 != NULL) {
            CHECK(mcme_nbt_int_or(mcme_nbt_get(n12, "yPos"), 99)
                  == 0);
            CHECK(mcme_nbt_get(n12, "Heightmaps") == NULL);
            static const char *const n12plains[] =
                {"minecraft:plains"};
            check_biomes(n12, 0, n12plains, 1, NULL, 0);
            check_java_block(n12, 0, 0, 0, "minecraft:dirt");
        }

        /* The end's chunk at the overworld's own coordinates: one
           dimension's answer, in DIM1, with no record's map and no
           invented containers. */
        McmeNbtDoc echunk;
        McmeTag *eroot = convert_chunk_root(arena, out, 2, 1, -2,
                                            &echunk);
        CHECK(eroot != NULL);
        if (eroot != NULL) {
            CHECK(mcme_nbt_int_or(mcme_nbt_get(eroot, "yPos"), 99)
                  == 0);
            CHECK(java_section(eroot, 0) != NULL);
            McmeTag *es = java_section(eroot, 0);
            CHECK(es != NULL
                  && mcme_nbt_get(es, "biomes") == NULL);
            CHECK(mcme_nbt_get(eroot, "Heightmaps") == NULL);
            check_java_block(eroot, 0, 0, 0, "minecraft:dirt");
        }
        mcme_arena_free(arena);
    }

    /* An output that exists is refused, whole; a path that is not a
       world is refused before anything is created. */
    CHECK(!mcme_convert_bedrock_java(in, out, &rep, &refusal));
    CHECK(refusal == MCME_WRITE_REFUSAL_USAGE);
    char nowhere[96];
    snprintf(nowhere, sizeof nowhere, "%s_missing", in);
    char nowhere_out[128];
    snprintf(nowhere_out, sizeof nowhere_out, "%s_missing", out);
    CHECK(!mcme_convert_bedrock_java(nowhere, nowhere_out, &rep,
                                     &refusal));
    CHECK(refusal == MCME_WRITE_REFUSAL_NOT_THERE);
    mcme_clear_error();

    char region_dir[512];
    snprintf(region_dir, sizeof region_dir, "%s/region", out);
    remove_store(region_dir);
    snprintf(region_dir, sizeof region_dir, "%s/DIM-1/region", out);
    remove_store(region_dir);
    snprintf(region_dir, sizeof region_dir, "%s/DIM-1", out);
    remove_store(region_dir);
    snprintf(region_dir, sizeof region_dir, "%s/DIM1/region", out);
    remove_store(region_dir);
    snprintf(region_dir, sizeof region_dir, "%s/DIM1", out);
    remove_store(region_dir);
    snprintf(file, sizeof file, "%s/level.dat", out);
    remove(file);
    rmdir(out);
    remove_store(dbdir);
    remove(lvl);
    rmdir(in);
}

/* A palette document for a payload this test builds: the block's name
   and its states, in the shape the translator reads. */
static McmeNbtDoc palette_doc(McmeArena *arena, const char *snbt)
{
    McmeNbtDoc doc;
    doc.arena = arena;
    doc.root = NULL;
    doc.root_name.data = (char *)"";
    doc.root_name.len = 0;
    doc.little_endian = true;
    McmeTag *tag = NULL;
    if (mcme_nbt_parse_snbt(arena, snbt, strlen(snbt), &tag)) {
        doc.root = tag;
    }
    return doc;
}

/* Every key of a store, bounded: a test's fixture hold is six keys,
   and a bounded answer is a test's own business. */
typedef struct {
    uint8_t keys[64][16];
    size_t  lens[64];
    size_t  n;
} KeyBag;

static bool take_key(void *ud, const McmeLevelEntry *e)
{
    KeyBag *b = (KeyBag *)ud;
    if (e->key_len <= 16 && b->n < 64) {
        memcpy(b->keys[b->n], e->key, e->key_len);
        b->lens[b->n] = e->key_len;
        b->n++;
    }
    return true;
}

/*
 * Convert against a world this test builds itself: one subchunk with
 * the waterlogging layer over blocks Java can and cannot waterlog,
 * one record holding the documents the map carries and the ones it
 * drops (decisions.md#D15). The input is written through mcme's own
 * writers — encode, put, the block-entity splice — so what convert
 * answers is pinned against the model.
 */
static void test_convert_carry(void)
{
    printf("  convert carry (waterlogging, block entities)\n");
    const char *lvl_src = "tests/golden/worlds/bedrock/level.dat";
    char esrc[512];
    snprintf(esrc, sizeof esrc, "%s/db", kEntitiesWorld);
    if (!mcme_fs_exists(lvl_src) || !mcme_fs_exists(esrc)) {
        printf("  SKIP convert carry (%s or %s not found)\n",
               lvl_src, esrc);
        return;
    }

    char in[64], out[96], dbdir[512], lvl[512];
    CHECK(fresh_store_dir(in, sizeof in));
    snprintf(dbdir, sizeof dbdir, "%s/db", in);
    /* A store to write into: mcme never creates one from nothing —
       CURRENT and a MANIFEST are the store's own bootstrap — so the
       smallest golden's db is copied in and emptied, every key
       tombstoned. What remains is a valid store holding nothing,
       which is what this world starts as. */
    CHECK(copy_store(esrc, dbdir));
    void *raw = NULL;
    size_t raw_len = 0;
    CHECK(mcme_fs_read(lvl_src, &raw, &raw_len));
    snprintf(lvl, sizeof lvl, "%s/level.dat", in);
    CHECK(mcme_fs_write(lvl, raw, raw_len));
    free(raw);

    McmeArena *arena = mcme_arena_new();
    CHECK(arena != NULL);
    if (arena == NULL) return;

    /* The subchunk: layer zero holds a stair, a stone block and a
       second stair the layer does not reach; layer one holds water
       over the stair (carried as the property) and over the stone
       (dropped and counted — stone has no such property). */
    uint16_t idx0[4096], idx1[4096];
    for (size_t i = 0; i < 4096; ++i) {
        idx0[i] = 2;    /* air */
        idx1[i] = 1;    /* air */
    }
    idx0[0] = 0;        /* the waterlogged stair, (0,0,0) */
    idx0[256] = 1;      /* the watered stone,     (1,0,0) */
    idx0[512] = 0;      /* the dry stair,         (2,0,0) */
    idx1[0] = 0;        /* water over the stair */
    idx1[256] = 0;      /* water over the stone */

    McmeNbtDoc pal0[3];
    /* A palette entry the map accepts needs its `version`: without it
       the entry is the pre-1.9 legacy spelling (decisions.md#D12) and
       has no Java answer. */
    pal0[0] = palette_doc(arena, "{name: \"minecraft:oak_stairs\", "
                                  "states: {}, version: 1L}");
    pal0[1] = palette_doc(arena, "{name: \"minecraft:stone\", "
                                  "states: {}, version: 1L}");
    pal0[2] = palette_doc(arena, "{name: \"minecraft:air\", "
                                  "states: {}, version: 1L}");
    McmeNbtDoc pal1[2];
    pal1[0] = palette_doc(arena, "{name: \"minecraft:water\", "
                                  "states: {}, version: 1L}");
    pal1[1] = palette_doc(arena, "{name: \"minecraft:air\", "
                                  "states: {}, version: 1L}");
    CHECK(pal0[0].root != NULL && pal0[1].root != NULL
          && pal0[2].root != NULL && pal1[0].root != NULL
          && pal1[1].root != NULL);

    McmeBedrockLayer layers[2];
    memset(layers, 0, sizeof layers);
    layers[0].bits = 2;
    layers[0].indices = idx0;
    layers[0].palette_len = 3;
    layers[0].palette = pal0;
    layers[1].bits = 1;
    layers[1].indices = idx1;
    layers[1].palette_len = 2;
    layers[1].palette = pal1;
    McmeBedrockSubchunk sc;
    memset(&sc, 0, sizeof sc);
    sc.version = 8;
    sc.layer_count = 2;
    sc.layers = layers;

    McmeBuffer payload;
    mcme_buffer_init(&payload);
    CHECK(mcme_bedrock_subchunk_encode(&sc, &payload));
    uint8_t key[16];
    size_t klen = 0;
    CHECK(mcme_bedrock_key_build(key, sizeof key, &klen, 3, 0, 0,
                                 0x2f, true, 0));
    KeyBag bag;
    memset(&bag, 0, sizeof bag);
    McmeLevelDb *db = NULL;
    CHECK(mcme_leveldb_open_rw(&db, dbdir));
    if (db != NULL) {
        mcme_leveldb_iterate(db, take_key, &bag);
        for (size_t i = 0; i < bag.n; ++i) {
            CHECK(mcme_leveldb_delete(db, bag.keys[i], bag.lens[i]));
        }
        CHECK(mcme_leveldb_put(db, key, klen, payload.data,
                               payload.len));
        mcme_leveldb_close(db);
    }
    mcme_buffer_free(&payload);

    /* The record, in both spellings of Bedrock's own, through the
       write API and as the game's bytes. Four of the five docs
       put through the write API carry namespaced ids, the furnace
       the bare savegame one — put takes both; the rest are the
       game's bytes — short savegame ids, Name/Count stacks — and
       all carry the same answer.
       Carried: both chests, the old-format sign (its text has no
       measured Java spelling, so it travels with both sides empty
       — D22), the furnace, the short-id chest, the short-id
       trapped chest, and the fourteen family documents appended
       below. Dropped and counted: the frame (no coordinates), the
       not-yet-filled loot chest, and the four with no Java
       counterpart (bed and banner keep their color in the
       document, the piston arm has no Java block entity, the
       vault's block Java 1.20 does not know). One stack inside
       the first chest carries an enchantment list, so that stack
       drops and the count says so — no half-translated item
       leaves. */
    McmeWriteRefusal refusal = MCME_WRITE_REFUSAL_NONE;
    struct {
        const char *snbt;
        int32_t x, y, z;
    } docs[] = {
        /* The game's stack spelling, with the bookkeeping the game
           adds beside it: the pickup byte, the block identity a
           block item repeats, the undamaged marker — those three
           drop with the stack's answer intact, so three of the four
           stacks carry. */
        { "{id: \"minecraft:chest\", CustomName: \"\", Items: "
          "[{Slot: 0b, Name: \"minecraft:diamond\", Count: 3b, "
          "Damage: 0s, WasPickedUp: 0b}, "
          "{Slot: 1b, Name: \"minecraft:stone\", Count: 64b, "
          "Damage: 0s, WasPickedUp: 0b, Block: {name: "
          "\"minecraft:stone\", states: {}, version: 1}}, "
          "{Slot: 2b, Name: \"minecraft:wooden_pickaxe\", Count: 1b, "
          "Damage: 0s, WasPickedUp: 0b, tag: {Damage: 0}}, "
          "{Slot: 3b, Name: \"minecraft:diamond_sword\", Count: 1b, "
          "Damage: 0s, WasPickedUp: 0b, tag: {ench: [{id: 16s, "
          "lvl: 3s}]}}]}",
          53, 10, 2 },
        /* the frozen fixture's spelling of the same data still
           carries: id/count/damage is Bedrock read through another
           writer's hand */
        { "{id: \"minecraft:chest\", Items: "
          "[{Slot: 0b, id: \"minecraft:coal\", count: 5b, "
          "damage: 0s}]}",
          54, 10, 2 },
        { "{id: \"minecraft:sign\", Text: \"mcme fixture\"}",
          57, 10, 5 },
        { "{id: \"minecraft:glowitemframe\", "
          "item: \"minecraft:stick\"}",
          58, 10, 6 },
        /* put takes the game's bare savegame id as readily as the
           namespaced one — the same container, the same answer
           under Java's id */
        { "{id: \"Furnace\", BurnTime: 100s, "
          "CookTime: 40s, Items: [{Slot: 0b, id: \"minecraft:coal\", "
          "count: 5b, damage: 0s}]}",
          56, 10, 4 },
    };
    for (size_t i = 0; i < sizeof docs / sizeof docs[0]; ++i) {
        McmeTag *entity = NULL;
        CHECK(mcme_nbt_parse_snbt(arena, docs[i].snbt,
                                  strlen(docs[i].snbt), &entity));
        CHECK(entity != NULL);
        CHECK(mcme_bedrock_put_block_entity(dbdir, docs[i].x,
                                            docs[i].y, docs[i].z, 0,
                                            entity, arena, &refusal));
    }
    CHECK(refusal == MCME_WRITE_REFUSAL_NONE);

    /* The game's own spelling, appended to the same record as the
       bytes the game would write: short savegame ids. The first
       document's CustomName is an emoji — a four-byte standard
       UTF-8 sequence, the very thing a decoder that allowed only
       modified UTF-8's spellings would refuse, losing the whole
       record (measured, a real world: three records, 621
       documents between them). The loot chest — contents not yet
       generated —
       must NOT carry: an empty container where a full one belonged
       is exactly what the mapping refuses to write. */
    static const char * const real_docs[] = {
        "{id: \"Chest\", x: 60, y: 10, z: 7, "
        "CustomName: \"\xF0\x9F\xA6\xB4\", Items: "
        "[{Slot: 0b, Name: \"minecraft:emerald\", Count: 2b, "
        "Damage: 0s, WasPickedUp: 0b}]}",
        "{id: \"Chest\", x: 61, y: 10, z: 7, "
        "LootTable: \"loot_tables/chests/simple_dungeon.json\", "
        "LootTableSeed: 5}",
        "{id: \"TrappedChest\", x: 62, y: 10, z: 7, Items: "
        "[{Slot: 0b, Name: \"minecraft:gold_ingot\", Count: 7b, "
        "Damage: 0s, WasPickedUp: 0b}]}",

        /* D22: the non-container families, in the game's own
           spelling — a container of one stack, a brewing stand
           whose progress Java spells BrewTime, a spawner choosing
           a mob both editions name alike, and the types whose
           Java block entity answers with only id and position. */
        "{id: \"ShulkerBox\", x: 48, y: 10, z: 8, Items: "
        "[{Slot: 0b, Name: \"minecraft:map\", Count: 1b, "
        "Damage: 0s, WasPickedUp: 0b}]}",
        "{id: \"BrewingStand\", x: 49, y: 10, z: 8, "
        "CookTime: 12s, FuelAmount: 3s, FuelTotal: 20s}",
        "{id: \"MobSpawner\", x: 50, y: 10, z: 8, Delay: 20s, "
        "MaxNearbyEntities: 6s, MaxSpawnDelay: 800s, "
        "MinSpawnDelay: 200s, RequiredPlayerRange: 16s, "
        "SpawnCount: 4s, SpawnRange: 4s, EntityIdentifier: "
        "\"minecraft:spider\", DisplayEntityScale: 1.0}",
        "{id: \"Comparator\", x: 52, y: 10, z: 8, "
        "OutputSignal: 5s}",
        "{id: \"Beacon\", x: 55, y: 10, z: 8, primary: 1, "
        "secondary: 2}",
        "{id: \"EndGateway\", x: 59, y: 10, z: 8, Age: 5, "
        "ExitPortal: [1, 2, 3], EndGatewayBadPosChecked: 1b}",
        "{id: \"Campfire\", x: 48, y: 12, z: 8, ItemTime1: 1, "
        "ItemTime2: 2, ItemTime3: 3, ItemTime4: 4}",
        "{id: \"Jukebox\", x: 49, y: 12, z: 8, RecordItem: "
        "{Name: \"minecraft:music_disc_cat\", Count: 1b, "
        "Damage: 0s, WasPickedUp: 0b}}",
        "{id: \"ChiseledBookshelf\", x: 50, y: 12, z: 8, "
        "LastInteractedSlot: 2, Items: [{Slot: 1b, Name: "
        "\"minecraft:book\", Count: 1b, Damage: 0s, "
        "WasPickedUp: 0b}]}",
        "{id: \"Lectern\", x: 51, y: 12, z: 8}",
        "{id: \"Skull\", x: 52, y: 12, z: 8, SkullType: 5b, "
        "Rotation: 180.0f, MouthMoving: 0b}",
        "{id: \"SculkSensor\", x: 55, y: 12, z: 8, "
        "VibrationListener: {selector: {}}}",
        "{id: \"HangingSign\", x: 59, y: 12, z: 8, FrontText: "
        "{Text: \"hi\", SignTextColor: -16777216}}",

        /* D22: counted, not carried — no Java block entity for
           the piston arm; bed and banner keep their color in the
           document while the name lives in the block; the vault's
           block does not exist in Java 1.20. */
        "{id: \"Bed\", x: 48, y: 14, z: 8, color: 3b}",
        "{id: \"Banner\", x: 49, y: 14, z: 8, Base: 13}",
        "{id: \"PistonArm\", x: 50, y: 14, z: 8}",
        "{id: \"Vault\", x: 51, y: 14, z: 8}",
    };
    McmeBuffer record;
    mcme_buffer_init(&record);
    McmeLevelDb *rdb = NULL;
    CHECK(mcme_leveldb_open_rw(&rdb, dbdir));
    if (rdb != NULL) {
        uint8_t rkey[16];
        size_t rklen = 0;
        CHECK(mcme_bedrock_key_build(rkey, sizeof rkey, &rklen, 3, 0,
                                     0, MCME_BEDROCK_TAG_BLOCK_ENTITY,
                                     false, 0));
        bool deleted = false;
        bool have = mcme_leveldb_get(rdb, rkey, rklen, &record,
                                     &deleted);
        CHECK(have);   /* the five puts above built the record */
        for (size_t i = 0; i < sizeof real_docs / sizeof real_docs[0];
             ++i) {
            McmeTag *entity = NULL;
            CHECK(mcme_nbt_parse_snbt(arena, real_docs[i],
                                      strlen(real_docs[i]), &entity));
            CHECK(entity != NULL);
            McmeNbtDoc doc;
            doc.arena = arena;
            doc.root = entity;
            doc.root_name.data = (char *)"";
            doc.root_name.len = 0;
            doc.little_endian = true;
            McmeBuffer encoded;
            mcme_buffer_init(&encoded);
            CHECK(mcme_nbt_write_binary(&doc, &encoded));
            CHECK(mcme_buffer_append(&record, encoded.data,
                                     encoded.len));
            mcme_buffer_free(&encoded);
        }

        /* A modern two-sided sign whose front text holds a real
           newline — SNBT's two escapes cannot spell one, so the
           string is set after parsing. The interior newline splits
           the lines; the text's end does not open a trailing
           empty one (D22). */
        {
            static const char kSign[] =
                "{id: \"Sign\", x: 53, y: 12, z: 8, IsWaxed: 1b, "
                "FrontText: {Text: \"placeholder\", SignTextColor: "
                "-16777216}, BackText: {Text: \"x\", SignTextColor: "
                "-16777216}}";
            McmeTag *entity = NULL;
            CHECK(mcme_nbt_parse_snbt(arena, kSign, strlen(kSign),
                                      &entity));
            CHECK(entity != NULL);
            const McmeTag *ft = mcme_nbt_get(entity, "FrontText");
            CHECK(ft != NULL && ft->type == MCME_TAG_Compound);
            McmeTag *txt = (McmeTag *)mcme_nbt_get(ft, "Text");
            CHECK(txt != NULL && txt->type == MCME_TAG_String);
            CHECK(mcme_nbt_set_string(arena, txt, "line1\nline2",
                                      11));
            McmeNbtDoc doc;
            doc.arena = arena;
            doc.root = entity;
            doc.root_name.data = (char *)"";
            doc.root_name.len = 0;
            doc.little_endian = true;
            McmeBuffer encoded;
            mcme_buffer_init(&encoded);
            CHECK(mcme_nbt_write_binary(&doc, &encoded));
            CHECK(mcme_buffer_append(&record, encoded.data,
                                     encoded.len));
            mcme_buffer_free(&encoded);
        }

        CHECK(mcme_leveldb_put(rdb, rkey, rklen, record.data,
                               record.len));
        mcme_leveldb_close(rdb);
    }
    mcme_buffer_free(&record);

    snprintf(out, sizeof out, "%s_java", in);
    McmeConvertReport rep;
    refusal = MCME_WRITE_REFUSAL_NONE;
    db = NULL;
    if (!mcme_convert_bedrock_java(in, out, &rep, &refusal)) {
        CHECK_MSG(false, "convert carry failed: %s",
                  mcme_last_error());
        mcme_arena_free(arena);
        remove_store(dbdir);
        remove(lvl);
        rmdir(in);
        return;
    }
    CHECK(refusal == MCME_WRITE_REFUSAL_NONE);
    CHECK(rep.chunks == 1);
    CHECK(rep.subchunks == 1);
    CHECK(rep.blocks == 4096);
    CHECK(rep.block_entities_carried == 20);
    CHECK(rep.block_entities == 6);   /* frame, loot chest, bed,
                                         banner, piston arm, vault */
    CHECK(rep.items == 1);            /* the enchanted stack */
    CHECK(rep.water_positions == 1);  /* the water over the stone */
    CHECK(rep.legacy_subchunks == 0);
    CHECK(rep.entities == 0);
    CHECK(rep.actors == 0);          /* no actor keys in this store */
    CHECK(rep.chunk_data == 0);
    CHECK(rep.other == 0);
    /* A converted chunk the store holds no record for: no map
       crosses, and the drop counts (D20). */
    CHECK(rep.heightmaps_carried == 0);
    CHECK(rep.heightmaps == 1);

    /* The store holds no nether or end chunks, so their directories
       are never made - an absent dimension stays absent (D21). */
    char dimdir[512];
    snprintf(dimdir, sizeof dimdir, "%s/DIM-1", out);
    CHECK(!mcme_fs_exists(dimdir));
    snprintf(dimdir, sizeof dimdir, "%s/DIM1", out);
    CHECK(!mcme_fs_exists(dimdir));

    /* The output says what the carry said: the stair takes the
       property, the stone does not, the dry stair stays dry. */
    McmeArena *out_arena = mcme_arena_new();
    CHECK(out_arena != NULL);
    if (out_arena != NULL) {
        McmeNbtDoc chunk;
        McmeTag *root = convert_chunk_root(out_arena, out, 0, 3, 0,
                                           &chunk);
        CHECK(root != NULL);
        if (root != NULL) {
            check_java_block(root, 48, 0, 0,
                "minecraft:oak_stairs{waterlogged: \"true\"}");
            check_java_block(root, 49, 0, 0, "minecraft:stone");
            check_java_block(root, 50, 0, 0, "minecraft:oak_stairs");

            /* The list holds the carried documents in the record's
               order, with the fields the mapping carries: both
               spellings of Bedrock's id out under Java's. */
            const McmeTag *bel = mcme_nbt_path(root,
                                               "block_entities");
            CHECK_MSG(bel != NULL && bel->type == MCME_TAG_List
                      && bel->list.len == 20,
                      "block_entities holds %zu entries, want 20",
                      (bel != NULL && bel->type == MCME_TAG_List)
                          ? bel->list.len : (size_t)0);
            const McmeTag *chest = mcme_nbt_path(root,
                                                 "block_entities.0");
            CHECK(chest != NULL);
            const McmeTag *f = mcme_nbt_path(root,
                                             "block_entities.0.id");
            CHECK(f != NULL && f->type == MCME_TAG_String
                  && strcmp(f->string.data,
                            "minecraft:chest") == 0);
            CHECK(mcme_nbt_int_or(mcme_nbt_path(root,
                "block_entities.0.x"), -1) == 53);
            CHECK(mcme_nbt_int_or(mcme_nbt_path(root,
                "block_entities.0.y"), -1) == 10);
            CHECK(mcme_nbt_int_or(mcme_nbt_path(root,
                "block_entities.0.z"), -1) == 2);
            /* the empty CustomName has nothing to say: dropped */
            CHECK(chest != NULL
                  && mcme_nbt_get(chest, "CustomName") == NULL);
            const McmeTag *items = mcme_nbt_path(root,
                                                 "block_entities.0.Items");
            CHECK(items != NULL && items->type == MCME_TAG_List
                  && items->list.len == 3);
            f = mcme_nbt_path(root, "block_entities.0.Items.0.id");
            CHECK(f != NULL && f->type == MCME_TAG_String
                  && strcmp(f->string.data,
                            "minecraft:diamond") == 0);
            CHECK(mcme_nbt_byte_or(mcme_nbt_path(root,
                "block_entities.0.Items.0.Slot"), -1) == 0);
            CHECK(mcme_nbt_byte_or(mcme_nbt_path(root,
                "block_entities.0.Items.0.Count"), -1) == 3);
            CHECK(mcme_nbt_byte_or(mcme_nbt_path(root,
                "block_entities.0.Items.1.Count"), -1) == 64);
            /* damage 0 carries no tag */
            const McmeTag *st0 = mcme_nbt_path(root,
                                               "block_entities.0.Items.0");
            CHECK(st0 != NULL && mcme_nbt_get(st0, "tag") == NULL);
            /* the block identity compound the block item repeated
               stays behind: Java's item id already says it */
            const McmeTag *st1 = mcme_nbt_path(root,
                                               "block_entities.0.Items.1");
            CHECK(st1 != NULL && mcme_nbt_get(st1, "Block") == NULL);
            /* the undamaged marker drops the same way */
            const McmeTag *st2 = mcme_nbt_path(root,
                                               "block_entities.0.Items.2");
            CHECK(st2 != NULL && mcme_nbt_get(st2, "tag") == NULL);
            f = mcme_nbt_path(root, "block_entities.0.Items.2.id");
            CHECK(f != NULL && f->type == MCME_TAG_String
                  && strcmp(f->string.data,
                            "minecraft:wooden_pickaxe") == 0);

            /* the enchanted stack is gone; the clean one stands */
            items = mcme_nbt_path(root, "block_entities.1.Items");
            CHECK(items != NULL && items->type == MCME_TAG_List
                  && items->list.len == 1);
            f = mcme_nbt_path(root, "block_entities.1.Items.0.id");
            CHECK(f != NULL && f->type == MCME_TAG_String
                  && strcmp(f->string.data,
                            "minecraft:coal") == 0);

            /* The old-format sign: its top-level text has no
               measured Java spelling, so the document carries
               with both sides empty — four escaped-empty messages
               each, no is_waxed (D22) */
            f = mcme_nbt_path(root, "block_entities.2.id");
            CHECK(f != NULL && f->type == MCME_TAG_String
                  && strcmp(f->string.data,
                            "minecraft:sign") == 0);
            CHECK(mcme_nbt_int_or(mcme_nbt_path(root,
                "block_entities.2.x"), -1) == 57);
            f = mcme_nbt_path(root,
                "block_entities.2.front_text.messages.0");
            CHECK_MSG(f != NULL && f->type == MCME_TAG_String
                      && strcmp(f->string.data, "\\\"\\\"") == 0,
                      "the old sign's front message is not the "
                      "escaped-empty pad");
            f = mcme_nbt_path(root,
                "block_entities.2.back_text.messages.3");
            CHECK(f != NULL && f->type == MCME_TAG_String
                  && strcmp(f->string.data, "\\\"\\\"") == 0);
            CHECK(mcme_nbt_path(root,
                  "block_entities.2.is_waxed") == NULL);

            /* the furnace's timers, as Java's ints */
            f = mcme_nbt_path(root, "block_entities.3.id");
            CHECK(f != NULL && f->type == MCME_TAG_String
                  && strcmp(f->string.data,
                            "minecraft:furnace") == 0);
            CHECK(mcme_nbt_int_or(mcme_nbt_path(root,
                "block_entities.3.BurnTime"), -1) == 100);
            CHECK(mcme_nbt_int_or(mcme_nbt_path(root,
                "block_entities.3.CookTime"), -1) == 40);

            /* the game's short id, out under Java's namespaced
               answer: "Chest" and "minecraft:chest" are the same
               container in two writers' hands */
            f = mcme_nbt_path(root, "block_entities.4.id");
            CHECK(f != NULL && f->type == MCME_TAG_String
                  && strcmp(f->string.data,
                            "minecraft:chest") == 0);
            CHECK(mcme_nbt_int_or(mcme_nbt_path(root,
                "block_entities.4.x"), -1) == 60);
            /* its emoji CustomName — standard UTF-8 in, the same
               text out: a record of four-byte strings decodes */
            f = mcme_nbt_path(root,
                              "block_entities.4.CustomName");
            CHECK_MSG(f != NULL && f->type == MCME_TAG_String
                      && f->string.len == 4
                      && memcmp(f->string.data,
                                "\xF0\x9F\xA6\xB4", 4) == 0,
                      "the emoji CustomName did not survive the "
                      "record read");
            items = mcme_nbt_path(root, "block_entities.4.Items");
            CHECK(items != NULL && items->type == MCME_TAG_List
                  && items->list.len == 1);
            CHECK(mcme_nbt_byte_or(mcme_nbt_path(root,
                "block_entities.4.Items.0.Count"), -1) == 2);
            f = mcme_nbt_path(root, "block_entities.4.Items.0.id");
            CHECK(f != NULL && f->type == MCME_TAG_String
                  && strcmp(f->string.data,
                            "minecraft:emerald") == 0);

            /* TrappedChest, the game's spelling, out as Java's
               trapped_chest — the loot chest between them dropped */
            f = mcme_nbt_path(root, "block_entities.5.id");
            CHECK(f != NULL && f->type == MCME_TAG_String
                  && strcmp(f->string.data,
                            "minecraft:trapped_chest") == 0);
            CHECK(mcme_nbt_byte_or(mcme_nbt_path(root,
                "block_entities.5.Items.0.Count"), -1) == 7);

            /* D22's families, in record order after them: the
               container with the game's short id, the brewing
               stand, the spawner, the two empty-position types
               with fields, and the rest. */
            /* 6: shulker_box keeps its one stack */
            f = mcme_nbt_path(root, "block_entities.6.id");
            CHECK(f != NULL && f->type == MCME_TAG_String
                  && strcmp(f->string.data,
                            "minecraft:shulker_box") == 0);
            f = mcme_nbt_path(root,
                              "block_entities.6.Items.0.id");
            CHECK(f != NULL && f->type == MCME_TAG_String
                  && strcmp(f->string.data,
                            "minecraft:map") == 0);

            /* 7: brewing — progress under BrewTime, fuel under
               Fuel; the furnace's CookTime is not its progress
               and FuelTotal stays behind */
            f = mcme_nbt_path(root, "block_entities.7.id");
            CHECK(f != NULL && f->type == MCME_TAG_String
                  && strcmp(f->string.data,
                            "minecraft:brewing_stand") == 0);
            CHECK(mcme_nbt_int_or(mcme_nbt_path(root,
                "block_entities.7.BrewTime"), -1) == 12);
            CHECK(mcme_nbt_int_or(mcme_nbt_path(root,
                "block_entities.7.Fuel"), -1) == 3);
            CHECK(mcme_nbt_path(root,
                  "block_entities.7.CookTime") == NULL);
            CHECK(mcme_nbt_path(root,
                  "block_entities.7.FuelTotal") == NULL);

            /* 8: the spawner's ranges under the same names, the
               mob under SpawnData; the display fields and Java's
               own SpawnPotentials default stay behind */
            f = mcme_nbt_path(root, "block_entities.8.id");
            CHECK(f != NULL && f->type == MCME_TAG_String
                  && strcmp(f->string.data,
                            "minecraft:mob_spawner") == 0);
            CHECK(mcme_nbt_int_or(mcme_nbt_path(root,
                "block_entities.8.Delay"), -1) == 20);
            CHECK(mcme_nbt_int_or(mcme_nbt_path(root,
                "block_entities.8.MaxSpawnDelay"), -1) == 800);
            f = mcme_nbt_path(root,
                              "block_entities.8.SpawnData.id");
            CHECK(f != NULL && f->type == MCME_TAG_String
                  && strcmp(f->string.data,
                            "minecraft:spider") == 0);
            CHECK(mcme_nbt_path(root,
                  "block_entities.8.SpawnPotentials") == NULL);
            CHECK(mcme_nbt_path(root,
                  "block_entities.8.DisplayEntityScale") == NULL);

            /* 9: the comparator's signal */
            f = mcme_nbt_path(root, "block_entities.9.id");
            CHECK(f != NULL && f->type == MCME_TAG_String
                  && strcmp(f->string.data,
                            "minecraft:comparator") == 0);
            CHECK(mcme_nbt_int_or(mcme_nbt_path(root,
                "block_entities.9.OutputSignal"), -1) == 5);

            /* 10: the beacon's two chosen effects under Java's
               capitals; Java fills the rest itself */
            f = mcme_nbt_path(root, "block_entities.10.id");
            CHECK(f != NULL && f->type == MCME_TAG_String
                  && strcmp(f->string.data,
                            "minecraft:beacon") == 0);
            CHECK(mcme_nbt_int_or(mcme_nbt_path(root,
                "block_entities.10.Primary"), -1) == 1);
            CHECK(mcme_nbt_int_or(mcme_nbt_path(root,
                "block_entities.10.Secondary"), -1) == 2);
            CHECK(mcme_nbt_path(root,
                  "block_entities.10.Levels") == NULL);

            /* 11: the gateway's age and the exit portal as X/Y/Z */
            f = mcme_nbt_path(root, "block_entities.11.id");
            CHECK(f != NULL && f->type == MCME_TAG_String
                  && strcmp(f->string.data,
                            "minecraft:end_gateway") == 0);
            CHECK(mcme_nbt_int_or(mcme_nbt_path(root,
                "block_entities.11.Age"), -1) == 5);
            CHECK(mcme_nbt_int_or(mcme_nbt_path(root,
                "block_entities.11.ExitPortal.X"), -1) == 1);
            CHECK(mcme_nbt_int_or(mcme_nbt_path(root,
                "block_entities.11.ExitPortal.Y"), -1) == 2);
            CHECK(mcme_nbt_int_or(mcme_nbt_path(root,
                "block_entities.11.ExitPortal.Z"), -1) == 3);
            CHECK(mcme_nbt_path(root,
                  "block_entities.11.EndGatewayBadPosChecked")
                  == NULL);

            /* 12: the four cooks' progress as Java's one int
               array, in ItemTime order */
            f = mcme_nbt_path(root, "block_entities.12.id");
            CHECK(f != NULL && f->type == MCME_TAG_String
                  && strcmp(f->string.data,
                            "minecraft:campfire") == 0);
            {
                const McmeTag *ct = mcme_nbt_path(root,
                    "block_entities.12.CookingTimes");
                CHECK(ct != NULL
                      && ct->type == MCME_TAG_Int_Array
                      && ct->ints.len == 4
                      && ct->ints.data[0] == 1
                      && ct->ints.data[1] == 2
                      && ct->ints.data[2] == 3
                      && ct->ints.data[3] == 4);
            }

            /* 13: the jukebox's record — the stack rule without
               a Slot, which is where a record stands */
            f = mcme_nbt_path(root, "block_entities.13.id");
            CHECK(f != NULL && f->type == MCME_TAG_String
                  && strcmp(f->string.data,
                            "minecraft:jukebox") == 0);
            f = mcme_nbt_path(root,
                              "block_entities.13.RecordItem.id");
            CHECK(f != NULL && f->type == MCME_TAG_String
                  && strcmp(f->string.data,
                            "minecraft:music_disc_cat") == 0);
            CHECK(mcme_nbt_byte_or(mcme_nbt_path(root,
                "block_entities.13.RecordItem.Count"), -1) == 1);
            CHECK(mcme_nbt_path(root,
                  "block_entities.13.RecordItem.Slot") == NULL);

            /* 14: the bookshelf's last-touched slot and stack */
            f = mcme_nbt_path(root, "block_entities.14.id");
            CHECK(f != NULL && f->type == MCME_TAG_String
                  && strcmp(f->string.data,
                            "minecraft:chiseled_bookshelf") == 0);
            CHECK(mcme_nbt_byte_or(mcme_nbt_path(root,
                "block_entities.14.last_interacted_slot"),
                -1) == 2);
            f = mcme_nbt_path(root,
                              "block_entities.14.Items.0.id");
            CHECK(f != NULL && f->type == MCME_TAG_String
                  && strcmp(f->string.data,
                            "minecraft:book") == 0);

            /* 15-17: the types whose Java block entity answers
               with id and position alone */
            f = mcme_nbt_path(root, "block_entities.15.id");
            CHECK(f != NULL && f->type == MCME_TAG_String
                  && strcmp(f->string.data,
                            "minecraft:lectern") == 0);
            f = mcme_nbt_path(root, "block_entities.16.id");
            CHECK(f != NULL && f->type == MCME_TAG_String
                  && strcmp(f->string.data,
                            "minecraft:skull") == 0);
            CHECK(mcme_nbt_path(root,
                  "block_entities.16.Rotation") == NULL);
            f = mcme_nbt_path(root, "block_entities.17.id");
            CHECK(f != NULL && f->type == MCME_TAG_String
                  && strcmp(f->string.data,
                            "minecraft:sculk_sensor") == 0);
            CHECK(mcme_nbt_path(root,
                  "block_entities.17.VibrationListener") == NULL);

            /* 18: the hanging sign, one line, three pads */
            f = mcme_nbt_path(root, "block_entities.18.id");
            CHECK(f != NULL && f->type == MCME_TAG_String
                  && strcmp(f->string.data,
                            "minecraft:hanging_sign") == 0);
            f = mcme_nbt_path(root,
                              "block_entities.18.front_text.messages.0");
            CHECK(f != NULL && f->type == MCME_TAG_String
                  && strcmp(f->string.data, "\"hi\"") == 0);
            f = mcme_nbt_path(root,
                              "block_entities.18.front_text.messages.1");
            CHECK(f != NULL && f->type == MCME_TAG_String
                  && strcmp(f->string.data, "\\\"\\\"") == 0);

            /* 19: the modern sign — the interior newline splits
               two lines, the text's end opens no third; back has
               one; is_waxed crossed */
            f = mcme_nbt_path(root, "block_entities.19.id");
            CHECK(f != NULL && f->type == MCME_TAG_String
                  && strcmp(f->string.data,
                            "minecraft:sign") == 0);
            f = mcme_nbt_path(root,
                "block_entities.19.front_text.messages.0");
            CHECK_MSG(f != NULL && f->type == MCME_TAG_String
                      && strcmp(f->string.data,
                                "\"line1\"") == 0,
                      "the sign's first line is not its JSON "
                      "string");
            f = mcme_nbt_path(root,
                "block_entities.19.front_text.messages.1");
            CHECK(f != NULL && f->type == MCME_TAG_String
                  && strcmp(f->string.data,
                            "\"line2\"") == 0);
            f = mcme_nbt_path(root,
                "block_entities.19.front_text.messages.2");
            CHECK(f != NULL && f->type == MCME_TAG_String
                  && strcmp(f->string.data, "\\\"\\\"") == 0);
            f = mcme_nbt_path(root,
                "block_entities.19.back_text.messages.0");
            CHECK(f != NULL && f->type == MCME_TAG_String
                  && strcmp(f->string.data, "\"x\"") == 0);
            CHECK(mcme_nbt_byte_or(mcme_nbt_path(root,
                "block_entities.19.front_text.has_glowing_text"),
                -1) == 0);
            f = mcme_nbt_path(root,
                "block_entities.19.front_text.color");
            CHECK(f != NULL && f->type == MCME_TAG_String
                  && strcmp(f->string.data, "black") == 0);
            CHECK(mcme_nbt_byte_or(mcme_nbt_path(root,
                "block_entities.19.is_waxed"), -1) == 1);
        }
        mcme_arena_free(out_arena);
    }

    char region_dir[512];
    snprintf(region_dir, sizeof region_dir, "%s/region", out);
    remove_store(region_dir);
    char file[512];
    snprintf(file, sizeof file, "%s/level.dat", out);
    remove(file);
    rmdir(out);
    remove_store(dbdir);
    remove(lvl);
    rmdir(in);
    mcme_arena_free(arena);
}

/*
 * `mcme chunk prune` against both fixtures: whole chunks, selected by
 * a box of columns (decisions.md#D13). The Bedrock half tombstones
 * every key the chunk owns under the LOCK; the Java half zeroes the
 * slot and leaves the payload bytes for `region compact`.
 */
static void test_prune(void)
{
    printf("  prune (whole chunks by column, both worlds)\n");

    McmeWriteRefusal refusal;
    McmePruneReport rep;
    McmeBox box;

    /* --- Bedrock: the fixture's eight keys — a six-subchunk column
       at (0,0), one key at (1,-2), one nether key at (0,0). --- */
    char src[512];
    snprintf(src, sizeof src, "%s/db", kChunksWorld);
    if (!mcme_fs_exists(src)) {
        printf("  SKIP prune bedrock (%s not found)\n", src);
    } else {
        char world[64], dbdir[512];
        CHECK(fresh_store_dir(world, sizeof world));
        snprintf(dbdir, sizeof dbdir, "%s/db", world);
        CHECK(copy_store(src, dbdir));

        /* Corners in either order: the box reaches one column. Six
           keys are one chunk, not six. */
        box = (McmeBox){ 15, 7, 15, 0, 0, 0 };
        refusal = MCME_WRITE_REFUSAL_NONE;
        CHECK(mcme_prune(world, &box, 0, &rep, &refusal));
        CHECK(rep.chunks == 1 && rep.keys == 6 && rep.region_files == 0);

        McmeLevelDb *db = NULL;
        CHECK(mcme_leveldb_open(&db, dbdir));
        if (db != NULL) {
            /* the pruned chunk's key reads as a tombstone; the
               neighbour and the other dimension still count */
            uint8_t key[16];
            size_t klen = 0;
            bool deleted = false;
            McmeBuffer val;
            CHECK(mcme_bedrock_subchunk_key(0, 0, 0, 0, key,
                                            sizeof key, &klen));
            mcme_buffer_init(&val);
            CHECK(mcme_leveldb_get(db, key, klen, &val, &deleted));
            CHECK(deleted);
            mcme_buffer_free(&val);
            CHECK(mcme_leveldb_count(db) == 2);
            mcme_leveldb_close(db);
        }

        /* the same box again: nothing to delete, nothing written */
        refusal = MCME_WRITE_REFUSAL_NONE;
        CHECK(mcme_prune(world, &box, 0, &rep, &refusal));
        CHECK(rep.chunks == 0 && rep.keys == 0 && rep.region_files == 0);

        /* Box reach is chunk columns: blocks 0..-32 floor to the
           column at (1,-2), which the first box did not touch. */
        box = (McmeBox){ 0, 0, 0, 31, 0, -32 };
        CHECK(mcme_prune(world, &box, 0, &rep, &refusal));
        CHECK(rep.chunks == 1 && rep.keys == 1);

        /* the other dimension only goes with its own flag */
        box = (McmeBox){ 0, 0, 0, 15, 0, 15 };
        CHECK(mcme_prune(world, &box, 1, &rep, &refusal));
        CHECK(rep.chunks == 1 && rep.keys == 1);

        /* refusal vocabulary */
        CHECK(!mcme_prune(NULL, &box, 0, &rep, &refusal));
        CHECK(refusal == MCME_WRITE_REFUSAL_USAGE);
        CHECK(!mcme_prune(world, NULL, 0, &rep, &refusal));
        CHECK(refusal == MCME_WRITE_REFUSAL_USAGE);
        char empty[64];
        CHECK(fresh_store_dir(empty, sizeof empty));
        CHECK(!mcme_prune(empty, &box, 0, &rep, &refusal));
        CHECK(refusal == MCME_WRITE_REFUSAL_NOT_THERE);
        rmdir(empty);
        mcme_clear_error();

        /* the LOCK the game holds refuses before a key is touched */
        McmeLevelDb *hold = NULL;
        CHECK(mcme_leveldb_open_rw(&hold, dbdir));
        CHECK(!mcme_prune(world, &box, 0, &rep, &refusal));
        CHECK(refusal == MCME_WRITE_REFUSAL_STORE);
        if (hold != NULL) mcme_leveldb_close(hold);
        mcme_clear_error();

        remove_store(dbdir);
        rmdir(world);
    }

    /* --- Java: the slot is zeroed and the payload bytes stay until
       region compact; the golden world ships four chunks in r.0.0
       and one in r.-1.-1. --- */
    if (!mcme_fs_exists(kWorldDir)) {
        printf("  SKIP prune java (%s not found)\n", kWorldDir);
        return;
    }

    char jw[64];
    CHECK(fresh_store_dir(jw, sizeof jw));
    char lvl_src[512], lvl_dst[600], reg_src[560], reg_dst[600];
    char file00[640];
    snprintf(lvl_src, sizeof lvl_src, "%s/level.dat", kWorldDir);
    snprintf(lvl_dst, sizeof lvl_dst, "%s/level.dat", jw);
    snprintf(reg_dst, sizeof reg_dst, "%s/region", jw);
    snprintf(file00, sizeof file00, "%s/r.0.0.mca", reg_dst);
    {
        void *raw = NULL;
        size_t raw_len = 0;
        CHECK(mcme_fs_read(lvl_src, &raw, &raw_len));
        CHECK(mcme_fs_write(lvl_dst, raw, raw_len));
        free(raw);
    }
    CHECK(mcme_fs_mkdir_p(reg_dst));
    snprintf(reg_src, sizeof reg_src, "%s/region/r.0.0.mca", kWorldDir);
    {
        void *raw = NULL;
        size_t raw_len = 0;
        CHECK(mcme_fs_read(reg_src, &raw, &raw_len));
        CHECK(mcme_fs_write(file00, raw, raw_len));
        free(raw);
    }
    char file_nether[640], file_nether_src[560];
    snprintf(file_nether, sizeof file_nether, "%s/r.-1.-1.mca", reg_dst);
    snprintf(file_nether_src, sizeof file_nether_src,
             "%s/region/r.-1.-1.mca", kWorldDir);
    {
        void *raw = NULL;
        size_t raw_len = 0;
        CHECK(mcme_fs_read(file_nether_src, &raw, &raw_len));
        CHECK(mcme_fs_write(file_nether, raw, raw_len));
        free(raw);
    }

    /* the copy stands as the fixture says it should */
    McmeRegion reg;
    CHECK(mcme_region_open(&reg, file00));
    CHECK(mcme_region_chunk_present(&reg, 0, 0));
    CHECK(mcme_region_chunk_present(&reg, 1, 0));
    CHECK(mcme_region_chunk_present(&reg, 0, 1));
    CHECK(mcme_region_chunk_present(&reg, 31, 31));
    mcme_region_close(&reg);

    /* Y is parsed and ignored: the box at y 300 prunes the same
       column a box at y 0 would. */
    box = (McmeBox){ 0, 300, 0, 15, 301, 15 };
    refusal = MCME_WRITE_REFUSAL_NONE;
    CHECK(mcme_prune(jw, &box, 0, &rep, &refusal));
    CHECK(rep.chunks == 1 && rep.keys == 0 && rep.region_files == 1);

    CHECK(mcme_region_open(&reg, file00));
    CHECK(!mcme_region_chunk_present(&reg, 0, 0));
    CHECK(mcme_region_chunk_present(&reg, 1, 0));
    CHECK(mcme_region_chunk_present(&reg, 0, 1));
    mcme_region_close(&reg);
    /* the other region file is not in the box's reach at all */
    CHECK(mcme_region_open(&reg, file_nether));
    CHECK(mcme_region_chunk_present(&reg, -1, -1));
    mcme_region_close(&reg);

    /* region compact reclaims the punched payload's bytes; the hole
       stays a hole. */
    McmeRegionCompact c;
    refusal = MCME_WRITE_REFUSAL_NONE;
    CHECK(mcme_region_compact(file00, &c, &refusal));
    CHECK(c.chunks == 3 && c.after < c.before);
    CHECK(mcme_region_open(&reg, file00));
    CHECK(!mcme_region_chunk_present(&reg, 0, 0));
    CHECK(mcme_region_chunk_present(&reg, 31, 31));
    mcme_region_close(&reg);

    /* a column no chunk stands in: nothing to delete, no files
       written */
    box = (McmeBox){ 32, 0, 32, 47, 0, 47 };   /* chunk (2, 2) */
    CHECK(mcme_prune(jw, &box, 0, &rep, &refusal));
    CHECK(rep.chunks == 0 && rep.region_files == 0);

    /* the dimension the game has not made, and the number Java
       keeps no directory for */
    box = (McmeBox){ 0, 0, 0, 15, 0, 15 };
    CHECK(!mcme_prune(jw, &box, 1, &rep, &refusal));
    CHECK(refusal == MCME_WRITE_REFUSAL_NOT_THERE);
    CHECK(!mcme_prune(jw, &box, 7, &rep, &refusal));
    CHECK(refusal == MCME_WRITE_REFUSAL_USAGE);
    mcme_clear_error();

    /* a region file that will not read refuses the whole prune with
       nothing punched: the copy becomes 8 bytes of garbage, and the
       prune must leave those bytes exactly as they are. */
    {
        uint8_t garbage[8] = { 'G', 'A', 'R', 'B', 'A', 'G', 'E', '!' };
        void *before = NULL, *after = NULL;
        size_t before_len = 0, after_len = 0;
        CHECK(mcme_fs_write(file00, garbage, sizeof garbage));
        CHECK(mcme_fs_read(file00, &before, &before_len));
        CHECK(!mcme_prune(jw, &box, 0, &rep, &refusal));
        CHECK(refusal == MCME_WRITE_REFUSAL_NOT_READABLE);
        CHECK(mcme_fs_read(file00, &after, &after_len));
        CHECK(after_len == before_len
              && memcmp(before, after, before_len) == 0);
        free(before);
        free(after);
        mcme_clear_error();
    }

    remove_store(reg_dst);
    remove(lvl_dst);
    rmdir(jw);
}

int mcme_test_world(void)
{
    g_failed = 0;
    printf("world\n");

    test_bedrock_keys();
    test_bedrock_subchunks();
    test_subchunk_refusals();
    test_biome_record();
    test_bedrock_inventory();
    test_bedrock_write();
    test_bedrock_column();
    test_bedrock_entity();
    test_bedrock_entity_write();

    test_translate();
    test_convert();
    test_convert_carry();
    test_prune();

    if (!mcme_fs_exists(kWorldDir)) {
        printf("  SKIP golden world (%s not found; run tools/ref_worlds.py)\n",
               kWorldDir);
        return g_failed;
    }

    test_container();
    test_chunks();
    test_region_write();
    test_region_compact();
    test_java_chunk();
    test_java_fill();

    return g_failed;
}
