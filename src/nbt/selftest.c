#include "mcme/core.h"
#include "mcme/mutf8.h"
#include "mcme/nbt.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * NBT self-tests.
 *
 * The heart of this file is a golden round-trip check against data produced by
 * the reference Python `amulet_nbt` 2.1.8 (see tools/ref_dump.py). For every
 * document we assert, byte for byte:
 *
 *   1. parse BE  -> write BE   == the original .be.bin
 *   2. parse BE  -> write LE   == the original .le.bin   (same key order,
 *                                 opposite byte order — proves the endianness
 *                                 switch rather than re-deriving the file)
 *   3. parse BE  -> write SNBT == the original .snbt
 *   4. parse LE  -> write LE   == .le.bin
 *   5. parse LE  -> write BE   == .be.bin
 *   6. parse SNBT-> write SNBT == .snbt                (idempotent)
 *   7. parse SNBT-> write BE -> parse -> write SNBT == .snbt
 *
 * (2) and (5) are the interesting ones: .be.bin and .le.bin differ only in
 * byte order, so producing the "other" file from the parsed tree is a much
 * stronger check than round-tripping the same file to itself.
 *
 * Checks 1-5 use the golden binaries; 6-7 use the golden SNBT. SNBT is NOT
 * compared against the binaries directly, because the reference sorts compound
 * keys for SNBT but preserves insertion order for binary — the two orderings
 * genuinely differ, and that is the reference's behaviour, not a bug here.
 *
 * If the golden data is absent (running `mcme selftest` from some other
 * directory) these tests are skipped rather than failed, so the binary stays
 * runnable anywhere.
 */

static int g_failed;

#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) {                                                         \
            g_failed++;                                                        \
            printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);           \
        }                                                                      \
    } while (0)

static const char kGoldenDir[] = "tests/golden/nbt";
static char kEmptyRoot[] = "";

static const char *const kGoldenDocs[] = {
    "arrays", "big", "compound_list", "deep_strings", "empty_compound",
    "floats", "lists", "nested", "scalars", "strings", "unnamed_root",
};
#define GOLDEN_DOC_COUNT (sizeof(kGoldenDocs) / sizeof(kGoldenDocs[0]))

/* Reads a golden file. Returns NULL (after clearing the error) if missing. */
static void *load(const char *path, size_t *out_len)
{
    void *data = NULL;
    size_t size = 0;
    if (!mcme_fs_read(path, &data, &size)) {
        mcme_clear_error();
        return NULL;
    }
    *out_len = size;
    return data;
}

static void check_buf(const char *what, const char *doc, const McmeBuffer *got,
                      const void *want, size_t want_len)
{
    if (got->len != want_len) {
        g_failed++;
        printf("  FAIL %s [%s]: got %zu bytes, expected %zu\n",
               what, doc, got->len, want_len);
        return;
    }
    if (want_len > 0 && memcmp(got->data, want, want_len) != 0) {
        const uint8_t *w = (const uint8_t *)want;
        for (size_t i = 0; i < want_len; ++i) {
            if (got->data[i] != w[i]) {
                g_failed++;
                printf("  FAIL %s [%s]: first difference at byte %zu "
                       "(got 0x%02X, expected 0x%02X)\n",
                       what, doc, i, got->data[i], w[i]);
                return;
            }
        }
    }
}

/* Builds a document view over a tag produced by the SNBT parser. SNBT carries
   no root name, so the name is empty. */
static McmeNbtDoc doc_from_tag(McmeArena *arena, McmeTag *tag)
{
    McmeNbtDoc d;
    d.arena = arena;
    d.root = tag;
    d.root_name.data = kEmptyRoot;
    d.root_name.len = 0;
    d.little_endian = false;
    return d;
}

/* ------------------------------------------------------------------
 * Golden binary round trips
 * ------------------------------------------------------------------ */

static void test_golden(const char *name)
{
    char be_path[512], le_path[512], snbt_path[512], root_path[512];
    snprintf(be_path,    sizeof be_path,    "%s/%s.be.bin",    kGoldenDir, name);
    snprintf(le_path,    sizeof le_path,    "%s/%s.le.bin",    kGoldenDir, name);
    snprintf(snbt_path,  sizeof snbt_path,  "%s/%s.snbt",      kGoldenDir, name);
    snprintf(root_path,  sizeof root_path,  "%s/%s.root.txt",  kGoldenDir, name);

    size_t be_len = 0, le_len = 0, snbt_len = 0, root_len = 0;
    void *be    = load(be_path,   &be_len);
    void *le    = load(le_path,   &le_len);
    void *snbt  = load(snbt_path, &snbt_len);
    void *root  = load(root_path, &root_len);

    if (!be || !le || !snbt || !root) {
        printf("  FAIL %s: missing golden files under %s\n", name, kGoldenDir);
        g_failed++;
        goto done;
    }

    /* --- 1, 2, 3: big-endian in --- */
    {
        McmeArena *a = mcme_arena_new();
        McmeNbtDoc doc;
        McmeBuffer out;

        if (!mcme_nbt_parse(&doc, a, be, be_len, false)) {
            g_failed++;
            printf("  FAIL %s: parse BE: %s\n", name, mcme_last_error());
        } else {
            CHECK(doc.root_name.len == root_len);
            CHECK(doc.root_name.len == root_len
                  && memcmp(doc.root_name.data, root, root_len) == 0);

            mcme_buffer_init(&out);
            CHECK(mcme_nbt_write_binary(&doc, &out));
            check_buf("BE round trip", name, &out, be, be_len);
            mcme_buffer_free(&out);

            mcme_buffer_init(&out);
            CHECK(mcme_nbt_write_binary_as(&doc, &out, true));
            check_buf("BE -> LE", name, &out, le, le_len);
            mcme_buffer_free(&out);

            mcme_buffer_init(&out);
            CHECK(mcme_nbt_write_snbt(doc.root, &out, 0));
            check_buf("BE -> SNBT", name, &out, snbt, snbt_len);
            mcme_buffer_free(&out);
        }
        mcme_arena_free(a);
    }

    /* --- 4, 5: little-endian in --- */
    {
        McmeArena *a = mcme_arena_new();
        McmeNbtDoc doc;
        McmeBuffer out;

        if (!mcme_nbt_parse(&doc, a, le, le_len, true)) {
            g_failed++;
            printf("  FAIL %s: parse LE: %s\n", name, mcme_last_error());
        } else {
            mcme_buffer_init(&out);
            CHECK(mcme_nbt_write_binary(&doc, &out));
            check_buf("LE round trip", name, &out, le, le_len);
            mcme_buffer_free(&out);

            mcme_buffer_init(&out);
            CHECK(mcme_nbt_write_binary_as(&doc, &out, false));
            check_buf("LE -> BE", name, &out, be, be_len);
            mcme_buffer_free(&out);
        }
        mcme_arena_free(a);
    }

    /* --- 6, 7: SNBT in --- */
    {
        McmeArena *a = mcme_arena_new();
        McmeTag *tag = NULL;
        McmeBuffer out;

        if (!mcme_nbt_parse_snbt(a, (const char *)snbt, snbt_len, &tag)) {
            g_failed++;
            printf("  FAIL %s: parse SNBT: %s\n", name, mcme_last_error());
        } else {
            mcme_buffer_init(&out);
            CHECK(mcme_nbt_write_snbt(tag, &out, 0));
            check_buf("SNBT idempotent", name, &out, snbt, snbt_len);
            mcme_buffer_free(&out);

            /* SNBT -> binary -> parse -> SNBT must return to the same text.
               This exercises the binary writer on a tree that came from text,
               where compound keys are in sorted rather than insertion order. */
            McmeNbtDoc from_text = doc_from_tag(a, tag);
            mcme_buffer_init(&out);
            CHECK(mcme_nbt_write_binary(&from_text, &out));

            {
                McmeArena *a2 = mcme_arena_new();
                McmeNbtDoc reparsed;
                if (!mcme_nbt_parse(&reparsed, a2, out.data, out.len, false)) {
                    g_failed++;
                    printf("  FAIL %s: reparse SNBT->BE: %s\n",
                           name, mcme_last_error());
                } else {
                    McmeBuffer out2;
                    mcme_buffer_init(&out2);
                    CHECK(mcme_nbt_write_snbt(reparsed.root, &out2, 0));
                    check_buf("SNBT -> BE -> SNBT", name, &out2,
                              snbt, snbt_len);
                    mcme_buffer_free(&out2);
                }
                mcme_arena_free(a2);
            }
            mcme_buffer_free(&out);
        }
        mcme_arena_free(a);
    }

done:
    free(be);
    free(le);
    free(snbt);
    free(root);
}

/* ------------------------------------------------------------------
 * Modified UTF-8
 *
 * The two behaviours that break naive ports: U+0000 becomes C0 80, and a
 * supplementary character becomes a six-byte surrogate pair.
 * ------------------------------------------------------------------ */

static void test_mutf8(void)
{
    McmeBuffer enc, dec;

    /* U+0000 must never emit a bare NUL. */
    mcme_buffer_init(&enc);
    CHECK(mcme_mutf8_encode("a\0b", 3, &enc));
    CHECK(enc.len == 4);
    CHECK(enc.data[0] == 'a' && enc.data[1] == 0xC0
          && enc.data[2] == 0x80 && enc.data[3] == 'b');
    CHECK(mcme_mutf8_encoded_len("a\0b", 3) == 4);

    mcme_buffer_init(&dec);
    CHECK(mcme_mutf8_decode((const char *)enc.data, enc.len, &dec));
    CHECK(dec.len == 3 && memcmp(dec.data, "a\0b", 3) == 0);
    CHECK(mcme_mutf8_decoded_len((const char *)enc.data, enc.len) == 3);
    mcme_buffer_free(&enc);
    mcme_buffer_free(&dec);

    /* U+1F600 (emoji): six bytes in modified UTF-8, four in standard UTF-8. */
    {
        const char *emoji = "\xF0\x9F\x98\x80";
        static const uint8_t want[] = { 0xED, 0xA0, 0xBD, 0xED, 0xB8, 0x80 };

        mcme_buffer_init(&enc);
        CHECK(mcme_mutf8_encode(emoji, 4, &enc));
        CHECK(enc.len == 6);
        CHECK(memcmp(enc.data, want, 6) == 0);
        CHECK(mcme_mutf8_encoded_len(emoji, 4) == 6);

        mcme_buffer_init(&dec);
        CHECK(mcme_mutf8_decode((const char *)enc.data, enc.len, &dec));
        CHECK(dec.len == 4 && memcmp(dec.data, emoji, 4) == 0);
        CHECK(mcme_mutf8_decoded_len((const char *)enc.data, enc.len) == 4);
        mcme_buffer_free(&enc);
        mcme_buffer_free(&dec);
    }

    /* The supplementary round trip works over the whole string. */
    {
        const char *s = "a\xC3\xA9\xF0\x9F\x98\x80\xE6\x9D\xB1";  /* aé😀東 */
        size_t slen = strlen(s);

        mcme_buffer_init(&enc);
        CHECK(mcme_mutf8_encode(s, slen, &enc));
        CHECK(enc.len == mcme_mutf8_encoded_len(s, slen));

        mcme_buffer_init(&dec);
        CHECK(mcme_mutf8_decode((const char *)enc.data, enc.len, &dec));
        CHECK(dec.len == slen && memcmp(dec.data, s, slen) == 0);
        mcme_buffer_free(&enc);
        mcme_buffer_free(&dec);
    }

    /* A four-byte sequence is standard UTF-8 and therefore NOT modified
       UTF-8 — that is the fastest way to tell the two encodings apart. */
    CHECK(!mcme_mutf8_valid("\xF0\x9F\x98\x80", 4));
    /* A lone surrogate is preserved rather than replaced, so odd files still
       round trip byte-for-byte. */
    CHECK(mcme_mutf8_valid("\xED\xA0\xBD", 3));
    /* Truncated sequences are rejected. */
    CHECK(!mcme_mutf8_valid("\xE6\x9D", 2));
    CHECK(!mcme_mutf8_valid("\xC0", 1));
    /* Overlong encodings are rejected, except the canonical C0 80 for U+0000. */
    CHECK(!mcme_mutf8_valid("\xC1\xBF", 2));
    CHECK(mcme_mutf8_valid("\xC0\x80", 2));

    /* Invalid UTF-8 input to encode() fails instead of producing garbage. */
    mcme_buffer_init(&enc);
    CHECK(!mcme_mutf8_encode("\xFF\xFE", 2, &enc));
    CHECK(enc.len == 0);
    mcme_buffer_free(&enc);

    /* mcme_utf8_valid must agree with encode() exactly — it is what the tree
       checks on the way in, so any disagreement would mean either rejecting
       text we could have written, or accepting text we cannot. */
    CHECK(mcme_utf8_valid("", 0));
    CHECK(mcme_utf8_valid("plain ascii", 11));
    CHECK(mcme_utf8_valid("caf\xC3\xA9 \xE6\x9D\xB1\xE4\xBA\xAC",
                          strlen("caf\xC3\xA9 \xE6\x9D\xB1\xE4\xBA\xAC")));
    CHECK(mcme_utf8_valid("\xF0\x9F\x98\x80", 4));          /* supplementary */
    CHECK(mcme_utf8_valid("a\0b", 3));                       /* embedded NUL */
    CHECK(mcme_utf8_valid("\xED\xA0\xBD", 3));               /* lone surrogate, WTF-8 */

    CHECK(!mcme_utf8_valid("\xCA\x20", 2));      /* lead byte + bad continuation */
    CHECK(!mcme_utf8_valid("\xFF", 1));          /* illegal lead byte */
    CHECK(!mcme_utf8_valid("\x80", 1));          /* stray continuation byte */
    CHECK(!mcme_utf8_valid("\xE6\x9D", 2));      /* truncated */
    CHECK(!mcme_utf8_valid("\xC0\xAF", 2));      /* overlong '/' */
    CHECK(!mcme_utf8_valid("\xF5\x80\x80\x80", 4));  /* > U+10FFFF */

    /* A trailing incomplete sequence at the very end. */
    CHECK(!mcme_utf8_valid("ok\xE2\x82", 4));

    /* Truncated modified UTF-8 fails to decode. */
    mcme_buffer_init(&dec);
    CHECK(!mcme_mutf8_decode("\xE6\x9D", 2, &dec));
    mcme_buffer_free(&dec);

    /* Code point lengths. */
    CHECK(mcme_mutf8_cp_len('A') == 1);
    CHECK(mcme_mutf8_cp_len(0x00) == 2);
    CHECK(mcme_mutf8_cp_len(0xE9) == 2);
    CHECK(mcme_mutf8_cp_len(0x6771) == 3);
    CHECK(mcme_mutf8_cp_len(0x1F600) == 6);
    CHECK(mcme_mutf8_cp_len(0x110000) == 0);   /* out of range */
    CHECK(mcme_mutf8_cp_len(0xD800) == 0);     /* surrogate is not a code point */
}

/* ------------------------------------------------------------------
 * Defensive parsing
 *
 * NBT comes off disk from sources we do not control. Every one of these must
 * fail cleanly with an error rather than reading out of bounds or asking for a
 * gigantic allocation. ASan is what actually enforces the "no OOB" half.
 * ------------------------------------------------------------------ */

static bool parse_should_fail(const void *data, size_t len, bool le)
{
    McmeArena *a = mcme_arena_new();
    McmeNbtDoc doc;
    bool ok = mcme_nbt_parse(&doc, a, data, len, le);
    if (ok) {
        g_failed++;
        printf("  FAIL: malformed input parsed successfully (len=%zu)\n", len);
    } else {
        CHECK(mcme_last_error()[0] != '\0');   /* a failure must explain itself */
    }
    mcme_clear_error();
    mcme_arena_free(a);
    return !ok;
}

static void test_malformed(void)
{
    /* Empty input. */
    parse_should_fail("", 0, false);

    /* TAG_End is a terminator, not a value, so it can never be a root. */
    {
        static const uint8_t end_root[] = { 0x00, 0x00, 0x00 };
        parse_should_fail(end_root, sizeof end_root, false);
    }

    /* A type number past the end of the tag table (the reference reports
       "Tag 110 does not exist"). 110 is 'n', i.e. the first byte of a
       text file mistaken for NBT. */
    {
        static const uint8_t bad_type[] = { 110, 0x00, 0x00 };
        parse_should_fail(bad_type, sizeof bad_type, false);
    }

    /* Truncated: a Byte root whose name is present but whose payload is not.
       (This must fail as a short read, NOT because a non-compound root is
       forbidden — see test_non_compound_root, which pins the reference's
       behaviour that any real tag type is a legal root.) */
    {
        static const uint8_t short_byte[] = { 0x01, 0x00, 0x01, 0x2A };
        parse_should_fail(short_byte, sizeof short_byte, false);
    }

    /* Truncated: root compound with a name but no payload. */
    {
        static const uint8_t no_payload[] = { 0x0A, 0x00, 0x04, 'R', 'o' };
        parse_should_fail(no_payload, sizeof no_payload, false);
    }

    /* Truncated: an Int tag whose 4-byte value is cut short. */
    {
        static const uint8_t short_int[] = {
            0x0A, 0x00, 0x00,          /* root compound, empty name */
            0x03, 0x00, 0x01, 'x',     /* int "x" */
            0x00, 0x00,                /* value truncated to 2 bytes */
        };
        parse_should_fail(short_int, sizeof short_int, false);
    }

    /* Invalid tag type inside a compound (0x0D is not a legal NBT type). */
    {
        static const uint8_t bad_type[] = {
            0x0A, 0x00, 0x00,
            0x0D, 0x00, 0x01, 'x',
            0x00,
        };
        parse_should_fail(bad_type, sizeof bad_type, false);
    }

    /* Negative array length. */
    {
        static const uint8_t neg_len[] = {
            0x0A, 0x00, 0x00,
            0x07, 0x00, 0x01, 'a',     /* byte array "a" */
            0xFF, 0xFF, 0xFF, 0xFF,    /* length -1 */
            0x00,
        };
        parse_should_fail(neg_len, sizeof neg_len, false);
    }

    /*
     * A list claiming 2 billion Byte elements. Without a bounds check against
     * the bytes actually remaining this would ask for ~68 GB. The count is a
     * 4-byte field read straight off disk, so it must never be trusted.
     */
    {
        static const uint8_t huge_list[] = {
            0x0A, 0x00, 0x00,
            0x09, 0x00, 0x01, 'l',     /* list "l" */
            0x01,                      /* element type = Byte */
            0x7F, 0xFF, 0xFF, 0xFF,    /* count = 2147483647 */
            0x00,
        };
        parse_should_fail(huge_list, sizeof huge_list, false);
    }

    /* A byte array claiming a length far beyond the buffer. */
    {
        static const uint8_t huge_bytes[] = {
            0x0A, 0x00, 0x00,
            0x07, 0x00, 0x01, 'a',
            0x7F, 0xFF, 0xFF, 0xFF,
            0x00,
        };
        parse_should_fail(huge_bytes, sizeof huge_bytes, false);
    }

    /* A list of TAG_End with a nonzero count — malformed, and would otherwise
       let a tiny file request a huge allocation with zero bytes of payload. */
    {
        static const uint8_t end_list[] = {
            0x0A, 0x00, 0x00,
            0x09, 0x00, 0x01, 'l',
            0x00,                      /* element type = End */
            0x00, 0x00, 0x00, 0x05,    /* count = 5 */
            0x00,
        };
        parse_should_fail(end_list, sizeof end_list, false);
    }

    /* A string whose length prefix runs past the end of the buffer. */
    {
        static const uint8_t long_str[] = {
            0x0A, 0x00, 0x00,
            0x08, 0x00, 0x01, 's',
            0xFF, 0xFF,                /* claims 65535 bytes */
            'x',
            0x00,
        };
        parse_should_fail(long_str, sizeof long_str, false);
    }
}

/* Regression: the SNBT fuzzer mutated one byte of a string into a lead byte
   followed by a non-continuation byte. It used to parse fine and then fail only
   on write_binary, i.e. we would read a world we could not save back. Every
   string path now validates on the way in. */
static void test_snbt_rejects_bad_utf8(void)
{
    struct { const char *snbt; bool should_parse; const char *what; } cases[] = {
        /* NB: adjacent literals, because a C hex escape greedily consumes hex
           digits and "\x20b" would silently mean 0x20B. */
        { "{s: \"a\xCA\x20" "b\"}",   false, "bad byte in value"   },
        { "{s: \"a\xC3\xA9" "b\"}",   true,  "valid UTF-8 in value" },
        { "{s: \"\xC0\xAF" "\"}",     false, "overlong sequence"   },
        { "{s: \"ok\xE2\x82\"}",      false, "truncated tail"      },
        { "{\xCA\x20" ": 1}",         false, "bad byte in key"     },
        { "{s: \"\xED\xA0\xBD\"}",    true,  "lone surrogate"      },
        { "{s: \"\xF0\x9F\x98\x80\"}", true, "supplementary char"  },
    };

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        McmeArena *a = mcme_arena_new();
        McmeTag *tag = NULL;
        bool ok = mcme_nbt_parse_snbt(a, cases[i].snbt, strlen(cases[i].snbt), &tag);
        if (ok != cases[i].should_parse) {
            g_failed++;
            printf("  FAIL snbt utf8 [%s]: parse returned %d, expected %d\n",
                   cases[i].what, ok, cases[i].should_parse);
        }
        if (!ok && mcme_last_error()[0] == '\0') {
            g_failed++;
            printf("  FAIL snbt utf8 [%s]: rejected without an explanation\n",
                   cases[i].what);
        }
        mcme_clear_error();
        mcme_arena_free(a);
    }

    /* An embedded NUL must survive: the reference writes it raw, so this is a
       length-counted string, not a C string — strlen would truncate it. */
    {
        static const char nul_case[] = "{s: \"a\0b\"}";
        McmeArena *a = mcme_arena_new();
        McmeTag *tag = NULL;
        bool ok = mcme_nbt_parse_snbt(a, nul_case, sizeof nul_case - 1, &tag);
        CHECK(ok);
        if (ok) {
            const McmeStr *s = &mcme_nbt_get(tag, "s")->string;
            CHECK(s->len == 3);
            CHECK(s->len == 3 && memcmp(s->data, "a\0b", 3) == 0);
        }
        mcme_clear_error();
        mcme_arena_free(a);
    }

    /* The public setter enforces the same invariant as the parsers. */
    {
        McmeArena *a = mcme_arena_new();
        McmeTag *t = mcme_nbt_new(a, MCME_TAG_String);
        CHECK(!mcme_nbt_set_string(a, t, "\xCA\x20", 2));
        CHECK(mcme_last_error()[0] != '\0');
        mcme_clear_error();
        mcme_arena_free(a);
    }
}

/* The reference loads a root of ANY real tag type — only TAG_End and undefined
   type numbers are refused. Minecraft files always use a compound, so this is
   easy to over-constrain; pinned here so it does not silently regress. */
static void test_non_compound_root(void)
{
    struct { const char *what; bool le;
             const uint8_t *bytes; size_t len; uint8_t want_type; } cases[] = {
        /* Byte root: type 01, name len 0001, name "z", value 42. */
        { "byte root",   false, (const uint8_t *)"\x01\x00\x01\x7A\x2A", 5, 1 },
        { "byte root",   true,  (const uint8_t *)"\x01\x01\x00\x7A\x2A", 5, 1 },
        /* List root: type 09, empty name, element type 01, count 2, two bytes. */
        { "list root",   false,
          (const uint8_t *)"\x09\x00\x00\x01\x00\x00\x00\x02\x07\x08", 10, 9 },
        /* String root: type 08, empty name, u16 length 3, "abc". */
        { "string root", false,
          (const uint8_t *)"\x08\x00\x00\x00\x03\x61\x62\x63", 8, 8 },
    };

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        McmeArena *a = mcme_arena_new();
        McmeNbtDoc doc;
        if (!mcme_nbt_parse(&doc, a, cases[i].bytes, cases[i].len, cases[i].le)) {
            g_failed++;
            printf("  FAIL root type [%s]: %s\n", cases[i].what, mcme_last_error());
            mcme_clear_error();
            mcme_arena_free(a);
            continue;
        }
        CHECK(doc.root != NULL);
        CHECK(doc.root && doc.root->type == cases[i].want_type);

        /* Written back it must be byte-identical, in the same endianness. */
        McmeBuffer out;
        mcme_buffer_init(&out);
        CHECK(mcme_nbt_write_binary_as(&doc, &out, cases[i].le));
        check_buf("non-compound root round trip", cases[i].what,
                  &out, cases[i].bytes, cases[i].len);
        mcme_buffer_free(&out);

        /* And it must render as SNBT without special-casing the root. */
        mcme_buffer_init(&out);
        CHECK(mcme_nbt_write_snbt(doc.root, &out, 0));
        CHECK(out.len > 0);
        mcme_buffer_free(&out);

        mcme_clear_error();
        mcme_arena_free(a);
    }
}

/* Two behaviours pinned against amulet_nbt 2.1.8:
 *
 *   - A list is HOMOGENEOUS. The reference refuses a mismatched element
 *     outright ("Expected type IntTag but got StringTag at 4") rather than
 *     coercing it. Getting this wrong corrupts output, because binary NBT
 *     stores one element type for the whole list.
 *   - Compound keys are UNIQUE, collapsing to first-position/last-value. The
 *     reference does this on both the binary and the SNBT read path.
 */
static void test_list_homogeneity_and_dup_keys(void)
{
    struct { const char *snbt; bool should_parse; const char *what; } lists[] = {
        { "[1, 2, 3]",        true,  "uniform ints"      },
        { "[]",               true,  "empty list"        },
        { "[IB]",             true,  "single element"    },
        { "[\"a\", \"b\"]",   true,  "uniform strings"   },
        { "[1, \"two\"]",     false, "int then string"   },
        { "[\"a\", 1]",       false, "string then int"   },
        { "[1b, 2]",          false, "byte then int"     },
        { "[IB, -128B]",      false, "string then byte"  },
        { "[{a:1}, 2]",       false, "compound then int" },
    };

    for (size_t i = 0; i < sizeof(lists) / sizeof(lists[0]); ++i) {
        McmeArena *a = mcme_arena_new();
        McmeTag *tag = NULL;
        bool ok = mcme_nbt_parse_snbt(a, lists[i].snbt, strlen(lists[i].snbt), &tag);
        if (ok != lists[i].should_parse) {
            g_failed++;
            printf("  FAIL list homogeneity [%s]: parse returned %d, expected %d\n",
                   lists[i].what, ok, lists[i].should_parse);
        } else if (!ok && mcme_last_error()[0] == '\0') {
            g_failed++;
            printf("  FAIL list homogeneity [%s]: rejected with no message\n",
                   lists[i].what);
        }
        mcme_clear_error();
        mcme_arena_free(a);
    }

    /* An empty list keeps the element type the reference writes (Byte). */
    {
        McmeArena *a = mcme_arena_new();
        McmeTag *tag = NULL;
        CHECK(mcme_nbt_parse_snbt(a, "[]", 2, &tag));
        CHECK(tag && tag->type == MCME_TAG_List && tag->list.elem == MCME_TAG_Byte);
        mcme_arena_free(a);
    }

    /* Duplicate keys: first position, last value — matching Python dict. */
    {
        McmeArena *a = mcme_arena_new();
        McmeTag *tag = NULL;
        const char *s = "{a: 1, b: 2, a: 3}";
        CHECK(mcme_nbt_parse_snbt(a, s, strlen(s), &tag));
        CHECK(tag && tag->compound.len == 2);            /* not 3 */
        CHECK(tag && tag->compound.len == 2
              && tag->compound.entries[0].name.len == 1
              && tag->compound.entries[0].name.data[0] == 'a');
        CHECK(tag && tag->compound.len == 2
              && mcme_nbt_long_or(tag->compound.entries[0].value, -1) == 3);
        CHECK(tag && tag->compound.len == 2
              && tag->compound.entries[1].name.data[0] == 'b');
        mcme_arena_free(a);
    }

    /* The same collapse happens on the BINARY path: amulet reads this 20-byte
       file as {"a": 2} and writes it back as 12 bytes. */
    {
        static const uint8_t dup[] = {
            0x0A, 0x00, 0x00,                                   /* root, "" */
            0x03, 0x00, 0x01, 'a', 0x00, 0x00, 0x00, 0x01,      /* a = 1 */
            0x03, 0x00, 0x01, 'a', 0x00, 0x00, 0x00, 0x02,      /* a = 2 */
            0x00,
        };
        McmeArena *a = mcme_arena_new();
        McmeNbtDoc doc;
        if (!mcme_nbt_parse(&doc, a, dup, sizeof dup, false)) {
            g_failed++;
            printf("  FAIL binary dup keys: parse: %s\n", mcme_last_error());
        } else {
            CHECK(doc.root->compound.len == 1);              /* not 2 */
            CHECK(doc.root->compound.len == 1
                  && doc.root->compound.entries[0].value->as_int == 2);
            CHECK(doc.root->compound.len == 1
                  && doc.root->compound.entries[0].name.data[0] == 'a');

            McmeBuffer out;
            mcme_buffer_init(&out);
            CHECK(mcme_nbt_write_binary(&doc, &out));
            check_buf("binary dup collapse", "dup", &out,
                      (const uint8_t *)"\x0A\x00\x00"
                      "\x03\x00\x01\x61\x00\x00\x00\x02\x00", 12);
            mcme_buffer_free(&out);
        }
        mcme_clear_error();
        mcme_arena_free(a);
    }
}

/* Deeply nested input must hit the depth limit instead of exhausting the C
   stack. Crafted nesting is a classic denial-of-service shape. */
static void test_depth_limit(void)
{
    enum { MAX_BUILD = 4096 };   /* 3 + 600*4 child headers + 601 terminators */
    uint8_t *buf = malloc(MAX_BUILD);
    CHECK(buf != NULL);
    if (!buf) return;

    for (int depth = 0; depth <= 600; depth += 1) {
        size_t p = 0;
        buf[p++] = 0x0A; buf[p++] = 0x00; buf[p++] = 0x00;  /* root */
        for (int i = 0; i < depth; ++i) {
            buf[p++] = 0x0A;                 /* nested compound */
            buf[p++] = 0x00; buf[p++] = 0x01;
            buf[p++] = 'a';                  /* named "a" */
        }
        for (int i = 0; i <= depth; ++i) buf[p++] = 0x00;  /* terminators */

        McmeArena *a = mcme_arena_new();
        McmeNbtDoc doc;
        bool ok = mcme_nbt_parse(&doc, a, buf, p, false);
        mcme_clear_error();
        mcme_arena_free(a);

        if (depth <= 10 && !ok) {
            g_failed++;
            printf("  FAIL: shallow nesting (depth %d) was rejected\n", depth);
        }
        if (depth >= 600 && ok) {
            g_failed++;
            printf("  FAIL: nesting of depth %d was accepted, "
                   "limit is %d\n", depth, MCME_NBT_MAX_DEPTH);
        }
    }

    free(buf);
}

/* Every strict prefix of a valid file is truncated. Parsing each must fail
   cleanly rather than read past the end — a cheap exhaustive fuzz that ASan
   checks for us. */
static void test_truncation_sweep(const char *name)
{
    char path[512];
    snprintf(path, sizeof path, "%s/%s.be.bin", kGoldenDir, name);

    size_t len = 0;
    void *data = load(path, &len);
    if (!data) return;

    for (size_t cut = 0; cut < len; ++cut) {
        McmeArena *a = mcme_arena_new();
        McmeNbtDoc doc;
        if (mcme_nbt_parse(&doc, a, data, cut, false)) {
            /* Only legal if the prefix happens to be a complete document. */
            mcme_arena_free(a);
            continue;
        }
        mcme_clear_error();
        mcme_arena_free(a);
    }

    /* Truncating the SNBT text likewise must not crash. */
    snprintf(path, sizeof path, "%s/%s.snbt", kGoldenDir, name);
    free(data);
    data = load(path, &len);
    if (data) {
        for (size_t cut = 0; cut < len; ++cut) {
            McmeArena *a = mcme_arena_new();
            McmeTag *tag = NULL;
            if (!mcme_nbt_parse_snbt(a, (const char *)data, cut, &tag)) {
                mcme_clear_error();
            }
            mcme_arena_free(a);
        }
    }
    free(data);
}

/* ------------------------------------------------------------------
 * Navigation and construction
 * ------------------------------------------------------------------ */

static void test_navigate(void)
{
    McmeArena *a = mcme_arena_new();

    /* Build {name:"world", meta:{ver:42, tags:[10,20,30]}} from scratch. */
    McmeTag *root = mcme_nbt_new(a, MCME_TAG_Compound);
    CHECK(root != NULL);
    CHECK(mcme_nbt_compound_init(a, root, 4));

    McmeTag *name = mcme_nbt_new(a, MCME_TAG_String);
    CHECK(mcme_nbt_set_string(a, name, "world", 5));
    CHECK(mcme_nbt_compound_put(a, root, "name", 4, name));

    McmeTag *meta = mcme_nbt_new(a, MCME_TAG_Compound);
    CHECK(mcme_nbt_compound_init(a, meta, 4));
    McmeTag *ver = mcme_nbt_new(a, MCME_TAG_Int);
    mcme_nbt_set_int(ver, 42);
    CHECK(mcme_nbt_compound_put(a, meta, "ver", 3, ver));

    McmeTag *tags = mcme_nbt_new(a, MCME_TAG_List);
    CHECK(mcme_nbt_list_alloc(a, tags, MCME_TAG_Int, 3));
    mcme_nbt_set_int(&tags->list.items[0], 10);
    mcme_nbt_set_int(&tags->list.items[1], 20);
    mcme_nbt_set_int(&tags->list.items[2], 30);
    CHECK(mcme_nbt_compound_put(a, meta, "tags", 4, tags));

    CHECK(mcme_nbt_compound_put(a, root, "meta", 4, meta));

    /* Dotted path lookup walks compounds and lists. */
    const McmeTag *found = mcme_nbt_path(root, "meta.ver");
    CHECK(found != NULL && found->type == MCME_TAG_Int && found->as_int == 42);

    found = mcme_nbt_path(root, "meta.tags.1");
    CHECK(found != NULL && found->type == MCME_TAG_Int && found->as_int == 20);

    found = mcme_nbt_path(root, "name");
    CHECK(found != NULL && found->type == MCME_TAG_String);
    CHECK(found && found->string.len == 5 && memcmp(found->string.data, "world", 5) == 0);

    /* Missing paths and bad shapes return NULL rather than a bogus value. */
    CHECK(mcme_nbt_path(root, "meta.nope") == NULL);
    CHECK(mcme_nbt_path(root, "nope.at.all") == NULL);
    CHECK(mcme_nbt_path(root, "meta.tags.99") == NULL);
    CHECK(mcme_nbt_path(root, "meta.tags.notanumber") == NULL);
    CHECK(mcme_nbt_path(root, "name.subfield") == NULL);   /* string is not a container */

    /* Replacing a key must keep its original position and spelling, because
       binary output is order-sensitive. */
    size_t before = root->compound.len;
    McmeTag *repl = mcme_nbt_new(a, MCME_TAG_Int);
    mcme_nbt_set_int(repl, 7);
    CHECK(mcme_nbt_compound_put(a, root, "name", 4, repl));
    CHECK(root->compound.len == before);              /* replaced, not appended */
    CHECK(root->compound.entries[0].name.len == 4);   /* slot 0 is still "name" */
    CHECK(memcmp(root->compound.entries[0].name.data, "name", 4) == 0);
    CHECK(root->compound.entries[0].value->as_int == 7);

    /* Typed getters fall back instead of failing. */
    CHECK(mcme_nbt_int_or(mcme_nbt_path(root, "meta.ver"), 0) == 42);
    CHECK(mcme_nbt_int_or(mcme_nbt_path(root, "meta.nope"), -1) == -1);
    CHECK(mcme_nbt_int_or(NULL, -1) == -1);

    mcme_arena_free(a);
}

/* ------------------------------------------------------------------
 * Entry point
 * ------------------------------------------------------------------ */

int mcme_test_nbt(void)
{
    g_failed = 0;

    test_mutf8();

    if (!mcme_fs_exists(kGoldenDir)) {
        printf("  (golden data at '%s' not found — skipping round-trip "
               "tests; run tools/ref_dump.py to regenerate)\n", kGoldenDir);
    } else {
        for (size_t i = 0; i < GOLDEN_DOC_COUNT; ++i) {
            test_golden(kGoldenDocs[i]);
        }
        /* Exhaustive truncation over two representative documents: one with
           awkward strings, one with nested containers. */
        test_truncation_sweep("strings");
        test_truncation_sweep("nested");
    }

    test_malformed();
    test_snbt_rejects_bad_utf8();
    test_list_homogeneity_and_dup_keys();
    test_non_compound_root();
    test_depth_limit();
    test_navigate();

    return g_failed;
}
