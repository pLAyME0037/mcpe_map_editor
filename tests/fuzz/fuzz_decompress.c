/*
 * libFuzzer harness for the compression layer.
 *
 * Compression is the first thing mcme touches when it opens an untrusted world
 * file: every Anvil chunk payload and both editions' level.dat arrive
 * compressed. A decompressor is exactly the kind of code that a malformed or
 * hostile input breaks in ways sanitizers catch and users do not forgive.
 *
 * The invariants asserted:
 *
 *   1. Never crash, never hang, never exceed the caller's ceiling. The ceiling
 *      is derived from the input size so the fuzzer reaches the over-limit path
 *      quickly instead of always succeeding or always being truncated.
 *   2. If decompression succeeds, the result round-trips: recompressing it with
 *      the same codec and decompressing again must yield identical bytes. This
 *      is the same idempotence argument the NBT fuzzers use — a decompressor
 *      that "succeeds" on garbage by emitting nonsense is a real failure mode,
 *      and mere survival would not catch it.
 *
 * Input layout: byte 0 selects the codec, the rest is the payload.
 */

#include "mcme/compress.h"
#include "mcme/core.h"

#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

static const McmeCompression kCodecs[] = {
    MCME_COMPRESS_STORE,
    MCME_COMPRESS_GZIP,
    MCME_COMPRESS_ZLIB,
    MCME_COMPRESS_LZ4,
};

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    if (size < 1) return 0;

    McmeCompression codec = kCodecs[data[0] % (sizeof(kCodecs) / sizeof(kCodecs[0]))];
    const uint8_t *payload = data + 1;
    size_t payload_len = size - 1;

    /* Deliberately small, and proportional to the input: large enough that a
       well-formed stream can succeed, small enough that the ceiling check is
       reachable and that the LZ4 path (which allocates its whole destination
       up front) cannot be steered into a huge allocation. */
    size_t max_out = 64u * 1024u;
    if (payload_len * 8u < max_out) max_out = payload_len * 8u + 1024u;
    if (max_out < 64u) max_out = 64u;

    McmeBuffer out;
    mcme_buffer_init(&out);
    bool ok = mcme_decompress(codec, payload, payload_len, &out, max_out);

    /* Invariant 1: whatever happened, we respected the ceiling. */
    if (out.len > max_out) abort();

    if (ok) {
        /* Invariant 2: the output must be stable under a recompress cycle. */
        McmeBuffer again;
        mcme_buffer_init(&again);
        if (mcme_compress(codec, out.data, out.len, &again)) {
            McmeBuffer back;
            mcme_buffer_init(&back);
            if (mcme_decompress(codec, again.data, again.len, &back, max_out)) {
                if (back.len != out.len) abort();
                if (out.len > 0 && memcmp(back.data, out.data, out.len) != 0) abort();
            }
            mcme_buffer_free(&back);
        }
        mcme_buffer_free(&again);
    }

    mcme_buffer_free(&out);
    return 0;
}
