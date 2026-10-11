#include "mcme/bedrock_keys.h"
#include "mcme/core.h"
#include "mcme/compress.h"
#include "mcme/leveldb.h"
#include "mcme/nbt.h"
#include "mcme/region.h"
#include "mcme/world.h"

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

/* ========================================================================
 * Bedrock chunk keys
 * ======================================================================== */

static const char kForgeWorld[] = "tests/golden/worlds/bedrock-forge";

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

int mcme_test_world(void)
{
    g_failed = 0;
    printf("world\n");

    test_bedrock_keys();
    test_bedrock_inventory();

    if (!mcme_fs_exists(kWorldDir)) {
        printf("  SKIP golden world (%s not found; run tools/ref_worlds.py)\n",
               kWorldDir);
        return g_failed;
    }

    test_container();
    test_chunks();

    return g_failed;
}
