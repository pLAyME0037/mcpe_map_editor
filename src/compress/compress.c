/*
 * mcme compression layer. See include/mcme/compress.h for the contract.
 *
 * Written to be boring: a streaming zlib loop and two calls into LZ4. The
 * interesting parts are the ceiling checks, because every input here is
 * attacker-controllable (see the safety note in the header).
 */

#include "mcme/compress.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

#include <zlib.h>

#include "lz4.h"

/* ========================================================================
 * Names and detection
 * ======================================================================== */

const char *mcme_compression_name(McmeCompression c)
{
    switch (c) {
    case MCME_COMPRESS_STORE:   return "store";
    case MCME_COMPRESS_GZIP:    return "gzip";
    case MCME_COMPRESS_ZLIB:    return "zlib";
    case MCME_COMPRESS_LZ4:     return "lz4";
    case MCME_COMPRESS_UNKNOWN: return "unknown";
    case MCME_COMPRESS_RAW_DEFLATE: return "deflate";
    }
    return "unknown";
}

McmeCompression mcme_compression_detect(const void *data, size_t len)
{
    const uint8_t *p = (const uint8_t *)data;
    if (p == NULL || len < 2) return MCME_COMPRESS_STORE;

    /* gzip: RFC 1952 magic. */
    if (p[0] == 0x1f && p[1] == 0x8b) return MCME_COMPRESS_GZIP;

    /* zlib: RFC 1950. CM must be deflate (low nibble 8), and CMF*256+FLG must
       be a multiple of 31. The window-size bits are checked too, since a
       nonzero reserved bit is the classic "not really zlib" tell. */
    unsigned cmf = p[0], flg = p[1];
    if ((cmf & 0x0f) == 8 &&
        (cmf >> 4) <= 7 &&
        ((cmf << 8) | flg) % 31 == 0 &&
        (flg & 0x20) == 0) {
        return MCME_COMPRESS_ZLIB;
    }

    return MCME_COMPRESS_STORE;
}

bool mcme_compression_detectable(McmeCompression c)
{
    return c == MCME_COMPRESS_GZIP || c == MCME_COMPRESS_ZLIB;
}

/* ========================================================================
 * Decompression
 * ======================================================================== */

/* Streaming inflate. `window_bits` selects the framing: 31 for gzip (15 + 16),
   15 for raw zlib, -15 for bare DEFLATE with no header at all. */
static bool inflate_stream(int window_bits,
                           const uint8_t *src, size_t src_len,
                           McmeBuffer *out, size_t max_out)
{
    enum { CHUNK = 64 * 1024 };

    z_stream zs;
    memset(&zs, 0, sizeof zs);

    int zr = inflateInit2(&zs, window_bits);
    if (zr != Z_OK) {
        mcme_set_error("zlib: inflateInit2 failed (%d)", zr);
        return false;
    }

    uint8_t *scratch = (uint8_t *)malloc(CHUNK);
    if (scratch == NULL) {
        inflateEnd(&zs);
        mcme_set_error("out of memory allocating %d bytes", (int)CHUNK);
        return false;
    }

    bool ok = false;
    size_t in_off = 0;
    size_t total = 0;

    for (;;) {
        /* Refill the input when zlib has consumed everything we gave it.
           The guard on `src` is belt-and-braces: mcme_decompress already
           rejects a NULL src with a nonzero length, so src_len > 0 here implies
           src != NULL, but computing a pointer from NULL would be undefined
           behaviour if that ever stopped being true. */
        if (zs.avail_in == 0 && in_off < src_len) {
            size_t remaining = src_len - in_off;
            if (remaining > (size_t)UINT_MAX) remaining = (size_t)UINT_MAX;
            zs.next_in = (Bytef *)(src != NULL ? src + in_off : (const uint8_t *)"");
            zs.avail_in = (uInt)remaining;
            in_off += remaining;
        }

        zs.next_out = scratch;
        zs.avail_out = (uInt)CHUNK;

        zr = inflate(&zs, Z_NO_FLUSH);

        size_t produced = (size_t)CHUNK - (size_t)zs.avail_out;
        if (produced > 0) {
            /* Check the ceiling BEFORE appending: appending first would let a
               single oversized chunk push us past max_out before we notice. */
            if (total + produced > max_out) {
                mcme_set_error("decompressed %zu bytes, over the %zu limit",
                               total + produced, max_out);
                break;
            }
            if (!mcme_buffer_append(out, scratch, produced)) {
                break; /* mcme_buffer_append already set the error */
            }
            total += produced;
        }

        if (zr == Z_STREAM_END) {
            ok = true;
            break;
        }
        if (zr != Z_OK) {
            mcme_set_error("zlib: inflate failed (%d: %s)", zr, zs.msg ? zs.msg : "");
            break;
        }
        /* Z_OK with no progress and no input left means the stream was cut
           short. Without this check we would spin forever on truncated input. */
        if (produced == 0 && zs.avail_in == 0 && in_off >= src_len) {
            mcme_set_error("compressed stream truncated (%zu bytes consumed)", total);
            break;
        }
    }

    free(scratch);
    inflateEnd(&zs);
    return ok;
}

bool mcme_decompress(McmeCompression c,
                     const void *src, size_t src_len,
                     McmeBuffer *out, size_t max_out)
{
    if (out == NULL) {
        mcme_set_error("decompress: output buffer is NULL");
        return false;
    }
    if (src == NULL && src_len != 0) {
        mcme_set_error("decompress: input is NULL but length is %zu", src_len);
        return false;
    }

    switch (c) {
    case MCME_COMPRESS_STORE:
        if (src_len > max_out) {
            mcme_set_error("stored payload is %zu bytes, over the %zu limit",
                           src_len, max_out);
            return false;
        }
        return mcme_buffer_append(out, src, src_len);

    case MCME_COMPRESS_GZIP:
        return inflate_stream(15 + 16, (const uint8_t *)src, src_len, out, max_out);

    case MCME_COMPRESS_ZLIB:
        return inflate_stream(15, (const uint8_t *)src, src_len, out, max_out);

    case MCME_COMPRESS_RAW_DEFLATE:
        /* No zlib header, no gzip header, no trailing checksum — just DEFLATE.
           -15 is what tells zlib to expect exactly that. Nothing in the format
           states the output size either, so the output grows lazily rather
           than being sized up front. */
        return inflate_stream(-15, (const uint8_t *)src, src_len, out, max_out);

    case MCME_COMPRESS_LZ4: {
        /* The LZ4 *block* format carries no size header, so the only way to
           decompress is to be told the destination capacity up front. That
           means we must allocate the whole ceiling here rather than growing
           lazily like the zlib path does — callers should pass a tight bound
           for LZ4 for exactly this reason. */
        size_t capacity = max_out;
        if (capacity > (size_t)INT_MAX) capacity = (size_t)INT_MAX;
        if (src_len > (size_t)INT_MAX) {
            mcme_set_error("lz4: input of %zu bytes exceeds the format limit", src_len);
            return false;
        }
        if (capacity == 0) {
            mcme_set_error("lz4: destination capacity is zero");
            return false;
        }

        char *dst = (char *)malloc(capacity);
        if (dst == NULL) {
            mcme_set_error("out of memory allocating %zu bytes for lz4", capacity);
            return false;
        }

        int n = LZ4_decompress_safe((const char *)src, dst,
                                    (int)src_len, (int)capacity);
        if (n < 0) {
            free(dst);
            mcme_set_error("lz4: decompression failed (%d)", n);
            return false;
        }

        bool ok = mcme_buffer_append(out, dst, (size_t)n);
        free(dst);
        return ok;
    }

    case MCME_COMPRESS_UNKNOWN:
        break;
    }

    mcme_set_error("decompress: unknown codec %d", (int)c);
    return false;
}

bool mcme_decompress_default(McmeCompression c,
                             const void *src, size_t src_len,
                             McmeBuffer *out)
{
    return mcme_decompress(c, src, src_len, out, MCME_DECOMPRESS_MAX);
}

bool mcme_decompress_auto(const void *src, size_t src_len,
                          McmeBuffer *out, size_t max_out)
{
    McmeCompression c = mcme_compression_detect(src, src_len);

    if (c == MCME_COMPRESS_STORE) {
        /* Refusing is deliberate. Silently passing the bytes through would
           mean a caller who asked for "the decompressed form" quietly gets
           possibly-compressed bytes instead. */
        mcme_set_error("decompress: no recognised container header in the first bytes");
        return false;
    }
    if (c == MCME_COMPRESS_UNKNOWN) {
        mcme_set_error("decompress: unrecognised container");
        return false;
    }
    return mcme_decompress(c, src, src_len, out, max_out);
}

/* ========================================================================
 * Compression
 * ======================================================================== */

static bool deflate_stream(int window_bits,
                           const uint8_t *src, size_t src_len,
                           McmeBuffer *out)
{
    enum { CHUNK = 64 * 1024 };

    z_stream zs;
    memset(&zs, 0, sizeof zs);

    int zr = deflateInit2(&zs, Z_DEFAULT_COMPRESSION, Z_DEFLATED,
                          window_bits, 8, Z_DEFAULT_STRATEGY);
    if (zr != Z_OK) {
        mcme_set_error("zlib: deflateInit2 failed (%d)", zr);
        return false;
    }

    uint8_t *scratch = (uint8_t *)malloc(CHUNK);
    if (scratch == NULL) {
        deflateEnd(&zs);
        mcme_set_error("out of memory allocating %d bytes", (int)CHUNK);
        return false;
    }

    bool ok = false;
    size_t in_off = 0;

    for (;;) {
        if (zs.avail_in == 0 && in_off < src_len) {
            size_t remaining = src_len - in_off;
            if (remaining > (size_t)UINT_MAX) remaining = (size_t)UINT_MAX;
            zs.next_in = (Bytef *)(src != NULL ? src + in_off : (const uint8_t *)"");
            zs.avail_in = (uInt)remaining;
            in_off += remaining;
        }

        int flush = (in_off >= src_len) ? Z_FINISH : Z_NO_FLUSH;

        zs.next_out = scratch;
        zs.avail_out = (uInt)CHUNK;

        zr = deflate(&zs, flush);
        if (zr != Z_OK && zr != Z_STREAM_END && zr != Z_BUF_ERROR) {
            mcme_set_error("zlib: deflate failed (%d: %s)", zr, zs.msg ? zs.msg : "");
            break;
        }

        size_t produced = (size_t)CHUNK - (size_t)zs.avail_out;
        if (produced > 0 && !mcme_buffer_append(out, scratch, produced)) {
            break;
        }

        if (zr == Z_STREAM_END) {
            ok = true;
            break;
        }
        if (zr == Z_BUF_ERROR && produced == 0 && zs.avail_in == 0 && in_off >= src_len) {
            mcme_set_error("zlib: deflate stalled");
            break;
        }
    }

    free(scratch);
    deflateEnd(&zs);
    return ok;
}

size_t mcme_compress_bound(McmeCompression c, size_t src_len)
{
    switch (c) {
    case MCME_COMPRESS_STORE:
        return src_len;
    case MCME_COMPRESS_GZIP:
    case MCME_COMPRESS_ZLIB:
    case MCME_COMPRESS_RAW_DEFLATE:
        /* compressBound covers the DEFLATE stream; a few dozen more bytes
           cover the gzip or zlib container header and checksum. Bare DEFLATE
           has neither, but the slack is harmless and keeps one bound. */
        return (size_t)compressBound((uLong)src_len) + 64u;
    case MCME_COMPRESS_LZ4: {
        if (src_len > (size_t)INT_MAX) return 0;
        int n = LZ4_compressBound((int)src_len);
        return n > 0 ? (size_t)n : 0;
    }
    case MCME_COMPRESS_UNKNOWN:
        break;
    }
    return 0;
}

bool mcme_compress(McmeCompression c,
                   const void *src, size_t src_len,
                   McmeBuffer *out)
{
    if (out == NULL) {
        mcme_set_error("compress: output buffer is NULL");
        return false;
    }
    if (src == NULL && src_len != 0) {
        mcme_set_error("compress: input is NULL but length is %zu", src_len);
        return false;
    }

    switch (c) {
    case MCME_COMPRESS_STORE:
        return mcme_buffer_append(out, src, src_len);

    case MCME_COMPRESS_GZIP:
        return deflate_stream(15 + 16, (const uint8_t *)src, src_len, out);

    case MCME_COMPRESS_ZLIB:
        return deflate_stream(15, (const uint8_t *)src, src_len, out);

    case MCME_COMPRESS_RAW_DEFLATE:
        return deflate_stream(-15, (const uint8_t *)src, src_len, out);

    case MCME_COMPRESS_LZ4: {
        if (src_len > (size_t)INT_MAX) {
            mcme_set_error("lz4: input of %zu bytes exceeds the format limit", src_len);
            return false;
        }
        size_t cap = mcme_compress_bound(MCME_COMPRESS_LZ4, src_len);
        if (cap == 0) {
            mcme_set_error("lz4: no bound for %zu bytes", src_len);
            return false;
        }

        char *dst = (char *)malloc(cap);
        if (dst == NULL) {
            mcme_set_error("out of memory allocating %zu bytes for lz4", cap);
            return false;
        }

        int n = LZ4_compress_default((const char *)src, dst, (int)src_len, (int)cap);
        bool ok;
        if (n <= 0) {
            mcme_set_error("lz4: compression failed (%d)", n);
            ok = false;
        } else {
            ok = mcme_buffer_append(out, dst, (size_t)n);
        }
        free(dst);
        return ok;
    }

    case MCME_COMPRESS_UNKNOWN:
        break;
    }

    mcme_set_error("compress: unknown codec %d", (int)c);
    return false;
}
