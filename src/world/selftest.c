#include "mcme/bedrock_chunk.h"
#include "mcme/bedrock_keys.h"
#include "mcme/bedrock_write.h"
#include "mcme/core.h"
#include "mcme/compress.h"
#include "mcme/leveldb.h"
#include "mcme/nbt.h"
#include "mcme/region.h"
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
        snprintf(base, sizeof base, "%s/%s", kSubchunkDir, names[c]);

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

        CHECK(mcme_bedrock_set_block(dbdir, 0, 0, 0, 0, &placed));
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
                                     &a.layers[0].palette[want]));
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
                                     &a.layers[0].palette[a.layers[0].indices[u]]));
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
        CHECK(mcme_bedrock_set_block(dbdir, 5, 22, 7, 0, &placed));
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
        CHECK(mcme_bedrock_set_block(dbdir, 0, -1, 0, 0, &placed));
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
        CHECK(mcme_bedrock_set_block(dbdir, 0, 68, 0, 0, &placed));
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

        CHECK(mcme_bedrock_set_block(dbdir, 0, 0, 0, 1, &placed));
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

    /* --- Refusals: an absent subchunk is refused,
           not created, and the direct API says what
           it was handed. --- */
    {
        CHECK(fresh_store_dir(tmp, sizeof tmp));
        snprintf(dbdir, sizeof dbdir, "%s/db", tmp);
        CHECK(copy_store(src, dbdir));
        CHECK(decode_fixture("v8-bits4", arena, &a));
        CHECK(mcme_bedrock_block_entry(arena, "mcme:placed",
                                       NULL, &placed));

        mcme_clear_error();
        CHECK(!mcme_bedrock_set_block(dbdir, 0, 0, 3 * 16, 0,
                                      &placed));
        CHECK(mcme_last_error()[0] != '\0');
        CHECK(!mcme_bedrock_set_block(dbdir, 32, 0, 0, 0,
                                      &placed));
        CHECK(mcme_last_error()[0] != '\0');
        /* Subchunk Y past the key's signed byte. */
        CHECK(!mcme_bedrock_set_block(dbdir, 0, 2048, 0, 0,
                                      &placed));
        CHECK(mcme_last_error()[0] != '\0');
        CHECK(!mcme_bedrock_set_block(NULL, 0, 0, 0, 0, &placed));
        CHECK(!mcme_bedrock_set_block(dbdir, 0, 0, 0, 0, NULL));
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
        CHECK(mcme_bedrock_fill_check(db, &box, 0));
        CHECK(mcme_bedrock_fill(db, &box, 0, &placed));
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

    /* --- A fill whose subchunks are not all stored writes
       nothing at all. --- */
    {
        CHECK(fresh_store_dir(tmp, sizeof tmp));
        snprintf(dbdir, sizeof dbdir, "%s/db", tmp);
        CHECK(copy_store(src, dbdir));
        CHECK(mcme_bedrock_block_entry(arena, "mcme:placed",
                                       NULL, &placed));

        McmeBox hole = {0, 0, 0, 0, 48, 0};  /* cy3 absent */
        CHECK(mcme_leveldb_open_rw(&db, dbdir));
        CHECK(!mcme_bedrock_fill_check(db, &hole, 0));
        CHECK(mcme_last_error()[0] != '\0');
        CHECK(!mcme_bedrock_fill(db, &hole, 0, &placed));
        mcme_leveldb_close(db);
        db = NULL;

        /* Nothing moved: the first subchunk is still the
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
            mcme_leveldb_close(db);
            db = NULL;
            mcme_buffer_free(&raw);
            free(want);
        }

        /* A box reaching past the key's signed byte is
           refused as unfindable, and null arguments are
           refused. */
        CHECK(mcme_leveldb_open_rw(&db, dbdir));
        McmeBox deep = {0, -3000, 0, 0, -2000, 0};
        CHECK(!mcme_bedrock_fill_check(db, &deep, 0));
        CHECK(!mcme_bedrock_fill(db, &deep, 0, &placed));
        CHECK(!mcme_bedrock_fill_check(NULL, &deep, 0));
        CHECK(!mcme_bedrock_fill_check(db, NULL, 0));
        CHECK(!mcme_bedrock_fill(db, NULL, 0, &placed));
        CHECK(!mcme_bedrock_fill(db, &deep, 0, NULL));
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

int mcme_test_world(void)
{
    g_failed = 0;
    printf("world\n");

    test_bedrock_keys();
    test_bedrock_subchunks();
    test_subchunk_refusals();
    test_bedrock_inventory();
    test_bedrock_write();

    if (!mcme_fs_exists(kWorldDir)) {
        printf("  SKIP golden world (%s not found; run tools/ref_worlds.py)\n",
               kWorldDir);
        return g_failed;
    }

    test_container();
    test_chunks();

    return g_failed;
}
