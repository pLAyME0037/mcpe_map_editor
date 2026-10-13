#include "mcme/core.h"
#include "mcme/compress.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * Compression self-tests.
 *
 * The vectors in tests/golden/compress come from Python's `zlib` and
 * `lz4.block` (see tools/ref_compress.py) — implementations independent of the
 * ones we link. That matters: inflating our own compressor's output would only
 * prove the pair is self-consistent, not that Minecraft could read it. So the
 * primary check here is that we reproduce the exact plaintext from somebody
 * else's compressed bytes.
 *
 * Round-tripping our own output is also tested, but as a secondary invariant:
 * it catches buffer-management bugs in the streaming loops (a short read that
 * the golden path never exercises because the vector sizes differ).
 *
 * If the golden data is absent, these tests are skipped rather than failed, so
 * `mcme selftest` stays runnable from any directory.
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

static const char kGoldenDir[] = "tests/golden/compress";

/* Suffix -> codec, for the three containers Minecraft actually writes. */
static const struct {
    const char *suffix;
    McmeCompression codec;
} kCodecs[] = {
    { ".gz",    MCME_COMPRESS_GZIP },
    { ".zlib",  MCME_COMPRESS_ZLIB },
    { ".lz4",   MCME_COMPRESS_LZ4  },
};

static const char *const kNames[] = {
    "text", "zeros", "random", "nuls", "tiny", "big",
};

static void path_join(char *dst, size_t cap, const char *name, const char *suffix)
{
    snprintf(dst, cap, "%s/%s%s", kGoldenDir, name, suffix);
}

/* Reads a whole file into malloc'd storage. Returns NULL on failure. */
static uint8_t *slurp(const char *path, size_t *out_len)
{
    void *data = NULL;
    size_t len = 0;
    if (!mcme_fs_read(path, &data, &len)) return NULL;
    *out_len = len;
    return (uint8_t *)data;
}

/* ========================================================================
 * Golden vectors: somebody else's compressed bytes must yield our plaintext
 * ======================================================================== */

static void test_golden_inflate(void)
{
    printf("  golden inflate (python zlib / lz4.block vectors)\n");

    for (size_t i = 0; i < sizeof(kNames) / sizeof(kNames[0]); ++i) {
        const char *name = kNames[i];

        char plain_path[512];
        path_join(plain_path, sizeof(plain_path), name, ".bin");

        size_t plain_len = 0;
        uint8_t *plain = slurp(plain_path, &plain_len);
        if (plain == NULL) {
            printf("  SKIP %s (no golden data; run tools/ref_compress.py)\n", name);
            continue;
        }

        for (size_t c = 0; c < sizeof(kCodecs) / sizeof(kCodecs[0]); ++c) {
            char comp_path[512];
            path_join(comp_path, sizeof(comp_path), name, kCodecs[c].suffix);

            size_t comp_len = 0;
            uint8_t *comp = slurp(comp_path, &comp_len);
            if (comp == NULL) {
                printf("  SKIP %s%s (no golden data)\n", name, kCodecs[c].suffix);
                continue;
            }

            /* The codec must also be what we would sniff from these bytes.
               LZ4 is the exception: the block format has no header to sniff,
               so it is excluded from the detection check deliberately. */
            if (mcme_compression_detectable(kCodecs[c].codec)) {
                CHECK_MSG(mcme_compression_detect(comp, comp_len) == kCodecs[c].codec,
                          "%s%s sniffs as %s, expected %s",
                          name, kCodecs[c].suffix,
                          mcme_compression_name(mcme_compression_detect(comp, comp_len)),
                          mcme_compression_name(kCodecs[c].codec));
            }

            McmeBuffer out;
            mcme_buffer_init(&out);
            bool ok = mcme_decompress(kCodecs[c].codec, comp, comp_len,
                                      &out, MCME_DECOMPRESS_MAX);
            CHECK_MSG(ok, "%s%s failed to inflate: %s",
                      name, kCodecs[c].suffix, mcme_last_error());
            if (ok) {
                CHECK_MSG(out.len == plain_len,
                          "%s%s inflated to %zu bytes, expected %zu",
                          name, kCodecs[c].suffix, out.len, plain_len);
                CHECK_MSG(out.len == plain_len &&
                          memcmp(out.data, plain, plain_len) == 0,
                          "%s%s inflated content differs from the plaintext",
                          name, kCodecs[c].suffix);
            }
            mcme_buffer_free(&out);
            free(comp);
        }
        free(plain);
    }
}

/* ========================================================================
 * Round trip: our compressor's output must survive our decompressor
 * ======================================================================== */

static void test_round_trip(void)
{
    printf("  round trip\n");

    static const McmeCompression codecs[] = {
        MCME_COMPRESS_STORE, MCME_COMPRESS_GZIP,
        MCME_COMPRESS_ZLIB, MCME_COMPRESS_LZ4,
        MCME_COMPRESS_RAW_DEFLATE,
    };

    /* Small, hand-written payloads. The golden vectors cover realistic sizes;
       these cover the edge cases a generator is less likely to hit. */
    static const struct {
        const char *label;
        const uint8_t *data;
        size_t len;
    } cases[] = {
        { "empty",     (const uint8_t *)"", 0 },
        { "one-byte",  (const uint8_t *)"x", 1 },
        { "nul-only",  (const uint8_t *)"\0", 1 },
        { "nuls",      (const uint8_t *)"\0\0\0\0\1\0\0\0", 8 },
        { "all-ff",    (const uint8_t *)"\xff\xff\xff\xff\xff\xff\xff\xff", 8 },
    };

    /* A repetitive blob big enough to exercise the multi-chunk streaming
       loops in both directions. */
    static uint8_t blob[200000];
    for (size_t i = 0; i < sizeof(blob); ++i) {
        blob[i] = (uint8_t)((i / 7) & 0xff);
    }

    for (size_t k = 0; k < sizeof(cases) / sizeof(cases[0]) + 1; ++k) {
        const char *label;
        const uint8_t *data;
        size_t len;
        if (k < sizeof(cases) / sizeof(cases[0])) {
            label = cases[k].label;
            data = cases[k].data;
            len = cases[k].len;
        } else {
            label = "streaming-blob";
            data = blob;
            len = sizeof(blob);
        }

        for (size_t c = 0; c < sizeof(codecs) / sizeof(codecs[0]); ++c) {
            McmeCompression codec = codecs[c];

            McmeBuffer packed;
            mcme_buffer_init(&packed);
            bool ok = mcme_compress(codec, data, len, &packed);
            CHECK_MSG(ok, "%s/%s compress failed: %s",
                      label, mcme_compression_name(codec), mcme_last_error());

            if (ok) {
                McmeBuffer back;
                mcme_buffer_init(&back);
                ok = mcme_decompress(codec, packed.data, packed.len,
                                     &back, MCME_DECOMPRESS_MAX);
                CHECK_MSG(ok, "%s/%s decompress failed: %s",
                          label, mcme_compression_name(codec), mcme_last_error());
                if (ok) {
                    CHECK_MSG(back.len == len &&
                              (len == 0 || memcmp(back.data, data, len) == 0),
                              "%s/%s round trip mismatch (%zu vs %zu bytes)",
                              label, mcme_compression_name(codec), back.len, len);
                }
                mcme_buffer_free(&back);
            }
            mcme_buffer_free(&packed);
        }
    }
}

/* ========================================================================
 * Refusal: the decompressor must say no cleanly, and must not hang
 * ======================================================================== */

static void test_refusals(void)
{
    printf("  refusal and ceiling\n");

    /* Truncated gzip: half a valid stream. Must fail promptly, not spin. */
    {
        char path[512];
        path_join(path, sizeof(path), "text", ".gz");
        size_t len = 0;
        uint8_t *gz = slurp(path, &len);
        if (gz == NULL) {
            printf("  SKIP truncated-gzip (no golden data)\n");
        } else {
            McmeBuffer out;
            mcme_buffer_init(&out);
            CHECK(!mcme_decompress(MCME_COMPRESS_GZIP, gz, len / 2, &out,
                                   MCME_DECOMPRESS_MAX));
            mcme_buffer_free(&out);
            free(gz);
        }
    }

    /* Ceiling: one byte less than the real size must fail, and must not
       append a partial payload that a caller might mistake for success. */
    {
        char path[512];
        path_join(path, sizeof(path), "text", ".zlib");
        size_t clen = 0, plen = 0;
        uint8_t *zlib_bytes = slurp(path, &clen);
        char plain_path[512];
        path_join(plain_path, sizeof(plain_path), "text", ".bin");
        uint8_t *plain = slurp(plain_path, &plen);

        if (zlib_bytes == NULL || plain == NULL) {
            printf("  SKIP ceiling (no golden data)\n");
        } else {
            McmeBuffer out;
            mcme_buffer_init(&out);
            CHECK(!mcme_decompress(MCME_COMPRESS_ZLIB, zlib_bytes, clen,
                                   &out, plen - 1));
            mcme_buffer_free(&out);

            /* Exactly at the ceiling must succeed: the limit is inclusive. */
            mcme_buffer_init(&out);
            CHECK(mcme_decompress(MCME_COMPRESS_ZLIB, zlib_bytes, clen,
                                  &out, plen));
            CHECK(out.len == plen);
            mcme_buffer_free(&out);
        }
        free(zlib_bytes);
        free(plain);
    }

    /* Garbage that is not any known container. */
    {
        static const uint8_t junk[] = { 0xde, 0xad, 0xbe, 0xef, 0x00, 0x11 };
        CHECK(mcme_compression_detect(junk, sizeof(junk)) == MCME_COMPRESS_STORE);

        McmeBuffer out;
        mcme_buffer_init(&out);
        CHECK(!mcme_decompress_auto(junk, sizeof(junk), &out, MCME_DECOMPRESS_MAX));
        mcme_buffer_free(&out);
    }

    /* An empty input is legitimate for STORE and meaningless for everything
       else. A framing codec cannot produce a payload from nothing. */
    {
        McmeBuffer out;
        mcme_buffer_init(&out);
        CHECK(mcme_decompress(MCME_COMPRESS_STORE, NULL, 0, &out, 16));
        CHECK(out.len == 0);
        mcme_buffer_free(&out);

        mcme_buffer_init(&out);
        CHECK(!mcme_decompress(MCME_COMPRESS_GZIP, NULL, 0, &out, 16));
        mcme_buffer_free(&out);

        mcme_buffer_init(&out);
        CHECK(!mcme_decompress(MCME_COMPRESS_ZLIB, NULL, 0, &out, 16));
        mcme_buffer_free(&out);
    }

    /* NULL output buffer is a programming error, caught rather than crashed
       into. */
    CHECK(!mcme_decompress(MCME_COMPRESS_STORE, "x", 1, NULL, 16));
    CHECK(!mcme_compress(MCME_COMPRESS_STORE, "x", 1, NULL));

    /* Length given without data is also refused, not read as a NULL deref. */
    {
        McmeBuffer out;
        mcme_buffer_init(&out);
        CHECK(!mcme_decompress(MCME_COMPRESS_STORE, NULL, 4, &out, 16));
        mcme_buffer_free(&out);
    }

    /* A corrupt LZ4 block (right length, wrong contents) must fail rather
       than write out-of-bounds. */
    {
        static const uint8_t garbage[64] = { 0x11 };
        McmeBuffer out;
        mcme_buffer_init(&out);
        CHECK(!mcme_decompress(MCME_COMPRESS_LZ4, garbage, sizeof(garbage),
                               &out, 4096));
        mcme_buffer_free(&out);
    }
}

/* ========================================================================
 * Detection edge cases
 * ======================================================================== */

static void test_detect(void)
{
    printf("  detection\n");

    CHECK(mcme_compression_detect("\x1f\x8b\x08", 3) == MCME_COMPRESS_GZIP);
    /* 0x78 0x9c is the common zlib header (default compression). */
    CHECK(mcme_compression_detect("\x78\x9c", 2) == MCME_COMPRESS_ZLIB);
    /* 0x78 0x01 is zlib with no compression. */
    CHECK(mcme_compression_detect("\x78\x01", 2) == MCME_COMPRESS_ZLIB);
    /* 0x78 0xda is zlib at level 9. */
    CHECK(mcme_compression_detect("\x78\xda", 2) == MCME_COMPRESS_ZLIB);

    /* CMF/FLG must be a multiple of 31, so this is not zlib. */
    CHECK(mcme_compression_detect("\x78\x9d", 2) == MCME_COMPRESS_STORE);
    /* CM must be 8 (deflate). */
    CHECK(mcme_compression_detect("\x68\x9c", 2) == MCME_COMPRESS_STORE);
    /* FDICT set means a preset dictionary, which we do not support. */
    CHECK(mcme_compression_detect("\x78\xbc", 2) == MCME_COMPRESS_STORE);

    /* Too short to decide: neither gzip nor zlib can be sniffed from 1 byte. */
    CHECK(mcme_compression_detect("\x1f", 1) == MCME_COMPRESS_STORE);
    CHECK(mcme_compression_detect("", 0) == MCME_COMPRESS_STORE);
    CHECK(mcme_compression_detect(NULL, 0) == MCME_COMPRESS_STORE);

    /* LZ4 block format has no magic at all, so it is never auto-detected. */
    CHECK(!mcme_compression_detectable(MCME_COMPRESS_LZ4));
    CHECK(mcme_compression_detectable(MCME_COMPRESS_GZIP));
    CHECK(mcme_compression_detectable(MCME_COMPRESS_ZLIB));

    /* Bound must never under-report: compressing into exactly that many bytes
       must succeed. */
    static const uint8_t payload[4096] = { 1, 2, 3 };
    static const McmeCompression codecs[] = {
        MCME_COMPRESS_STORE, MCME_COMPRESS_GZIP,
        MCME_COMPRESS_ZLIB, MCME_COMPRESS_LZ4,
        MCME_COMPRESS_RAW_DEFLATE,
    };
    for (size_t c = 0; c < sizeof(codecs) / sizeof(codecs[0]); ++c) {
        size_t bound = mcme_compress_bound(codecs[c], sizeof(payload));
        CHECK_MSG(bound >= sizeof(payload),
                  "%s bound %zu is smaller than the input",
                  mcme_compression_name(codecs[c]), bound);
        if (bound == 0) continue;

        uint8_t *exact = (uint8_t *)malloc(bound);
        CHECK(exact != NULL);
        if (exact == NULL) continue;

        /* Compress straight into a buffer we then pretend has exactly `bound`
           capacity, by checking the writer did not exceed it. */
        McmeBuffer out;
        mcme_buffer_init(&out);
        bool ok = mcme_compress(codecs[c], payload, sizeof(payload), &out);
        CHECK(ok);
        CHECK_MSG(!ok || out.len <= bound,
                  "%s wrote %zu bytes, over its own bound of %zu",
                  mcme_compression_name(codecs[c]), out.len, bound);
        mcme_buffer_free(&out);
        free(exact);
    }
}

int mcme_test_compress(void)
{
    g_failed = 0;
    printf("compress\n");

    if (!mcme_fs_exists(kGoldenDir)) {
        printf("  SKIP golden vectors (%s not found; run tools/ref_compress.py)\n",
               kGoldenDir);
    } else {
        test_golden_inflate();
    }

    test_round_trip();
    test_refusals();
    test_detect();

    return g_failed;
}
