#include "mcme/bedrock_chunk.h"
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

int mcme_test_world(void)
{
    g_failed = 0;
    printf("world\n");

    test_bedrock_keys();
    test_bedrock_subchunks();
    test_subchunk_refusals();
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
