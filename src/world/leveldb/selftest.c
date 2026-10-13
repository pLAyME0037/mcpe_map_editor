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
#include <sys/stat.h>
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
 * Compaction
 *
 * The property under test is the one the wiki entry promises: compaction
 * changes the store's shape, never its answers. Everything compared across a
 * compaction is copied out first, because compaction rebuilds the buffers a
 * view's pointers live in — a borrowed pointer taken before one is exactly
 * the mistake this test must not make.
 * ======================================================================== */

typedef struct {
    uint8_t *key; size_t klen;
    uint8_t *val; size_t vlen;
    uint64_t seq;
} OwnedEntry;

static bool capture_owned(void *ud, const McmeLevelEntry *e)
{
    OwnedEntry **out = (OwnedEntry **)ud;
    OwnedEntry o;
    o.klen = e->key_len;
    o.vlen = e->val_len;
    o.seq = e->sequence;
    o.key = (uint8_t *)malloc(o.klen > 0 ? o.klen : 1);
    o.val = (uint8_t *)malloc(o.vlen > 0 ? o.vlen : 1);
    if (o.key == NULL || o.val == NULL) {
        free(o.key);
        free(o.val);
        return false;
    }
    memcpy(o.key, e->key, o.klen);
    memcpy(o.val, e->val, o.vlen);
    arrput(*out, o);
    return true;
}

static void free_owned(OwnedEntry *a)
{
    for (size_t i = 0; i < (size_t)arrlen(a); ++i) {
        free(a[i].key);
        free(a[i].val);
    }
    arrfree(a);
}

/* Two views are the same store: same keys, same values, same sequences.
   Sequence equality is not gold-plating — a compaction that renumbered
   entries would answer the same lookups today and diverge the moment the
   game wrote a key both copies contain. */
static bool same_view(const OwnedEntry *a, size_t an,
                      const OwnedEntry *b, size_t bn)
{
    if (an != bn) return false;
    for (size_t i = 0; i < an; ++i) {
        if (a[i].klen != b[i].klen || a[i].vlen != b[i].vlen) return false;
        if (a[i].seq != b[i].seq) return false;
        if (a[i].klen > 0 && memcmp(a[i].key, b[i].key, a[i].klen) != 0) return false;
        if (a[i].vlen > 0 && memcmp(a[i].val, b[i].val, a[i].vlen) != 0) return false;
    }
    return true;
}

/* How many files in `dir` end in `suffix`. The shape a compaction promises is
   observable from the shell: one table, no logs. */
static size_t count_files_with(const char *dir, const char *suffix)
{
    DIR *d = opendir(dir);
    if (d == NULL) return (size_t)-1;

    size_t n = 0;
    size_t sl = strlen(suffix);
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        size_t nl = strlen(e->d_name);
        if (nl >= sl && strcmp(e->d_name + nl - sl, suffix) == 0) n++;
    }
    closedir(d);
    return n;
}

/* Reads key into buf, asserts it is a live value equal to `want`. */
static bool expect_value(McmeLevelDb *db, const char *key, const char *want)
{
    McmeBuffer buf;
    mcme_buffer_init(&buf);
    bool deleted = true;
    bool ok = mcme_leveldb_get(db, key, strlen(key), &buf, &deleted);
    ok = ok && !deleted && buf.len == strlen(want) &&
         memcmp(buf.data, want, buf.len) == 0;
    mcme_buffer_free(&buf);
    return ok;
}

/* Reads key into buf, asserts it is a tombstone. */
static bool expect_deleted(McmeLevelDb *db, const char *key)
{
    McmeBuffer buf;
    mcme_buffer_init(&buf);
    bool deleted = false;
    bool ok = mcme_leveldb_get(db, key, strlen(key), &buf, &deleted);
    ok = ok && deleted && buf.len == 0;
    mcme_buffer_free(&buf);
    return ok;
}

/* Finds a store's single .ldb file, for the tests below and for the big-table
   test further down. */
static bool find_table(const char *dir, char *out, size_t cap)
{
    DIR *d = opendir(dir);
    if (d == NULL) return false;

    struct dirent *e;
    bool found = false;
    while (!found && (e = readdir(d)) != NULL) {
        size_t n = strlen(e->d_name);
        if (n > 4 && strcmp(e->d_name + n - 4, ".ldb") == 0) {
            snprintf(out, cap, "%s/%s", dir, e->d_name);
            found = true;
        }
    }
    closedir(d);
    return found;
}

static void test_compact(void)
{
    const char *src = "tests/golden/worlds/bedrock-forge/db";
    if (!mcme_fs_exists(src)) {
        printf("  SKIP compact tests (%s not found)\n", src);
        return;
    }

    char tmp[] = "/tmp/opencode/mcme_ldb_compact_XXXXXX";
    CHECK(mkdtemp(tmp) != NULL);
    char dbdir[512];
    snprintf(dbdir, sizeof dbdir, "%s/db", tmp);
    CHECK(copy_store(src, dbdir));

    OwnedEntry *before = NULL;   /* the view as the fixture + edits left it */
    OwnedEntry *after1 = NULL;   /* after the first compaction */
    OwnedEntry *after2 = NULL;   /* after a post-compaction write + compact */
    OwnedEntry *reopen = NULL;   /* what a fresh handle sees at the end */

    /* Payloads the storage choice in block_builder_finish has to see
       (decisions.md#D14): one compressible enough to land bare DEFLATE, one
       printable but noisy enough to stay raw. Both must survive every
       compaction below byte for byte. */
    /* One byte past each payload holds a NUL: expect_value measures with
       strlen, the payloads themselves are stored without it. */
    size_t big_len = 65536;
    char *big = (char *)malloc(big_len + 1);
    CHECK(big != NULL);
    if (big != NULL) {
        memset(big, 'a', big_len);
        big[big_len] = '\0';
    }

    size_t noise_len = 16384;
    char *noise = (char *)malloc(noise_len + 1);
    CHECK(noise != NULL);
    if (noise != NULL) {
        uint32_t x = 12345;
        for (size_t i = 0; i < noise_len; ++i) {
            x = x * 1103515245u + 12345u;
            noise[i] = (char)(' ' + (x >> 16) % 95);
        }
        noise[noise_len] = '\0';
    }

    /* --- A read-only handle refuses to compact, and says so. --- */
    {
        McmeLevelDb *db = NULL;
        CHECK(mcme_leveldb_open(&db, dbdir));
        CHECK(!mcme_leveldb_compact(db));
        CHECK(mcme_last_error()[0] != '\0');
        mcme_leveldb_close(db);
    }

    /* --- Edit, then compact: the answers do not change, the shape does. --- */
    {
        McmeLevelDb *db = NULL;
        CHECK(mcme_leveldb_open_rw(&db, dbdir));

        CHECK(mcme_leveldb_put(db, "compact/a", 9, "alpha", 5));
        if (big != NULL) {
            CHECK(mcme_leveldb_put(db, "compact/big", sizeof("compact/big") - 1,
                                   big, big_len));
        }
        if (noise != NULL) {
            CHECK(mcme_leveldb_put(db, "compact/noise",
                                   sizeof("compact/noise") - 1,
                                   noise, noise_len));
        }
        /* `edge/single` lives in the fixture's table and was overwritten in
           its log; deleting it puts a tombstone in the log, and compaction
           must carry that tombstone into the table rather than drop it and
           resurrect whatever sits underneath. */
        CHECK(mcme_leveldb_has(db, "edge/single", 11));
        CHECK(mcme_leveldb_delete(db, "edge/single", 11));

        size_t tables_before = mcme_leveldb_table_count(db);
        CHECK(tables_before >= 1);
        CHECK(mcme_leveldb_log_count(db) >= 1);

        mcme_leveldb_iterate(db, capture_owned, &before);
        CHECK(before != NULL && (size_t)arrlen(before) > 0);

        CHECK(mcme_leveldb_compact(db));

        /* The shape the wiki promises: one table, no logs, both on disk. */
        CHECK(mcme_leveldb_table_count(db) == 1);
        CHECK(mcme_leveldb_log_count(db) == 0);
        CHECK(count_files_with(dbdir, ".log") == 0);
        CHECK(count_files_with(dbdir, ".ldb") == 1);
        CHECK(count_files_with(dbdir, ".sst") == 0);

        /* The answers: byte-identical view, sequences included. */
        mcme_leveldb_iterate(db, capture_owned, &after1);
        CHECK(same_view(before, (size_t)arrlen(before),
                        after1, (size_t)arrlen(after1)));

        CHECK(expect_value(db, "compact/a", "alpha"));
        CHECK(expect_value(db, "compact/big", big));
        CHECK(expect_value(db, "compact/noise", noise));
        CHECK(expect_deleted(db, "edge/single"));
        CHECK(mcme_leveldb_has(db, "edge/single", 11));

        /* The table must be smaller than the compressible payload on its
           own: the blocks went out bare DEFLATE, not raw (D14). Without
           this the write path could quietly stop compressing and every
           check above would still pass. */
        {
            char table_path[600];
            struct stat st;
            bool statted = find_table(dbdir, table_path, sizeof table_path) &&
                           stat(table_path, &st) == 0;
            CHECK_MSG(statted, "no statable .ldb after compaction");
            CHECK_MSG(!statted || (size_t)st.st_size < big_len,
                      "compacted table is %zu bytes — blocks stored raw?",
                      statted ? (size_t)st.st_size : (size_t)0);
        }

        mcme_leveldb_close(db);
    }

    /* --- Reopen: the compaction is in the MANIFEST, not in that handle's
           memory. Then write again and compact again — twice has to be as
           safe as once, because the auto-trigger will do it repeatedly. --- */
    {
        McmeLevelDb *db = NULL;
        CHECK(mcme_leveldb_open(&db, dbdir));

        OwnedEntry *seen = NULL;
        mcme_leveldb_iterate(db, capture_owned, &seen);
        CHECK(same_view(after1, (size_t)arrlen(after1),
                        seen, (size_t)arrlen(seen)));
        free_owned(seen);

        mcme_leveldb_close(db);

        CHECK(mcme_leveldb_open_rw(&db, dbdir));
        CHECK(mcme_leveldb_put(db, "compact/b", 9, "beta", 4));
        CHECK(mcme_leveldb_log_count(db) == 1);   /* a fresh log, lazily made */

        /* Exactly one more live key than before the write... */
        OwnedEntry *mid = NULL;
        mcme_leveldb_iterate(db, capture_owned, &mid);
        CHECK(mid != NULL && (size_t)arrlen(mid) == (size_t)arrlen(after1) + 1);

        CHECK(mcme_leveldb_compact(db));
        CHECK(mcme_leveldb_table_count(db) == 1);
        CHECK(mcme_leveldb_log_count(db) == 0);

        /* ...and the second compaction changed none of them. The comparison
           is against `mid`, not against `after1` plus an offset: where a key
           sorts is the merge's business, and the test's business is only
           that compaction is not it. */
        mcme_leveldb_iterate(db, capture_owned, &after2);
        CHECK(same_view(mid, (size_t)arrlen(mid),
                        after2, (size_t)arrlen(after2)));
        free_owned(mid);
        CHECK(expect_value(db, "compact/a", "alpha"));
        CHECK(expect_value(db, "compact/b", "beta"));
        CHECK(expect_value(db, "compact/big", big));
        CHECK(expect_value(db, "compact/noise", noise));
        CHECK(expect_deleted(db, "edge/single"));

        mcme_leveldb_close(db);
    }

    /* --- Final reopen: everything, twice compacted, still reads back. --- */
    {
        McmeLevelDb *db = NULL;
        CHECK(mcme_leveldb_open(&db, dbdir));

        mcme_leveldb_iterate(db, capture_owned, &reopen);
        CHECK(same_view(after2, (size_t)arrlen(after2),
                        reopen, (size_t)arrlen(reopen)));
        CHECK(expect_value(db, "compact/b", "beta"));
        CHECK(expect_value(db, "compact/big", big));
        CHECK(expect_value(db, "compact/noise", noise));
        CHECK(expect_deleted(db, "edge/single"));

        mcme_leveldb_close(db);
    }

    free(big);
    free(noise);

    free_owned(before);
    free_owned(after1);
    free_owned(after2);
    free_owned(reopen);

    remove_store(dbdir);
    rmdir(tmp);
}

/* ========================================================================
 * A table past what a whole-file reader would take
 *
 * The reader used to load the whole .ldb into memory and refuse anything over
 * 512 MiB — which meant refusing tables this directory's own compaction
 * writes, since a compaction writes the store as one table. The test splices
 * a hole before the footer so the file reports over that ceiling while every
 * handle still names its own offset: the size is what is under test, not the
 * volume, and a hole costs nothing to write or read.
 * ======================================================================== */

/* Rewrites the table with `hole` bytes of hole immediately before its
   footer: every block handle still names its own offset, the footer is still
   the last 48 bytes, and the file now reports over the ceiling the old
   whole-file read enforced. fseek past EOF then one write is a hole. */
static bool grow_table(const char *path, size_t hole)
{
    void *data = NULL;
    size_t len = 0;
    if (!mcme_fs_read(path, &data, &len)) return false;
    if (len < MCME_LDB_FOOTER_SIZE) {
        free(data);
        return false;
    }

    FILE *f = fopen(path, "wb");
    if (f == NULL) {
        free(data);
        return false;
    }
    bool ok = fwrite(data, 1, len - MCME_LDB_FOOTER_SIZE, f) ==
              len - MCME_LDB_FOOTER_SIZE;
    ok = ok && fseek(f, (long)hole, SEEK_CUR) == 0;
    ok = ok && fwrite((uint8_t *)data + len - MCME_LDB_FOOTER_SIZE, 1,
                      MCME_LDB_FOOTER_SIZE, f) == MCME_LDB_FOOTER_SIZE;
    if (fclose(f) != 0) ok = false;
    free(data);
    return ok;
}

/* Rewrites the footer's index handle so it claims nearly the whole file:
   still inside the file's bounds — so the bounds guard passes — but over the
   per-block ceiling, which is what has to refuse it. Slack of eight bytes
   keeps the trailer guard passing too, so the refusal can only come from the
   size. */
static bool forge_index_size(const char *path)
{
    FILE *f = fopen(path, "r+b");
    if (f == NULL) return false;

    bool ok = fseek(f, 0, SEEK_END) == 0;
    long end = ok ? ftell(f) : -1;
    ok = ok && end >= (long)MCME_LDB_FOOTER_SIZE &&
         fseek(f, end - MCME_LDB_FOOTER_SIZE, SEEK_SET) == 0;

    uint8_t footer[MCME_LDB_FOOTER_SIZE];
    ok = ok && fread(footer, 1, MCME_LDB_FOOTER_SIZE, f) ==
                MCME_LDB_FOOTER_SIZE;

    uint64_t meta_off = 0, meta_size = 0, index_off = 0;
    const uint8_t *p = footer;
    const uint8_t *limit = footer + MCME_LDB_FOOTER_SIZE - 8;
    if (ok) {
        size_t n = mcme_ldb_varint_get(p, limit, &meta_off);
        p += n;
        ok = n != 0;
        n = ok ? mcme_ldb_varint_get(p, limit, &meta_size) : 0;
        p += n;
        ok = ok && n != 0;
        n = ok ? mcme_ldb_varint_get(p, limit, &index_off) : 0;
        ok = ok && n != 0;
    }
    uint64_t claimed = ok ? (uint64_t)end - index_off - 8 : 0;

    McmeBuffer out;
    mcme_buffer_init(&out);
    if (ok) {
        ok = mcme_ldb_varint_put(&out, meta_off) &&
             mcme_ldb_varint_put(&out, meta_size) &&
             mcme_ldb_varint_put(&out, index_off) &&
             mcme_ldb_varint_put(&out, claimed);
        while (ok && out.len < MCME_LDB_FOOTER_SIZE - 8) {
            ok = mcme_buffer_append_u8(&out, 0);
        }
        for (int i = 0; ok && i < 8; ++i) {
            ok = mcme_buffer_append_u8(&out,
                 (uint8_t)((MCME_LDB_MAGIC >> (i * 8)) & 0xff));
        }
    }
    if (ok) ok = fseek(f, end - MCME_LDB_FOOTER_SIZE, SEEK_SET) == 0;
    if (ok) ok = fwrite(out.data, 1, out.len, f) == out.len;
    mcme_buffer_free(&out);
    if (fclose(f) != 0) ok = false;
    return ok;
}

static void test_big_table(void)
{
    const char *src = "tests/golden/worlds/bedrock-forge/db";
    if (!mcme_fs_exists(src)) {
        printf("  SKIP big table test (%s not found)\n", src);
        return;
    }

    char tmp[] = "/tmp/opencode/mcme_ldb_big_XXXXXX";
    CHECK(mkdtemp(tmp) != NULL);
    char dbdir[512];
    snprintf(dbdir, sizeof dbdir, "%s/db", tmp);
    CHECK(copy_store(src, dbdir));

    char table_path[600];
    CHECK_MSG(find_table(dbdir, table_path, sizeof table_path),
              "no .ldb in the copied store");

    /* The view before the splice: the grown file must read back exactly
       this, byte for byte, sequences included. */
    OwnedEntry *before = NULL;
    {
        McmeLevelDb *db = NULL;
        CHECK(mcme_leveldb_open(&db, dbdir));
        if (db != NULL) {
            mcme_leveldb_iterate(db, capture_owned, &before);
            mcme_leveldb_close(db);
        }
        CHECK(before != NULL && (size_t)arrlen(before) > 0);
    }

    /* --- Grow past the ceiling: the store opens and answers with the same
           view. Under the old whole-file read this open refused outright. */
    {
        const size_t hole = (size_t)512u * 1024u * 1024u;
        CHECK(grow_table(table_path, hole));

        struct stat st;
        CHECK_MSG(stat(table_path, &st) == 0 &&
                  (size_t)st.st_size > (size_t)512u * 1024u * 1024u,
                  "table did not grow past the ceiling");

        McmeLevelDb *db = NULL;
        CHECK_MSG(mcme_leveldb_open(&db, dbdir),
                  "open of the grown store failed: %s", mcme_last_error());
        if (db != NULL) {
            OwnedEntry *seen = NULL;
            mcme_leveldb_iterate(db, capture_owned, &seen);
            CHECK_MSG(same_view(before, (size_t)arrlen(before),
                                seen, (size_t)arrlen(seen)),
                      "view changed after growing the table");
            free_owned(seen);
            mcme_leveldb_close(db);
        }
    }

    /* --- The forged index handle: in bounds but over the block ceiling, so
           the open must refuse it — and say the size is why. */
    {
        CHECK(forge_index_size(table_path));

        McmeLevelDb *db = NULL;
        CHECK(!mcme_leveldb_open(&db, dbdir));
        CHECK(db == NULL);
        CHECK_MSG(strstr(mcme_last_error(), "over the") != NULL,
                  "expected the block-size ceiling, got: %s",
                  mcme_last_error());
    }

    free_owned(before);
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

    printf("  compact\n");
    test_compact();

    printf("  big table\n");
    test_big_table();

    test_store();

    return g_failed;
}
