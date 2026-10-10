/*
 * Fuzz target: binary NBT parser.
 *
 * Build & run:
 *   ./rebuild.sh fuzz
 *   ./build/fuzz_nbt -max_total_time=60
 *
 * The invariant checked is not "it doesn't crash" (ASan does that) but
 * "serialization is idempotent":
 *
 *     parse(X) -> write -> Y
 *     parse(Y) -> write -> Z        assert Y == Z
 *
 * A violation means the parser and writer disagree about the format: something
 * is read but not written back, or written in a way that reads back differently.
 * Both are silent-corruption bugs in an editor that must save what it loaded.
 *
 * Note the assertion is Y == Z, NOT X == Y. X may legitimately not round-trip:
 * a hand-written file can use a non-canonical spelling (a bare 0x00 for U+0000,
 * say) that our writer normalises. What must hold is that once we have written
 * a file, re-reading and re-writing it is stable.
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

static void roundtrip(const uint8_t *data, size_t size, bool le)
{
    McmeArena *a = mcme_arena_new();
    if (!a) return;

    McmeNbtDoc first;
    if (!mcme_nbt_parse(&first, a, data, size, le)) {
        /* Rejected input is fine; it must simply not leave an error set that
           confuses a later call, and must not crash. */
        mcme_clear_error();
        mcme_arena_free(a);
        return;
    }

    McmeBuffer y;
    mcme_buffer_init(&y);
    if (!mcme_nbt_write_binary_as(&first, &y, le)) {
        /* Readable but not writable: that is the data-loss bug this exists to
           catch, so treat a write failure on successfully parsed input as a
           finding rather than a graceful skip. */
        fail("parsed input could not be re-serialized", mcme_last_error());
    }

    /* Serializing to SNBT must also work on anything we parsed. */
    {
        McmeBuffer snbt;
        mcme_buffer_init(&snbt);
        if (!mcme_nbt_write_snbt(first.root, &snbt, 0)) {
            fail("parsed input could not be written as SNBT", mcme_last_error());
        }
        mcme_buffer_free(&snbt);
    }

    McmeArena *a2 = mcme_arena_new();
    McmeNbtDoc second;
    if (!a2 || !mcme_nbt_parse(&second, a2, y.data, y.len, le)) {
        fail("output of our own writer failed to parse", mcme_last_error());
    }

    McmeBuffer z;
    mcme_buffer_init(&z);
    if (!mcme_nbt_write_binary_as(&second, &z, le)) {
        fail("re-parsed output could not be re-serialized", mcme_last_error());
    }

    if (z.len != y.len || (z.len > 0 && memcmp(z.data, y.data, z.len) != 0)) {
        fail("serialization is not idempotent", "Y != Z");
    }

    mcme_buffer_free(&z);
    mcme_arena_free(a2);
    mcme_buffer_free(&y);
    mcme_arena_free(a);
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    roundtrip(data, size, false);   /* Java / big-endian */
    roundtrip(data, size, true);    /* Bedrock / little-endian */
    return 0;
}
