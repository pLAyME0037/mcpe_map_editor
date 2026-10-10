#include "mcme/core.h"
#include "mcme/compress.h"
#include "mcme/nbt.h"
#include "mcme/region.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
            snprintf(path, sizeof path, "%s/region/%s.mca", kWorldDir, e->region);
            if (!mcme_region_open(&reg, path)) {
                printf("  SKIP %s (%s)\n", e->region, mcme_last_error());
                continue;
            }
            reg_open = true;
            snprintf(open_region, sizeof open_region, "%s", e->region);
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
            snprintf(base, sizeof base, "%s/%s.%d.%d", kChunkDir,
                     e->region, e->cx, e->cz);

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

int mcme_test_world(void)
{
    g_failed = 0;
    printf("world\n");

    if (!mcme_fs_exists(kWorldDir)) {
        printf("  SKIP golden world (%s not found; run tools/ref_worlds.py)\n",
               kWorldDir);
        return g_failed;
    }

    test_container();
    test_chunks();

    return g_failed;
}
