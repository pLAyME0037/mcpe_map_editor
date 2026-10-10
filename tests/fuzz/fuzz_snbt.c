/*
 * Fuzz target: SNBT parser.
 *
 * Build & run:
 *   ./rebuild.sh fuzz
 *   ./build/fuzz_snbt -max_total_time=60
 *
 * Same idempotence invariant as the binary target, applied to text:
 *
 *     from_snbt(X) -> to_snbt -> Y
 *     from_snbt(Y) -> to_snbt -> Z        assert Y == Z
 *
 * X itself need not round-trip (the reference sorts compound keys on output,
 * and may accept spellings it would not emit), but once we have produced Y it
 * must be a fixed point. A violation means our parser accepts text our printer
 * does not generate, or vice versa.
 *
 * Non-ASCII bytes are expected and common: the reference writes raw UTF-8, and
 * even raw NUL/newline/tab inside string literals. Those must parse.
 */

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mcme/core.h"
#include "mcme/nbt.h"

static void fail(const char *msg, const char *detail)
{
    fprintf(stderr, "FUZZ INVARIANT VIOLATED: %s (%s)\n", msg, detail);
    abort();
}

static void roundtrip(const uint8_t *data, size_t size)
{
    McmeArena *a = mcme_arena_new();
    if (!a) return;

    McmeTag *first = NULL;
    if (!mcme_nbt_parse_snbt(a, (const char *)data, size, &first)) {
        mcme_clear_error();
        mcme_arena_free(a);
        return;
    }

    McmeBuffer y;
    mcme_buffer_init(&y);
    if (!mcme_nbt_write_snbt(first, &y, 0)) {
        fail("parsed SNBT could not be re-serialized", mcme_last_error());
    }

    /* Anything we print must also be readable as binary, since that is how it
       gets saved to a world. */
    {
        McmeArena *ab = mcme_arena_new();
        McmeNbtDoc doc;
        doc.arena = ab;
        doc.root = first;
        doc.root_name.data = (char *)"";
        doc.root_name.len = 0;
        doc.little_endian = false;

        McmeBuffer bin;
        mcme_buffer_init(&bin);
        if (!mcme_nbt_write_binary(&doc, &bin)) {
            fail("SNBT that parsed could not be written as binary",
                 mcme_last_error());
        }

        McmeArena *ab2 = mcme_arena_new();
        McmeNbtDoc back;
        if (!ab2 || !mcme_nbt_parse(&back, ab2, bin.data, bin.len, false)) {
            fail("our own binary output failed to parse", mcme_last_error());
        }
        if (ab2) mcme_arena_free(ab2);

        mcme_buffer_free(&bin);
        mcme_arena_free(ab);
    }

    McmeArena *a2 = mcme_arena_new();
    McmeTag *second = NULL;
    if (!a2 || !mcme_nbt_parse_snbt(a2, (const char *)y.data, y.len, &second)) {
        fail("our own SNBT output failed to parse", mcme_last_error());
    }

    McmeBuffer z;
    mcme_buffer_init(&z);
    if (!mcme_nbt_write_snbt(second, &z, 0)) {
        fail("re-parsed SNBT could not be re-serialized", mcme_last_error());
    }

    if (z.len != y.len || (z.len > 0 && memcmp(z.data, y.data, z.len) != 0)) {
        fail("SNBT serialization is not idempotent", "Y != Z");
    }

    mcme_buffer_free(&z);
    mcme_arena_free(a2);
    mcme_buffer_free(&y);
    mcme_arena_free(a);
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    roundtrip(data, size);
    return 0;
}
