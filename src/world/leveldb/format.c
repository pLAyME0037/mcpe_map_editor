/*
 * LevelDB format primitives: varints, CRC32C, internal keys, file helpers.
 * See internal.h for what each is for.
 */

#include "internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

/* ========================================================================
 * Varints
 * ======================================================================== */

size_t mcme_ldb_varint_get(const uint8_t *p, const uint8_t *end, uint64_t *out)
{
    const uint8_t *start = p;
    uint64_t value = 0;

    /* A varint is at most 10 bytes; anything longer is corrupt, and bailing
       rather than reading on is the point. */
    for (size_t shift = 0; shift < 70; shift += 7) {
        if (p >= end) return 0;                  /* truncated */
        if (shift > 63) return 0;                /* would overflow 64 bits */

        uint8_t byte = *p++;
        value |= (uint64_t)(byte & 0x7f) << shift;

        if ((byte & 0x80) == 0) {
            *out = value;
            return (size_t)(p - start);
        }
    }
    return 0;                                    /* more than 10 bytes */
}

bool mcme_ldb_varint_put(McmeBuffer *b, uint64_t v)
{
    uint8_t buf[10];
    size_t n = mcme_ldb_varint_encode(v, buf, sizeof buf);
    if (n == 0) return false;
    return mcme_buffer_append(b, buf, n);
}

size_t mcme_ldb_varint_encode(uint64_t v, uint8_t *out, size_t cap)
{
    size_t n = 0;
    do {
        if (n >= cap || n >= 10) return 0;
        uint8_t byte = (uint8_t)(v & 0x7f);
        v >>= 7;
        if (v != 0) byte |= 0x80;
        out[n++] = byte;
    } while (v != 0);
    return n;
}

/* ========================================================================
 * CRC32C
 *
 * Reflected, table-driven. The table is built once on first use rather than
 * written out as a 256-entry literal: generating it is eight lines and makes
 * the polynomial visible instead of buried in 256 magic numbers.
 * ======================================================================== */

static uint32_t g_crc32c_table[256];
static bool     g_crc32c_ready = false;

static void crc32c_init(void)
{
    /* 0x82f63b78 is the reflected form of the Castagnoli polynomial
       0x1edc6f41. */
    const uint32_t poly = 0x82f63b78u;
    for (uint32_t i = 0; i < 256; ++i) {
        uint32_t crc = i;
        for (int bit = 0; bit < 8; ++bit) {
            crc = (crc & 1u) ? (crc >> 1) ^ poly : (crc >> 1);
        }
        g_crc32c_table[i] = crc;
    }
    g_crc32c_ready = true;
}

uint32_t mcme_ldb_crc32c(uint32_t seed, const void *data, size_t len)
{
    if (!g_crc32c_ready) crc32c_init();

    const uint8_t *p = (const uint8_t *)data;
    uint32_t crc = ~seed;
    for (size_t i = 0; i < len; ++i) {
        crc = g_crc32c_table[(crc ^ p[i]) & 0xffu] ^ (crc >> 8);
    }
    return ~crc;
}

uint32_t mcme_ldb_mask_crc(uint32_t crc)
{
    /* LevelDB's Mask(): rotate right by 15, then add a constant with all the
       low bits set, so a run of zero bytes cannot produce a checksum of zero.

       The rotation is 15, not 17, and getting it backwards is invisible on
       hand-written test data built with the same wrong function — it only
       shows up against a store LevelDB actually wrote. The constant and the
       direction are one round trip away from each other, so the self-test
       inverts the mask and checks it lands back on the original. */
    return ((crc >> 15) | (crc << 17)) + 0xa282ead8u;
}

/* ========================================================================
 * Internal keys
 * ======================================================================== */

/* Reads the 8-byte tag at the end of an internal key. A key shorter than 8
   bytes cannot carry one, so it sorts before everything else that is well
   formed — which keeps a corrupt key from reading out of bounds. */
static uint64_t tag_of(const uint8_t *p, size_t len)
{
    if (len < 8) return 0;
    const uint8_t *t = p + len - 8;
    return ((uint64_t)t[0]) | ((uint64_t)t[1] << 8) | ((uint64_t)t[2] << 16) |
           ((uint64_t)t[3] << 24) | ((uint64_t)t[4] << 32) | ((uint64_t)t[5] << 40) |
           ((uint64_t)t[6] << 48) | ((uint64_t)t[7] << 56);
}

static size_t user_len_of(size_t len)
{
    return (len >= 8) ? len - 8 : len;
}

int mcme_ldb_user_cmp(const void *a, size_t a_len, const void *b, size_t b_len)
{
    size_t ua = user_len_of(a_len), ub = user_len_of(b_len);
    size_t n = ua < ub ? ua : ub;
    int r = (n > 0) ? memcmp(a, b, n) : 0;
    if (r != 0) return r;
    if (ua == ub) return 0;
    return (ua < ub) ? -1 : 1;
}

int mcme_ldb_internal_cmp(const void *a, size_t a_len, const void *b, size_t b_len)
{
    int r = mcme_ldb_user_cmp(a, a_len, b, b_len);
    if (r != 0) return r;

    /* Same user key: higher sequence sorts FIRST, so a merge that reads in
       order naturally sees the newest copy before any older one. */
    uint64_t ta = tag_of((const uint8_t *)a, a_len);
    uint64_t tb = tag_of((const uint8_t *)b, b_len);
    uint64_t sa = ta >> 8, sb = tb >> 8;
    if (sa != sb) return (sa > sb) ? -1 : 1;

    /* Same sequence: the type breaks the tie, deletion before value, which is
       LevelDB's own rule. */
    uint8_t ya = (uint8_t)(ta & 0xff), yb = (uint8_t)(tb & 0xff);
    if (ya != yb) return (ya < yb) ? -1 : 1;
    return 0;
}

bool mcme_ldb_put_internal_key(McmeBuffer *b, const void *user_key,
                               size_t user_len, uint64_t seq, uint8_t type)
{
    if (!mcme_buffer_append(b, user_key, user_len)) return false;
    uint64_t tag = (seq << 8) | (uint64_t)type;
    uint8_t tail[8];
    for (int i = 0; i < 8; ++i) tail[i] = (uint8_t)((tag >> (i * 8)) & 0xff);
    return mcme_buffer_append(b, tail, sizeof tail);
}

/* ========================================================================
 * File helpers
 * ======================================================================== */

bool mcme_ldb_read_file(const char *path, uint8_t **out, size_t *out_len,
                        size_t max)
{
    void *data = NULL;
    size_t len = 0;
    if (!mcme_fs_read(path, &data, &len)) return false;

    if (len > max) {
        mcme_set_error("leveldb: %s is %zu bytes, over the %zu limit",
                       path, len, max);
        free(data);
        return false;
    }

    *out = (uint8_t *)data;
    *out_len = len;
    return true;
}

const char *mcme_ldb_path(char *buf, size_t cap, const char *dir,
                          const char *name)
{
    int n = snprintf(buf, cap, "%s/%s", dir, name);
    if (n < 0 || (size_t)n >= cap) return NULL;
    return buf;
}

bool mcme_ldb_file_number(const char *name, uint64_t *out)
{
    /* Names are zero-padded digits then a suffix: "000007.log",
       "MANIFEST-000006". Find where the leading digits stop and require the
       rest to be all digits too. */
    const char *digits = name;
    while (*digits >= '0' && *digits <= '9') ++digits;

    const char *end = digits;
    while (*end != '\0') ++end;
    while (end > digits && end[-1] >= '0' && end[-1] <= '9') --end;

    if (end == digits) return false;

    uint64_t v = 0;
    for (const char *p = digits; p < end; ++p) {
        if (*p < '0' || *p > '9') return false;
        v = v * 10u + (uint64_t)(*p - '0');
    }
    *out = v;
    return true;
}
