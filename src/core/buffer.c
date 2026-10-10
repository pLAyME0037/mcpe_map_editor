#include "mcme/core.h"

#include <stdlib.h>
#include <string.h>

/*
 * Growable byte buffer.
 *
 * Every serializer in mcme writes into one of these. The growth policy is
 * simple doubling, which keeps append amortised O(1); the buffer is not
 * shrunken, because in practice a serializer's buffer is taken and handed off
 * rather than reused across very different sizes.
 *
 * Floats are written by reinterpreting their IEEE-754 bit pattern as an
 * integer, not by arithmetic. That preserves NaN payloads and the sign of
 * negative zero, both of which round-trip through NBT and must not be
 * normalised away.
 */

void mcme_buffer_init(McmeBuffer *b)
{
    b->data = NULL;
    b->len = 0;
    b->cap = 0;
}

void mcme_buffer_free(McmeBuffer *b)
{
    if (!b) return;
    free(b->data);
    b->data = NULL;
    b->len = 0;
    b->cap = 0;
}

bool mcme_buffer_reserve(McmeBuffer *b, size_t extra)
{
    if (extra > (size_t)-1 - b->len) {
        mcme_set_error("buffer: size overflow");
        return false;
    }
    size_t need = b->len + extra;
    if (need <= b->cap) return true;

    size_t cap = b->cap ? b->cap : 64;
    while (cap < need) {
        if (cap > ((size_t)-1) / 2) { cap = need; break; }
        cap *= 2;
    }

    uint8_t *grown = realloc(b->data, cap);
    if (!grown) {
        mcme_set_error("buffer: out of memory (%zu bytes)", cap);
        return false;
    }
    b->data = grown;
    b->cap = cap;
    return true;
}

bool mcme_buffer_append(McmeBuffer *b, const void *data, size_t len)
{
    if (len == 0) return true;
    if (!mcme_buffer_reserve(b, len)) return false;
    memcpy(b->data + b->len, data, len);
    b->len += len;
    return true;
}

bool mcme_buffer_append_u8(McmeBuffer *b, uint8_t v)
{
    return mcme_buffer_append(b, &v, 1);
}

bool mcme_buffer_append_char(McmeBuffer *b, char c)
{
    return mcme_buffer_append_u8(b, (uint8_t)c);
}

bool mcme_buffer_append_cstr(McmeBuffer *b, const char *s)
{
    return mcme_buffer_append(b, s, strlen(s));
}

/* ------------------------------------------------------------------
 * Endian-specific numeric appends.
 *
 * Each is written byte-by-byte rather than via a cast, so the result does not
 * depend on the host's byte order. NBT files are always a fixed endianness
 * regardless of the machine that produced them.
 * ------------------------------------------------------------------ */

bool mcme_buffer_append_u16be(McmeBuffer *b, uint16_t v)
{
    uint8_t bytes[2] = { (uint8_t)(v >> 8), (uint8_t)v };
    return mcme_buffer_append(b, bytes, 2);
}

bool mcme_buffer_append_u16le(McmeBuffer *b, uint16_t v)
{
    uint8_t bytes[2] = { (uint8_t)v, (uint8_t)(v >> 8) };
    return mcme_buffer_append(b, bytes, 2);
}

bool mcme_buffer_append_i32be(McmeBuffer *b, int32_t v)
{
    uint32_t u = (uint32_t)v;
    uint8_t bytes[4] = {
        (uint8_t)(u >> 24), (uint8_t)(u >> 16), (uint8_t)(u >> 8), (uint8_t)u
    };
    return mcme_buffer_append(b, bytes, 4);
}

bool mcme_buffer_append_i32le(McmeBuffer *b, int32_t v)
{
    uint32_t u = (uint32_t)v;
    uint8_t bytes[4] = {
        (uint8_t)u, (uint8_t)(u >> 8), (uint8_t)(u >> 16), (uint8_t)(u >> 24)
    };
    return mcme_buffer_append(b, bytes, 4);
}

bool mcme_buffer_append_i64be(McmeBuffer *b, int64_t v)
{
    uint64_t u = (uint64_t)v;
    uint8_t bytes[8];
    for (int i = 0; i < 8; ++i) bytes[i] = (uint8_t)(u >> (56 - 8 * i));
    return mcme_buffer_append(b, bytes, 8);
}

bool mcme_buffer_append_i64le(McmeBuffer *b, int64_t v)
{
    uint64_t u = (uint64_t)v;
    uint8_t bytes[8];
    for (int i = 0; i < 8; ++i) bytes[i] = (uint8_t)(u >> (8 * i));
    return mcme_buffer_append(b, bytes, 8);
}

/* Reinterpret a float's bits as int32 without violating strict aliasing. */
static int32_t f32_bits(float v)
{
    int32_t bits;
    memcpy(&bits, &v, sizeof(bits));
    return bits;
}

static int64_t f64_bits(double v)
{
    int64_t bits;
    memcpy(&bits, &v, sizeof(bits));
    return bits;
}

bool mcme_buffer_append_f32be(McmeBuffer *b, float v)
{
    return mcme_buffer_append_i32be(b, f32_bits(v));
}

bool mcme_buffer_append_f32le(McmeBuffer *b, float v)
{
    return mcme_buffer_append_i32le(b, f32_bits(v));
}

bool mcme_buffer_append_f64be(McmeBuffer *b, double v)
{
    return mcme_buffer_append_i64be(b, f64_bits(v));
}

bool mcme_buffer_append_f64le(McmeBuffer *b, double v)
{
    return mcme_buffer_append_i64le(b, f64_bits(v));
}

uint8_t *mcme_buffer_take(McmeBuffer *b, size_t *out_len)
{
    uint8_t *data = b->data;
    if (out_len) *out_len = b->len;
    b->data = NULL;
    b->len = 0;
    b->cap = 0;
    return data;
}
