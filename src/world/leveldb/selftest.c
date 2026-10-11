/*
 * LevelDB reader self-tests.
 *
 * The reader is tested at two levels, because the two halves fail differently.
 *
 * The primitives — varints, CRC32C, internal-key ordering, log framing — are
 * checked against known answers and against hand-built byte sequences. These
 * need no fixture: they are the parts whose correctness is a property of the
 * format rather than of any particular world, and a wrong one corrupts every
 * store silently while still looking plausible.
 *
 * The store reader is checked against two golden stores, both dumped from
 * Python's leveldb binding (tools/ref_leveldb.py):
 *
 *   tests/golden/worlds/bedrock         100 raw log entries -> 15 live keys,
 *                                       because 85 are tombstones for keys that
 *                                       were never written as values.
 *   tests/golden/worlds/bedrock-tables  the same world grown and compacted
 *                                       (tools/ref_leveldb_compact.py), so a
 *                                       table and a log share the work, with
 *                                       deletions in the log whose values are
 *                                       in the table.
 *
 * Reading these correctly requires the merge to apply deletions rather than
 * report every record it finds, to order by sequence number rather than by
 * which file a record came from, and to tell "present but deleted" from
 * "absent" — which a reader that ignores tombstones gets wrong by returning
 * an older copy of the key instead.
 *
 * Skips rather than fails when the golden data is missing, so `mcme selftest`
 * remains runnable from any directory.
 */

#include "internal.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "stb_ds.h"

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

/*
 * The two golden stores, and what each is here to prove.
 *
 * bedrock is a small world whose entire contents are still in one log file:
 * nothing ever forced a flush. It exercises the log reader, WriteBatch
 * expansion, and tombstones applied within one source.
 *
 * bedrock-tables is that same world grown and compacted (see
 * tools/ref_leveldb_compact.py), so it has a table AND a log, with the newest
 * copy of some keys in the log, older copies in the table, and 100 deletions
 * in the log whose values are in the table. That last part is the case a
 * log-only fixture cannot reach: it is the merge, not just the parsing.
 */
typedef struct {
    const char *dir;
    const char *live;        /* reference's merged key/value view */
    const char *batch;       /* reference's raw log records, deletions kept */
    size_t      want_tables;
    size_t      want_logs;
} StoreCase;

static const StoreCase kStoreCases[] = {
    { "tests/golden/worlds/bedrock/db",
      "tests/golden/leveldb/bedrock.txt",
      "tests/golden/leveldb/bedrock-batch.txt", 0, 1 },

    { "tests/golden/worlds/bedrock-tables/db",
      "tests/golden/leveldb/bedrock-tables.txt",
      "tests/golden/leveldb/bedrock-tables-batch.txt", 1, 1 },

    /* Forged rather than written by LevelDB (tools/ref_leveldb_table.py).
       The two stores above are real, and between them prove the reader
       handles a real world at real scale. What they cannot do is be small:
       Minecraft's fork targets ~100 KiB data blocks, so both end up with a
       single block and a single index entry. This one has eight blocks and
       eight index entries, restart points every eight entries, and alternates
       stored and bare-DEFLATE blocks — so it is the only fixture in which the
       index walk takes a second step, and iteration resumes from a restart
       point rather than only from the head of a block. */
    { "tests/golden/worlds/bedrock-forge/db",
      "tests/golden/leveldb/bedrock-forge.txt",
      "tests/golden/leveldb/bedrock-forge-batch.txt", 1, 1 },
};

/* ========================================================================
 * Varints
 * ======================================================================== */

static void test_varint(void)
{
    /* The canonical examples from LevelDB's own encoding. */
    static const struct { uint64_t v; uint8_t bytes[10]; size_t len; } cases[] = {
        { 0,    { 0x00 }, 1 },
        { 1,    { 0x01 }, 1 },
        { 127,  { 0x7f }, 1 },
        { 128,  { 0x80, 0x01 }, 2 },
        { 300,  { 0xac, 0x02 }, 2 },
        { 16383,{ 0xff, 0x7f }, 2 },
        { 16384,{ 0x80, 0x80, 0x01 }, 3 },
    };

    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; ++i) {
        uint8_t buf[10];
        size_t n = mcme_ldb_varint_encode(cases[i].v, buf, sizeof buf);
        CHECK_MSG(n == cases[i].len, "varint %llu encoded to %zu, want %zu",
                  (unsigned long long)cases[i].v, n, cases[i].len);
        CHECK(memcmp(buf, cases[i].bytes, n) == 0);

        uint64_t back = 0;
        size_t got = mcme_ldb_varint_get(buf, buf + n, &back);
        CHECK(got == n);
        CHECK_MSG(back == cases[i].v, "varint decoded to %llu, want %llu",
                  (unsigned long long)back, (unsigned long long)cases[i].v);
    }

    /* Round trip over a spread of magnitudes, including the full 64-bit range. */
    static const uint64_t values[] = {
        0, 1, 127, 128, 129, 16383, 16384, 1u << 21, 1u << 28,
        0xffffffffULL, 0x100000000ULL, 0x7fffffffffffffffULL, UINT64_MAX
    };
    for (size_t i = 0; i < sizeof values / sizeof values[0]; ++i) {
        uint8_t buf[10];
        size_t n = mcme_ldb_varint_encode(values[i], buf, sizeof buf);
        CHECK(n > 0 && n <= 10);
        uint64_t back = 0;
        CHECK(mcme_ldb_varint_get(buf, buf + n, &back) == n);
        CHECK(back == values[i]);
    }

    /* Truncated: the continuation bit says another byte follows, but there is
       none. Must return 0 rather than consume what it has. */
    {
        const uint8_t trunc[] = { 0x80 };
        uint64_t v = 12345;
        CHECK(mcme_ldb_varint_get(trunc, trunc + sizeof trunc, &v) == 0);
        CHECK(v == 12345);          /* untouched on failure */
    }

    /* Empty range. */
    {
        uint64_t v = 1;
        CHECK(mcme_ldb_varint_get(NULL, NULL, &v) == 0);
    }

    /* An 11-byte varint would overflow a uint64's 10-byte ceiling. LevelDB
       stops at 10; refusing is the point, because accepting more means
       accepting a length the caller will then use to walk a buffer. */
    {
        const uint8_t overlong[] = {
            0x80,0x80,0x80,0x80,0x80,0x80,0x80,0x80,0x80,0x80,0x01
        };
        uint64_t v = 0;
        CHECK(mcme_ldb_varint_get(overlong, overlong + sizeof overlong, &v) == 0);
    }

    /* A varint that claims to be 128 but runs past `end`. */
    {
        const uint8_t short_buf[] = { 0xac };     /* 300's first byte */
        uint64_t v = 0;
        CHECK(mcme_ldb_varint_get(short_buf, short_buf + 1, &v) == 0);
    }
}

/* ========================================================================
 * CRC32C
 * ======================================================================== */

static void test_crc(void)
{
    /* The standard check value: CRC-32C of "123456789" is 0xE3069283. This
       pins the polynomial, the reflection, and the final xor all at once. */
    CHECK(mcme_ldb_crc32c(0, "123456789", 9) == 0xe3069283u);

    /* Seed 0 over nothing is 0. */
    CHECK(mcme_ldb_crc32c(0, "", 0) == 0);

    /* Chaining is how the table walks a record in pieces. */
    {
        const char *s = "123456789";
        uint32_t whole = mcme_ldb_crc32c(0, s, 9);
        uint32_t part  = mcme_ldb_crc32c(mcme_ldb_crc32c(0, s, 4), s + 4, 5);
        CHECK(whole == part);
    }

    /* Masking is invertible: LevelDB relies on that to verify, and a reader
       that only ever masks would silently accept a wrong checksum. */
    {
        uint32_t crc = 0x12345678u;
        uint32_t masked = mcme_ldb_mask_crc(crc);
        CHECK(masked != crc);

        /* The documented inverse: RotateLeft(x - 0xa282ead8, 15). */
        uint32_t back = (masked - 0xa282ead8u);
        back = (back << 15) | (back >> 17);
        CHECK(back == crc);
    }

    /* A corrupt length must not survive masking — that is the entire reason
       the mask exists. Flip the length field of a plausible header and the
       checksum of the payload no longer matches what the header claims. */
    {
        const char payload[] = "the quick brown fox";
        uint32_t good = mcme_ldb_mask_crc(mcme_ldb_crc32c(0, payload, 19));
        uint32_t wrong_len = mcme_ldb_mask_crc(mcme_ldb_crc32c(0, payload, 18));
        CHECK(good != wrong_len);
    }
}

/* ========================================================================
 * Internal keys
 * ======================================================================== */

/* Builds an internal key into a caller's buffer. */
static size_t make_ikey(uint8_t *out, const char *user, uint64_t seq, uint8_t type)
{
    size_t n = strlen(user);
    memcpy(out, user, n);
    uint64_t tag = (seq << 8) | type;
    for (int i = 0; i < 8; ++i) out[n + (size_t)i] = (uint8_t)((tag >> (i * 8)) & 0xff);
    return n + 8;
}

static void test_internal_key(void)
{
    uint8_t a[64], b[64];
    size_t alen, blen;

    /* Same user key, different sequences: NEWER sorts first, because a merge
       walking forward must meet the newest copy of a key before any older one.
       Getting this backwards reads stale data out of every multi-version key
       in a real store. */
    alen = make_ikey(a, "chunk", 100, MCME_LDB_VALUE);
    blen = make_ikey(b, "chunk", 99,  MCME_LDB_VALUE);
    CHECK(mcme_ldb_internal_cmp(a, alen, b, blen) < 0);

    alen = make_ikey(a, "chunk", 5, MCME_LDB_VALUE);
    blen = make_ikey(b, "chunk", 9, MCME_LDB_VALUE);
    CHECK(mcme_ldb_internal_cmp(a, alen, b, blen) > 0);

    /* Identical internal keys compare equal. */
    alen = make_ikey(a, "chunk", 7, MCME_LDB_VALUE);
    blen = make_ikey(b, "chunk", 7, MCME_LDB_VALUE);
    CHECK(mcme_ldb_internal_cmp(a, alen, b, blen) == 0);

    /* Different user keys: the user key decides, regardless of sequence. A
       higher sequence on a shorter key must not jump ahead of it. */
    alen = make_ikey(a, "aa", 1, MCME_LDB_VALUE);
    blen = make_ikey(b, "ab", 999999, MCME_LDB_VALUE);
    CHECK(mcme_ldb_internal_cmp(a, alen, b, blen) < 0);

    alen = make_ikey(a, "b", 1, MCME_LDB_VALUE);
    blen = make_ikey(b, "a", 999999, MCME_LDB_VALUE);
    CHECK(mcme_ldb_internal_cmp(a, alen, b, blen) > 0);

    /* A prefix sorts before its extension — memcmp's length tiebreak. Bedrock
       keys differ only in their trailing tag byte (0x2d, 0x36, 0x76), so this
       is the comparison that actually orders a chunk's keys. */
    alen = make_ikey(a, "abc", 1, MCME_LDB_VALUE);
    blen = make_ikey(b, "abcd", 1, MCME_LDB_VALUE);
    CHECK(mcme_ldb_internal_cmp(a, alen, b, blen) < 0);

    /* Byte 0x00 < 0xff at the same position, i.e. dimension -1 sorts last.
       Signed coordinates in Bedrock keys are two's complement, and a reader
       that compared them as signed integers would place them wrongly. */
    alen = make_ikey(a, "\x00\x00\x00\x00", 1, MCME_LDB_VALUE);
    blen = make_ikey(b, "\xff\xff\xff\xff", 1, MCME_LDB_VALUE);
    CHECK(mcme_ldb_internal_cmp(a, alen, b, blen) < 0);

    /* Value type only breaks a tie when sequence AND user key match, which
       cannot happen for two real entries of the same key. */
    alen = make_ikey(a, "k", 4, MCME_LDB_VALUE);
    blen = make_ikey(b, "k", 4, MCME_LDB_DELETION);
    CHECK(mcme_ldb_internal_cmp(a, alen, b, blen) > 0);

    /* The user-key comparator ignores the tag entirely. */
    alen = make_ikey(a, "k", 4, MCME_LDB_VALUE);
    blen = make_ikey(b, "k", 99, MCME_LDB_DELETION);
    CHECK(mcme_ldb_user_cmp(a, alen, b, blen) == 0);

    /* Short keys must not read past their end. Neither side carries a tag, so
       the comparison stays on the bytes that exist. */
    {
        const uint8_t ab[] = { 'a', 'b' };
        const uint8_t a2[] = { 'a' };
        CHECK(mcme_ldb_internal_cmp(ab, 2, ab, 2) == 0);
        CHECK(mcme_ldb_internal_cmp(a2, 1, ab, 2) < 0);
        CHECK(mcme_ldb_internal_cmp(ab, 2, a2, 1) > 0);
    }
}

/* ========================================================================
 * Log framing
 * ======================================================================== */

typedef struct {
    /* Captured records, concatenated with a separator so boundaries survive. */
    McmeBuffer seen;
    size_t     count;
    bool       stop_after_first;
} LogCapture;

static bool on_log_record_capture(void *ud, const uint8_t *rec, size_t len)
{
    LogCapture *c = (LogCapture *)ud;
    if (c->count > 0) mcme_buffer_append_u8(&c->seen, '|');
    mcme_buffer_append(&c->seen, rec, len);
    c->count++;
    return !(c->stop_after_first && c->count == 1);
}

/* The checksum LevelDB puts in a fragment header: its own type byte followed by
   its own payload. Deliberately NOT the whole logical record — see log.c. */
static uint32_t frag_crc(uint8_t type, const uint8_t *payload, size_t len)
{
    uint32_t crc = mcme_ldb_crc32c(0, &type, 1);
    return mcme_ldb_crc32c(crc, payload, len);
}

/* Appends one fragment header followed by its payload. */
static void put_frag(McmeBuffer *b, uint8_t type, const uint8_t *payload,
                     size_t len, uint32_t record_crc)
{
    uint32_t masked = mcme_ldb_mask_crc(record_crc);
    for (int i = 0; i < 4; ++i) mcme_buffer_append_u8(b, (uint8_t)((masked >> (i * 8)) & 0xff));
    mcme_buffer_append_u8(b, (uint8_t)(len & 0xff));
    mcme_buffer_append_u8(b, (uint8_t)((len >> 8) & 0xff));
    mcme_buffer_append_u8(b, type);
    mcme_buffer_append(b, payload, len);
}

static void test_log(void)
{
    /* --- A single FULL record, the shape both the fixture's log and every
           MANIFEST record actually take. --- */
    {
        const uint8_t payload[] = { 0xde, 0xad, 0xbe, 0xef };
        uint32_t crc = frag_crc(1, payload, sizeof payload);

        McmeBuffer log;
        mcme_buffer_init(&log);
        put_frag(&log, 1, payload, sizeof payload, crc);
        /* A short tail after a complete record is a normal end of file. */
        mcme_buffer_append(&log, "\x01\x02", 2);

        LogCapture cap;
        mcme_buffer_init(&cap.seen);
        cap.count = 0;
        cap.stop_after_first = false;

        size_t records = 0;
        CHECK(mcme_ldb_log_read(log.data, log.len, on_log_record_capture, &cap,
                                &records));
        CHECK(records == 1);
        CHECK(cap.count == 1);
        CHECK(cap.seen.len == sizeof payload);
        if (cap.seen.data != NULL) {
            CHECK(memcmp(cap.seen.data, payload, sizeof payload) == 0);
        }

        mcme_buffer_free(&cap.seen);
        mcme_buffer_free(&log);
    }

    /* --- The record's TYPE BYTE is part of its checksum.
           LevelDB's writer extends over (&type, 1) and then the payload. A
           reader that checksums the payload alone accepts this record, while
           rejecting every one a real LevelDB wrote -- the inverse of the usual
           failure, and not something any hand-written test built on the same
           assumption would notice. This pins the convention on its own, so the
           store test is not the only thing that catches it. --- */
    {
        const uint8_t payload[] = { 0xde, 0xad, 0xbe, 0xef };

        McmeBuffer log;
        mcme_buffer_init(&log);
        /* Correct length, correct type, checksum computed without the type. */
        put_frag(&log, 1, payload, sizeof payload,
                 mcme_ldb_crc32c(0, payload, sizeof payload));

        LogCapture cap;
        mcme_buffer_init(&cap.seen);
        cap.count = 0;
        cap.stop_after_first = false;

        size_t records = 0;
        CHECK(!mcme_ldb_log_read(log.data, log.len, on_log_record_capture, &cap,
                                 &records));
        CHECK(records == 0);

        mcme_buffer_free(&cap.seen);
        mcme_buffer_free(&log);
    }

    /* --- A record split FIRST + MIDDLE + LAST across three block boundaries,
           all carrying the checksum of the whole payload rather than of their
           own fragment.
           The sizes are LevelDB's writer's own arithmetic: it fills a block to
           exactly kBlockSize - 7 payload bytes, switches, and repeats. A test
           that wrote a fragment larger than the space left in its block would
           be asserting a shape no writer ever produces. --- */
    {
        enum { FIRST_LEN = 32768 - 7, MIDDLE_LEN = 32768 - 7, TAIL_LEN = 5000 };
        enum { TOTAL = FIRST_LEN + MIDDLE_LEN + TAIL_LEN };

        uint8_t payload[TOTAL];
        for (size_t i = 0; i < TOTAL; ++i) {
            payload[i] = (uint8_t)(i * 31u + 7u);
        }

        McmeBuffer log;
        mcme_buffer_init(&log);
        /* Each fragment's checksum covers its own type byte and its own slice
           of the payload, so the three differ even though they share a record. */
        put_frag(&log, 2, payload,                          FIRST_LEN,
                 frag_crc(2, payload, FIRST_LEN));
        put_frag(&log, 3, payload + FIRST_LEN,              MIDDLE_LEN,
                 frag_crc(3, payload + FIRST_LEN, MIDDLE_LEN));
        put_frag(&log, 4, payload + FIRST_LEN + MIDDLE_LEN, TAIL_LEN,
                 frag_crc(4, payload + FIRST_LEN + MIDDLE_LEN, TAIL_LEN));

        /* Blocks one and two must be exactly full: that is what makes the
           reader step forward rather than re-read the same offset. Two
           32761-byte payloads plus their 7-byte headers fill both. */
        CHECK(log.len == 2u * MCME_LDB_BLOCK_SIZE + TAIL_LEN + 7u);

        LogCapture cap;
        mcme_buffer_init(&cap.seen);
        cap.count = 0;
        cap.stop_after_first = false;

        size_t records = 0;
        CHECK(mcme_ldb_log_read(log.data, log.len, on_log_record_capture, &cap,
                                &records));
        CHECK_MSG(records == 1, "fragmented record produced %zu records", records);
        CHECK(cap.seen.len == TOTAL);
        if (cap.seen.len == TOTAL) {
            CHECK(memcmp(cap.seen.data, payload, TOTAL) == 0);
        }

        mcme_buffer_free(&cap.seen);
        mcme_buffer_free(&log);
    }

    /* --- A fragment that claims a payload running past the end of its block.
           This is the header-overrun guard: reading it as if it fitted would
           walk straight off the buffer, so it must stop the walk instead. --- */
    {
        McmeBuffer log;
        mcme_buffer_init(&log);
        const uint8_t payload[] = { 1, 2, 3 };
        put_frag(&log, 1, payload, sizeof payload, frag_crc(1, payload, 3));
        /* Overwrite the length field to claim more than the block holds. */
        log.data[4] = 0xff;
        log.data[5] = 0xff;
        log.data[6] = 1;                        /* type stays FULL */

        LogCapture cap;
        mcme_buffer_init(&cap.seen);
        cap.count = 0;
        cap.stop_after_first = false;

        size_t records = 0;
        CHECK(mcme_ldb_log_read(log.data, log.len, on_log_record_capture, &cap,
                                &records));
        CHECK(records == 0);
        CHECK(cap.seen.len == 0);

        mcme_buffer_free(&cap.seen);
        mcme_buffer_free(&log);
    }

    /* --- Corruption: a bad checksum stops the walk rather than handing the
           caller possibly-altered bytes, and everything before it survives. --- */
    {
        const uint8_t good[] = { 1, 2, 3, 4 };
        const uint8_t bad[]  = { 9, 9, 9, 9 };

        McmeBuffer log;
        mcme_buffer_init(&log);
        put_frag(&log, 1, good, sizeof good, frag_crc(1, good, sizeof good));
        /* Correct length, plausible type, checksum belonging to a different
           payload: the shape real damage takes. */
        put_frag(&log, 1, bad,  sizeof bad,  frag_crc(1, good, sizeof good));

        LogCapture cap;
        mcme_buffer_init(&cap.seen);
        cap.count = 0;
        cap.stop_after_first = false;

        size_t records = 0;
        CHECK(!mcme_ldb_log_read(log.data, log.len, on_log_record_capture, &cap,
                                 &records));
        CHECK(records == 1);        /* the intact record still came through */

        mcme_buffer_free(&cap.seen);
        mcme_buffer_free(&log);
    }

    /* --- An unknown fragment type is damage, not an end of file. --- */
    {
        const uint8_t payload[] = { 1 };
        uint32_t crc = frag_crc(99, payload, sizeof payload);

        McmeBuffer log;
        mcme_buffer_init(&log);
        put_frag(&log, 99, payload, sizeof payload, crc);

        LogCapture cap;
        mcme_buffer_init(&cap.seen);
        cap.count = 0;
        cap.stop_after_first = false;

        size_t records = 0;
        CHECK(!mcme_ldb_log_read(log.data, log.len, on_log_record_capture, &cap,
                                 &records));
        CHECK(records == 0);

        mcme_buffer_free(&cap.seen);
        mcme_buffer_free(&log);
    }

    /* --- An empty file reads cleanly with no records: a fresh store's log
           is legitimately empty. --- */
    {
        LogCapture cap;
        mcme_buffer_init(&cap.seen);
        cap.count = 0;
        cap.stop_after_first = false;
        size_t records = 0;
        CHECK(mcme_ldb_log_read((const uint8_t *)"", 0, on_log_record_capture,
                                &cap, &records));
        CHECK(records == 0);
        mcme_buffer_free(&cap.seen);
    }

    /* --- A callback returning false stops the walk but is not an error. --- */
    {
        const uint8_t payload[] = { 7 };
        uint32_t crc = frag_crc(1, payload, sizeof payload);

        McmeBuffer log;
        mcme_buffer_init(&log);
        put_frag(&log, 1, payload, sizeof payload, crc);
        put_frag(&log, 1, payload, sizeof payload, crc);

        LogCapture cap;
        mcme_buffer_init(&cap.seen);
        cap.count = 0;
        cap.stop_after_first = true;

        size_t records = 0;
        CHECK(mcme_ldb_log_read(log.data, log.len, on_log_record_capture, &cap,
                                &records));
        CHECK(records == 0);        /* it stopped before counting the first */

        mcme_buffer_free(&cap.seen);
        mcme_buffer_free(&log);
    }

    /* --- NULL arguments are refused rather than dereferenced. --- */
    {
        size_t records = 0;
        CHECK(!mcme_ldb_log_read(NULL, 10, on_log_record_capture, NULL, &records));
    }

    /* --- The writer round-trips through the reader, including the two paths
           that are easy to get wrong: a record that ends within six bytes of
           a block end (the writer must pad the trailer, or the reader sees a
           seven-byte header made of garbage) and a record bigger than one
           block (FIRST/MIDDLE/LAST, each fragment checksummed alone). This
           writes with mcme_ldb_log_append and reads with mcme_ldb_log_read —
           both ours — so the external check is that the reference library
           reads what `mcme db put` produces, done in the store test's
           golden comparison, not here. --- */
    {
        /* 32768 - 7 - 4: after this FULL record, four bytes remain — fewer
           than a seven-byte header — so the writer must pad the trailer to
           the block end and the next record starts a block over. Four, not
           ten: ten would fit the next header, and nothing would be tested. */
        enum { TAIL = 4 };
        size_t payload_len = MCME_LDB_BLOCK_SIZE - 7 - TAIL;
        uint8_t *payload = (uint8_t *)malloc(payload_len);
        CHECK(payload != NULL);
        if (payload != NULL) {
            for (size_t i = 0; i < payload_len; ++i) {
                payload[i] = (uint8_t)(i * 31);
            }

            char path[] = "/tmp/opencode/mcme_ldb_log_XXXXXX";
            int fd = mkstemp(path);
            CHECK(fd >= 0);
            FILE *fh = fd >= 0 ? fdopen(fd, "w+b") : NULL;
            CHECK(fh != NULL);

            if (fh != NULL) {
                size_t off = 0;
                CHECK(mcme_ldb_log_append(fh, payload, payload_len, &off));
                /* Second record: must land at the next block boundary, after
                   exactly TAIL zero bytes. */
                const uint8_t second[] = { 0xca, 0xfe };
                CHECK(mcme_ldb_log_append(fh, second, sizeof second, &off));

                /* Third: bigger than a block, so it fragments. */
                size_t big_len = MCME_LDB_BLOCK_SIZE + 100;
                uint8_t *big = (uint8_t *)malloc(big_len);
                CHECK(big != NULL);
                if (big != NULL) {
                    for (size_t i = 0; i < big_len; ++i) big[i] = (uint8_t)(i * 7);
                    CHECK(mcme_ldb_log_append(fh, big, big_len, &off));

                    CHECK(fflush(fh) == 0);
                    fseek(fh, 0, SEEK_END);
                    long size = ftell(fh);
                    /* payload + 7, then TAIL padding, then second + 7, then
                       the big record: full blocks on each side of it. */
                    CHECK(size == (long)(payload_len + 7 + TAIL + 2 + 7) +
                                  (long)big_len + 7 + 7);
                    fseek(fh, 0, SEEK_SET);

                    uint8_t *data = (uint8_t *)malloc((size_t)size);
                    CHECK(data != NULL);
                    if (data != NULL) {
                        CHECK(fread(data, 1, (size_t)size, fh) == (size_t)size);

                        /* The padding is exactly TAIL zero bytes and the
                           reader must step over them into the next block. */
                        size_t pad_at = payload_len + 7;
                        for (size_t i = 0; i < TAIL; ++i) {
                            CHECK(data[pad_at + i] == 0);
                        }

                        LogCapture cap;
                        mcme_buffer_init(&cap.seen);
                        cap.count = 0;
                        cap.stop_after_first = false;
                        size_t records = 0;
                        CHECK(mcme_ldb_log_read(data, (size_t)size,
                                                on_log_record_capture, &cap,
                                                &records));
                        /* Three logical records, whatever the fragmentation
                           underneath. cap.seen holds all three joined by
                           '|', in order — so the check is that the reader
                           reassembled each one's exact bytes, not merely
                           that it found three of them. */
                        CHECK(records == 3);
                        size_t want_len = payload_len + 1 + 2 + 1 + big_len;
                        CHECK(cap.seen.len == want_len);
                        if (cap.seen.data != NULL && cap.seen.len == want_len) {
                            CHECK(memcmp(cap.seen.data, payload, payload_len) == 0);
                            CHECK(cap.seen.data[payload_len] == '|');
                            CHECK(memcmp(cap.seen.data + payload_len + 1,
                                         second, 2) == 0);
                            CHECK(cap.seen.data[payload_len + 3] == '|');
                            CHECK(memcmp(cap.seen.data + payload_len + 4,
                                         big, big_len) == 0);
                        }
                        mcme_buffer_free(&cap.seen);
                        free(data);
                    }
                    free(big);
                }
                fclose(fh);
            }
            remove(path);
            free(payload);
        }
    }
}

/* ========================================================================
 * Golden helpers
 * ======================================================================== */

static int hex_nibble(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* Decodes an even-length hex string into malloc'd bytes. NULL on bad input. */
/* Compares two byte ranges that are allowed to be empty.
   memcmp's contract requires non-NULL pointers even for a zero length, and a
   zero-length value really does leave a buffer's data pointer NULL — so calling
   memcmp on one is undefined behaviour that a sanitizer build reports while the
   comparison itself is still "correct". Going through here keeps the call
   legal without sprinkling length checks around every comparison. */
static bool same_bytes(const void *a, const void *b, size_t n)
{
    if (n == 0) return true;
    return memcmp(a, b, n) == 0;
}

static uint8_t *from_hex(const char *s, size_t *out_len)
{
    size_t n = strlen(s);
    if (n % 2 != 0) return NULL;
    uint8_t *out = (uint8_t *)malloc(n / 2 + 1);
    if (out == NULL) return NULL;
    for (size_t i = 0; i < n; i += 2) {
        int hi = hex_nibble(s[i]), lo = hex_nibble(s[i + 1]);
        if (hi < 0 || lo < 0) { free(out); return NULL; }
        out[i / 2] = (uint8_t)((hi << 4) | lo);
    }
    *out_len = n / 2;
    return out;
}

typedef struct {
    uint8_t *key;  size_t klen;
    uint8_t *val;  size_t vlen;
    long     seq;
    int      type;            /* 0 deletion, 1 value; -1 for the live dump */
} GoldenEntry;

/*
 * Reads a golden dump into a malloc'd array. Returns NULL when the file is
 * absent, which callers treat as "skip this test".
 *
 * Format is one entry per line: `key-hex value-hex` for the live dump, and
 * `seq type key-hex value-hex` for the batch dump. Lines starting with '#' are
 * comments.
 */
static GoldenEntry *load_golden(const char *path, const char *fmt, size_t *out_n)
{
    FILE *fh = fopen(path, "r");
    if (fh == NULL) return NULL;

    /* A plain realloc'd array with an explicit count, deliberately not an
       stb_ds array: mixing the two is a heap read before the allocation,
       which is exactly the kind of thing this project's sanitizer build
       exists to catch. */
    GoldenEntry *entries = NULL;
    size_t count = 0, cap = 0;
    char *line = NULL;
    size_t line_cap = 0;

    while (getline(&line, &line_cap, fh) > 0) {
        if (line[0] == '#' || line[0] == '\n' || line[0] == '\0') continue;

        char khex[4096] = {0};
        char vhex[16384] = {0};
        long seq = -1;
        int type = -1;
        int got = 0;

        if (strcmp(fmt, "live") == 0) {
            /* A live entry always has a value, but the value may legitimately
               be zero bytes, so its line is `<key-hex> ` with nothing after the
               space. Asking sscanf for two tokens drops that line entirely,
               which reads as a reference that is one entry short rather than
               as a parser that cannot represent an empty value. Read the key,
               then take whatever follows it — including nothing. */
            int after_key = 0;
            got = sscanf(line, "%4095s%n", khex, &after_key);
            if (got != 1 || after_key <= 0) continue;

            const char *rest = line + after_key;
            while (*rest == ' ' || *rest == '\t') rest++;
            size_t n = strcspn(rest, "\r\n");
            if (n >= sizeof vhex) n = sizeof vhex - 1;
            memcpy(vhex, rest, n);
            vhex[n] = '\0';
        } else {
            /* `seq type key value`, where a DELETION record has no value at
               all — the field is simply absent, not an empty token. So the
               value is whatever follows the key, with nothing required. */
            int after_key = 0;
            got = sscanf(line, "%ld %d %4095s%n", &seq, &type, khex, &after_key);
            if (got != 3 || after_key <= 0) continue;

            const char *rest = line + after_key;
            while (*rest == ' ' || *rest == '\t') rest++;
            size_t n = strcspn(rest, "\r\n");
            if (n >= sizeof vhex) n = sizeof vhex - 1;
            memcpy(vhex, rest, n);
            vhex[n] = '\0';
        }

        if (count == cap) {
            size_t next = cap ? cap * 2 : 32;
            GoldenEntry *grown =
                (GoldenEntry *)realloc(entries, next * sizeof *entries);
            if (grown == NULL) break;
            entries = grown;
            cap = next;
        }

        GoldenEntry e;
        memset(&e, 0, sizeof e);
        e.key = from_hex(khex, &e.klen);
        e.val = from_hex(vhex, &e.vlen);
        e.seq = seq;
        e.type = type;
        if (e.key == NULL || e.val == NULL) {
            free(e.key); free(e.val);
            continue;
        }
        entries[count++] = e;
    }

    free(line);
    fclose(fh);
    *out_n = count;
    return entries;
}

static void free_golden(GoldenEntry *entries, size_t n)
{
    for (size_t i = 0; i < n; ++i) {
        free(entries[i].key);
        free(entries[i].val);
    }
    free(entries);
}

/* Every live entry mcme's iterate() produced, captured for comparison. */
typedef struct {
    const uint8_t *key;  size_t klen;
    const uint8_t *val;  size_t vlen;
    uint64_t       seq;
} Captured;

static bool capture_entry(void *ud, const McmeLevelEntry *e)
{
    Captured **out = (Captured **)ud;
    Captured c;
    c.key = e->key;
    c.klen = e->key_len;
    c.val = e->val;
    c.vlen = e->val_len;
    c.seq = e->sequence;
    arrput(*out, c);
    return true;
}

/* ========================================================================
 * Golden: the store
 * ======================================================================== */

/* Counts entries and stops after the first. */
static bool stop_after_one(void *ud, const McmeLevelEntry *e)
{
    (void)e;
    size_t *n = (size_t *)ud;
    (*n)++;
    return false;
}

static void check_store(const StoreCase *c)
{
    size_t live_n = 0, batch_n = 0;
    GoldenEntry *live = load_golden(c->live, "live", &live_n);
    GoldenEntry *batch = load_golden(c->batch, "batch", &batch_n);

    if (live == NULL || live_n == 0) {
        printf("  skip %s: no %s\n", c->dir, c->live);
        free_golden(live, live_n);
        free_golden(batch, batch_n);
        return;
    }

    McmeLevelDb *db = NULL;
    if (!mcme_leveldb_open(&db, c->dir)) {
        CHECK_MSG(false, "open(%s): %s", c->dir, mcme_last_error());
        free_golden(live, live_n);
        free_golden(batch, batch_n);
        return;
    }

    /* --- Identity of the store. --- */
    {
        const char *cmp = mcme_leveldb_comparator(db);
        CHECK(cmp != NULL);
        if (cmp != NULL) {
            CHECK_MSG(strcmp(cmp, "leveldb.BytewiseComparator") == 0,
                      "comparator is '%s'", cmp);
        }
        CHECK_MSG(mcme_leveldb_log_count(db) == c->want_logs,
                  "log count %zu, want %zu",
                  mcme_leveldb_log_count(db), c->want_logs);
        CHECK_MSG(mcme_leveldb_table_count(db) == c->want_tables,
                  "table count %zu, want %zu",
                  mcme_leveldb_table_count(db), c->want_tables);
    }

    /* --- count() agrees with the reference's live key total. --- */
    CHECK_MSG(mcme_leveldb_count(db) == live_n,
              "count() = %zu, reference = %zu", mcme_leveldb_count(db), live_n);

    /* --- iterate() yields exactly the reference's entries, in the same
           order, byte for byte. Order matters: the whole merge rests on it. --- */
    Captured *got = NULL;
    size_t got_n = mcme_leveldb_iterate(db, capture_entry, &got);

    CHECK_MSG(got_n == live_n, "iterate() delivered %zu, reference has %zu",
              got_n, live_n);

    size_t compare_n = got_n < live_n ? got_n : live_n;
    for (size_t i = 0; i < compare_n; ++i) {
        CHECK_MSG(got[i].klen == live[i].klen,
                  "entry %zu key length %zu, want %zu", i, got[i].klen, live[i].klen);
        if (got[i].klen == live[i].klen) {
            CHECK_MSG(same_bytes(got[i].key, live[i].key, got[i].klen),
                      "entry %zu key %.*s does not match", i,
                      (int)got[i].klen, got[i].key);
        }
        CHECK_MSG(got[i].vlen == live[i].vlen,
                  "entry %zu value length %zu, want %zu", i, got[i].vlen, live[i].vlen);
        if (got[i].vlen == live[i].vlen) {
            CHECK_MSG(same_bytes(got[i].val, live[i].val, got[i].vlen),
                      "entry %zu value differs", i);
        }
        CHECK(got[i].seq > 0);
    }
    arrfree(got);

    /* --- Tombstones are applied, not reported and not mistaken for absence.
           This is the check that fails if the merge forgets deletions. --- */
    if (batch != NULL && batch_n > 0) {
        size_t deletions = 0, values = 0;
        for (size_t i = 0; i < batch_n; ++i) {
            if (batch[i].type == 0) deletions++;
            else if (batch[i].type == 1) values++;
        }
        CHECK_MSG(deletions > 0, "fixture has no tombstones to test against");
        CHECK_MSG(values > 0, "fixture has no values in its log");
        if (c->want_tables == 0) {
            /* A log-only store: every live key comes out of the log, so the
               counts must line up exactly. */
            CHECK_MSG(values == live_n, "%zu raw values, %zu live keys",
                      values, live_n);
        }

        /* Spot-check every tombstone: present in the store, reported as
           deleted, and never delivered by iterate(). */
        for (size_t i = 0; i < batch_n; ++i) {
            if (batch[i].type != 0) continue;

            CHECK_MSG(mcme_leveldb_has(db, batch[i].key, batch[i].klen),
                      "tombstone %zu not present in the store", i);

            McmeBuffer buf;
            mcme_buffer_init(&buf);
            bool deleted = false;
            CHECK(mcme_leveldb_get(db, batch[i].key, batch[i].klen, &buf, &deleted));
            CHECK_MSG(deleted, "get() on tombstone %zu did not report deleted", i);
            CHECK(buf.len == 0);
            mcme_buffer_free(&buf);

            /* And no live entry may carry that key. */
            bool seen_live = false;
            for (size_t j = 0; j < live_n; ++j) {
                if (live[j].klen == batch[i].klen &&
                    memcmp(live[j].key, batch[i].key, batch[i].klen) == 0) {
                    seen_live = true;
                    break;
                }
            }
            CHECK_MSG(!seen_live, "tombstone %zu also appears as a live key", i);
        }

        /* Spot-check every value: fetchable by exact match, correct bytes. */
        for (size_t i = 0; i < live_n; ++i) {
            CHECK_MSG(mcme_leveldb_has(db, live[i].key, live[i].klen),
                      "live key %zu not found by has()", i);

            McmeBuffer buf;
            mcme_buffer_init(&buf);
            bool deleted = true;
            CHECK_MSG(mcme_leveldb_get(db, live[i].key, live[i].klen, &buf,
                                       &deleted),
                      "get(%zu): %s", i, mcme_last_error());
            CHECK(!deleted);
            CHECK_MSG(buf.len == live[i].vlen,
                      "get(%zu) returned %zu bytes, want %zu", i, buf.len,
                      live[i].vlen);
            if (buf.len == live[i].vlen) {
                CHECK_MSG(same_bytes(buf.data, live[i].val, buf.len),
                          "get(%zu) value differs", i);
            }
            mcme_buffer_free(&buf);
        }
    }

    /* --- Absence is reported as absence, not as failure-to-open. --- */
    {
        const uint8_t missing[] = { 0xde, 0xad, 0xbe, 0xef, 0x00 };
        CHECK(!mcme_leveldb_has(db, missing, sizeof missing));

        McmeBuffer buf;
        mcme_buffer_init(&buf);
        bool deleted = false;
        CHECK(!mcme_leveldb_get(db, missing, sizeof missing, &buf, &deleted));
        CHECK(buf.len == 0);
        mcme_buffer_free(&buf);

        /* A key one byte longer than a real one must also be absent: Bedrock
           distinguishes chunk tags by a single trailing byte, so a reader that
           compared only a prefix would answer "present" here. */
        CHECK(!mcme_leveldb_has(db, "\x00\x00\x00\x00\x00\x00\x00\x00", 8));
    }

    /* --- Early exit from iterate is honoured and is not an error. --- */
    {
        size_t n = 0;
        CHECK(mcme_leveldb_iterate(db, stop_after_one, &n) == 1);
        CHECK(n == 1);
    }

    mcme_leveldb_close(db);
    free_golden(live, live_n);
    free_golden(batch, batch_n);
}

static void test_store(void)
{
    const size_t n = sizeof kStoreCases / sizeof kStoreCases[0];
    for (size_t i = 0; i < n; ++i) {
        printf("  store: %s\n", kStoreCases[i].dir);
        check_store(&kStoreCases[i]);
    }
}

/* ========================================================================
 * Refusals
 * ======================================================================== */

static void test_refusals(void)
{
    /* Nothing at all. */
    {
        McmeLevelDb *db = NULL;
        CHECK(!mcme_leveldb_open(&db, "tests/golden/definitely-not-here"));
        CHECK(db == NULL);
        CHECK(mcme_last_error()[0] != '\0');
    }

    /* A directory that exists but is not a store: no CURRENT file. This is
       what happens when a user points the tool at the world folder instead of
       its db/ subfolder, which is the most common way to get this wrong. */
    {
        McmeLevelDb *db = NULL;
        CHECK(!mcme_leveldb_open(&db, "tests/golden"));
        CHECK(db == NULL);
    }

    /* A file where a directory is expected. */
    {
        McmeLevelDb *db = NULL;
        CHECK(!mcme_leveldb_open(&db, kStoreCases[0].live));
        CHECK(db == NULL);
    }

    /* NULL arguments. */
    {
        McmeLevelDb *db = (McmeLevelDb *)0x1;
        CHECK(!mcme_leveldb_open(&db, NULL));
        CHECK(db == NULL);
        CHECK(!mcme_leveldb_open(NULL, "x"));
    }

    /* close(NULL) is a no-op, and the accessors tolerate NULL because callers
       reach for them after a failed open to build an error message. */
    mcme_leveldb_close(NULL);
    CHECK(mcme_leveldb_count(NULL) == 0);
    CHECK(mcme_leveldb_has(NULL, "x", 1) == false);
    CHECK(mcme_leveldb_table_count(NULL) == 0);
    CHECK(mcme_leveldb_log_count(NULL) == 0);
    CHECK(mcme_leveldb_comparator(NULL) == NULL);
    CHECK(mcme_leveldb_iterate(NULL, capture_entry, NULL) == 0);
}

/* ========================================================================
 * Writing
 * ======================================================================== */

/* Copies a whole db/ directory to `dst` so a write test never touches the
   fixture. Only regular files; the store layout is flat. */
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

static void test_write(void)
{
    const char *src = "tests/golden/worlds/bedrock-forge/db";
    if (!mcme_fs_exists(src)) {
        printf("  SKIP write tests (%s not found)\n", src);
        return;
    }

    char tmp[] = "/tmp/opencode/mcme_ldb_write_XXXXXX";
    CHECK(mkdtemp(tmp) != NULL);
    char dbdir[512];
    snprintf(dbdir, sizeof dbdir, "%s/db", tmp);
    CHECK(copy_store(src, dbdir));

    /* --- A read-only handle refuses to write, and says so. --- */
    {
        McmeLevelDb *db = NULL;
        CHECK(mcme_leveldb_open(&db, dbdir));
        CHECK(!mcme_leveldb_put(db, "k", 1, "v", 1));
        CHECK(mcme_last_error()[0] != '\0');
        mcme_leveldb_close(db);
    }

    /* --- put, then read the same key back through the same handle. --- */
    {
        McmeLevelDb *db = NULL;
        CHECK(mcme_leveldb_open_rw(&db, dbdir));

        size_t before = mcme_leveldb_count(db);
        CHECK(mcme_leveldb_put(db, "write/new", 9, "hello", 5));

        McmeBuffer buf;
        mcme_buffer_init(&buf);
        bool deleted = true;
        CHECK(mcme_leveldb_get(db, "write/new", 9, &buf, &deleted));
        CHECK(!deleted);
        CHECK(buf.len == 5 && memcmp(buf.data, "hello", 5) == 0);
        mcme_buffer_free(&buf);
        CHECK(mcme_leveldb_count(db) == before + 1);

        /* --- Overwrite through the same handle: newest wins. --- */
        CHECK(mcme_leveldb_put(db, "write/new", 9, "world", 5));
        mcme_buffer_init(&buf);
        CHECK(mcme_leveldb_get(db, "write/new", 9, &buf, &deleted));
        CHECK(buf.len == 5 && memcmp(buf.data, "world", 5) == 0);
        mcme_buffer_free(&buf);

        /* --- Empty value is legal and is not a deletion. --- */
        CHECK(mcme_leveldb_put(db, "write/empty", 10, NULL, 0));
        mcme_buffer_init(&buf);
        deleted = true;
        CHECK(mcme_leveldb_get(db, "write/empty", 10, &buf, &deleted));
        CHECK(!deleted && buf.len == 0);
        mcme_buffer_free(&buf);

        /* --- Delete a key that lives in the table: the tombstone must beat
               it, and the fixture's own log overwrite of the same key must
               not resurrect it either. `edge/single` is exactly such a key:
               in the table, overwritten in the fixture's log, still live. --- */
        CHECK(mcme_leveldb_has(db, "edge/single", 11));
        CHECK(mcme_leveldb_delete(db, "edge/single", 11));
        mcme_buffer_init(&buf);
        deleted = false;
        CHECK(mcme_leveldb_get(db, "edge/single", 11, &buf, &deleted));
        CHECK(deleted && buf.len == 0);
        mcme_buffer_free(&buf);
        /* has() counts a tombstone as present — the store does know the key —
           which is the documented answer, not an accident of the merge. */
        CHECK(mcme_leveldb_has(db, "edge/single", 11));

        /* --- A batch applies as one unit, later entries newer. --- */
        {
            McmeLevelOp ops[3];
            ops[0] = (McmeLevelOp){ "write/batch", 11, "one", 3, false };
            ops[1] = (McmeLevelOp){ "write/batch", 11, "two", 3, false };
            ops[2] = (McmeLevelOp){ "write/gone", 10, NULL, 0, false };
            ops[2].deleted = true;
            CHECK(mcme_leveldb_write(db, ops, 3));

            mcme_buffer_init(&buf);
            CHECK(mcme_leveldb_get(db, "write/batch", 11, &buf, &deleted));
            CHECK(buf.len == 3 && memcmp(buf.data, "two", 3) == 0);
            mcme_buffer_free(&buf);

            mcme_buffer_init(&buf);
            deleted = false;
            CHECK(mcme_leveldb_get(db, "write/gone", 10, &buf, &deleted));
            CHECK(deleted && buf.len == 0);
            mcme_buffer_free(&buf);
        }

        /* --- Refusals: NULL key, empty batch. --- */
        CHECK(!mcme_leveldb_put(db, NULL, 1, "v", 1));
        McmeLevelOp none = { "x", 1, "y", 1, false };
        CHECK(!mcme_leveldb_write(db, &none, 0));

        mcme_leveldb_close(db);
    }

    /* --- Reopen read-only: everything the writable handle wrote is there,
           which is the whole point of writing records rather than state. --- */
    {
        McmeLevelDb *db = NULL;
        CHECK(mcme_leveldb_open(&db, dbdir));

        McmeBuffer buf;
        mcme_buffer_init(&buf);
        bool deleted = true;
        CHECK(mcme_leveldb_get(db, "write/new", 9, &buf, &deleted));
        CHECK(!deleted);
        CHECK(buf.len == 5 && memcmp(buf.data, "world", 5) == 0);
        mcme_buffer_free(&buf);

        mcme_buffer_init(&buf);
        CHECK(mcme_leveldb_get(db, "write/empty", 10, &buf, &deleted));
        CHECK(!deleted && buf.len == 0);
        mcme_buffer_free(&buf);

        mcme_buffer_init(&buf);
        deleted = false;
        CHECK(mcme_leveldb_get(db, "write/gone", 10, &buf, &deleted));
        CHECK(deleted);
        mcme_buffer_free(&buf);

        mcme_leveldb_close(db);
    }

    /* --- The LOCK is exclusive: a second writable handle refuses. --- */
    {
        McmeLevelDb *a = NULL, *b = NULL;
        CHECK(mcme_leveldb_open_rw(&a, dbdir));
        CHECK(!mcme_leveldb_open_rw(&b, dbdir));
        CHECK(b == NULL);
        CHECK(mcme_last_error()[0] != '\0');
        mcme_leveldb_close(a);

        /* And after close, the lock is gone: flock dies with the fd. */
        CHECK(mcme_leveldb_open_rw(&b, dbdir));
        mcme_leveldb_close(b);
    }

    remove_store(dbdir);
    rmdir(tmp);
}

/* ========================================================================
 * Entry point
 * ======================================================================== */

int mcme_test_leveldb(void)
{
    g_failed = 0;

    printf("  varints\n");
    test_varint();

    printf("  crc32c\n");
    test_crc();

    printf("  internal keys\n");
    test_internal_key();

    printf("  log framing\n");
    test_log();

    printf("  refusals\n");
    test_refusals();

    printf("  write\n");
    test_write();

    test_store();

    return g_failed;
}
