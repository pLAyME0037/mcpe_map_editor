/*
 * LevelDB log reader.
 *
 * A log file is a sequence of 32 KiB blocks. Each block holds as many record
 * fragments as fit:
 *
 *   4 bytes  masked CRC32C of the record's type byte and payload, little-endian
 *   2 bytes  fragment length, little-endian
 *   1 byte   type: 1 FULL, 2 FIRST, 3 MIDDLE, 4 LAST
 *   n bytes  fragment payload
 *
 * A record larger than a block is split across fragments. Each fragment carries
 * its own checksum, computed over that fragment's type byte and payload; the
 * checksum is NOT a property of the whole logical record. If a fragment header
 * would not fit in the remaining space, the rest of the block is skipped and
 * the record continues at the next block boundary.
 *
 * A MANIFEST is a log too, so this reader serves both. What a record means is
 * decided by the caller.
 *
 * Corruption policy: a bad checksum, an unknown type, or a length that runs
 * past the end of the block ends the walk there. LevelDB does the same — the
 * tail of a log is expected to be unfinished, since the writer may have been
 * killed mid-record — so stopping is correct behaviour, not a fallback.
 */

#include "internal.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Fragment types. */
#define LDB_REC_FULL    1
#define LDB_REC_FIRST   2
#define LDB_REC_MIDDLE  3
#define LDB_REC_LAST    4

/* Hard ceiling on one reassembled record. A legitimate record never approaches
   this; it exists so a corrupt length cannot drive an unbounded allocation. */
#define LDB_RECORD_MAX (64u * 1024u * 1024u)

bool mcme_ldb_log_read(const uint8_t *data, size_t len,
                       McmeLdbRecordFn fn, void *ud, size_t *out_records)
{
    if (data == NULL || fn == NULL) {
        mcme_set_error("leveldb log: NULL argument");
        return false;
    }

    McmeBuffer record;      /* reassembled logical record */
    mcme_buffer_init(&record);
    bool in_fragment = false;
    size_t records = 0;
    bool ok = true;

    for (size_t block_at = 0; block_at < len; block_at += MCME_LDB_BLOCK_SIZE) {
        size_t block_end = block_at + MCME_LDB_BLOCK_SIZE;
        if (block_end > len) block_end = len;

        size_t at = block_at;
        while (at + 7 <= block_end) {
            uint32_t crc = (uint32_t)data[at]         | ((uint32_t)data[at + 1] << 8) |
                           ((uint32_t)data[at + 2] << 16) | ((uint32_t)data[at + 3] << 24);
            uint32_t frag_len = (uint32_t)data[at + 4] | ((uint32_t)data[at + 5] << 8);
            uint8_t type = data[at + 6];
            at += 7;

            /* A fragment must fit in the block it starts in. LevelDB writes it
               that way, so anything else is damage. */
            if (frag_len == 0 || at + frag_len > block_end) {
                /* A short or malformed tail is normal on a log the writer was
                   killed while appending. Stop cleanly, and do not report it as
                   an error: callers rely on reading everything up to here. */
                goto done;
            }

            /* A type no writer emits. Unlike a short tail this is real damage:
               the writer puts the whole header on disk and then the payload,
               so a write interrupted partway leaves a valid header with a
               missing payload — which the length check above already treats as
               a clean end of file. A well-formed header naming an impossible
               type is something else, and says so. */
            if (type != LDB_REC_FULL && type != LDB_REC_FIRST &&
                type != LDB_REC_MIDDLE && type != LDB_REC_LAST) {
                mcme_set_error("leveldb log: unknown record type %u at byte %zu",
                               (unsigned)type, block_at);
                ok = false;
                goto done;
            }

            const uint8_t *payload = data + at;
            at += frag_len;

            /* The checksum covers THIS fragment's type byte followed by its
               own payload — not the whole reassembled logical record. Both
               halves of that matter, and both are easy to get wrong:

                 - Coverage. LevelDB's writer extends over (&type, 1) and then
                   the payload. A reader that checksums the payload alone
                   rejects every record a modern LevelDB wrote, and the file
                   gives no hint that it is doing so.

                 - Scope. Because a record is fragmented only when it outgrows
                   a block, a reader that waits until the record is complete
                   and then checksums the whole thing cannot use this format at
                   all. Verifying per fragment is both correct and cheaper:
                   damage is caught before its bytes are appended.

               Verifying here also means an orphaned MIDDLE/LAST fragment — one
               whose FIRST was lost — still has its integrity checked before it
               is discarded. */
            uint32_t got = mcme_ldb_crc32c(0, &type, 1);
            got = mcme_ldb_crc32c(got, payload, frag_len);
            if (mcme_ldb_mask_crc(got) != crc) {
                /* Damaged fragment. Stop rather than hand the caller bytes
                   that may have been altered. */
                mcme_set_error("leveldb log: checksum mismatch at byte %zu", block_at);
                ok = false;
                goto done;
            }

            if (type == LDB_REC_FIRST) {
                /* A new logical record is starting. If one was already under
                   construction it was never terminated — the log was cut in the
                   middle, so drop it. */
                mcme_buffer_free(&record);
                mcme_buffer_init(&record);
                in_fragment = true;
            } else if (type == LDB_REC_FULL) {
                mcme_buffer_free(&record);
                mcme_buffer_init(&record);
                in_fragment = false;
            } else if (!in_fragment) {
                /* MIDDLE or LAST with nothing under construction: the FIRST
                   fragment was lost. Skip the orphan. */
                continue;
            }

            if (record.len + frag_len > LDB_RECORD_MAX) {
                mcme_set_error("leveldb log: record over %u bytes", LDB_RECORD_MAX);
                ok = false;
                goto done;
            }
            if (!mcme_buffer_append(&record, payload, frag_len)) {
                ok = false;
                goto done;
            }

            bool complete = (type == LDB_REC_FULL || type == LDB_REC_LAST);
            if (!complete) continue;

            in_fragment = false;

            if (!fn(ud, record.data, record.len)) {
                goto done;    /* caller asked to stop; not an error */
            }
            records++;
            mcme_buffer_free(&record);
            mcme_buffer_init(&record);
        }

        /* Fewer than 7 bytes left in this block: LevelDB treats the remainder
           as padding and continues in the next block. */
    }

done:
    mcme_buffer_free(&record);
    if (out_records != NULL) *out_records = records;
    return ok;
}

/* ========================================================================
 * Log writer
 *
 * The mirror of the reader above, and of upstream's Writer::AddRecord: one
 * logical record is emitted as fragments of at most (32768 - 7) bytes, and a
 * fragment that would not fit in the space left in this block is followed by
 * zero padding to the block end, with the record continuing in the next block.
 *
 * Two details are load-bearing and easy to get wrong in either direction:
 *
 *   - The writer NEVER leaves fewer than 7 bytes in a block. A reader that
 *     finds 3 leftover bytes cannot tell padding from a truncated header, and
 *     upstream solves it by writing exactly those 3 bytes and moving on.
 *   - Each fragment carries its own checksum over (its type byte, its payload)
 *     — never over the whole logical record. This is the same rule the reader
 *     verifies, and the fixture generator (tools/ref_leveldb_table.py) writes
 *     it independently, so all three agree or the tests say which one moved.
 *
 * `block_offset` is the file's current size modulo the block size: appending
 * to a log that already has records means resuming mid-block, exactly as
 * upstream's Writer(dest, dest_length) constructor does.
 */

/* Appends one physical fragment: 7-byte header, then payload. */
static bool emit_fragment(FILE *fh, uint8_t type,
                          const uint8_t *payload, size_t len)
{
    if (len > 0xffff) {
        mcme_set_error("leveldb log: fragment over 65535 bytes");
        return false;
    }

    uint8_t header[7];
    header[4] = (uint8_t)(len & 0xff);
    header[5] = (uint8_t)(len >> 8);
    header[6] = type;

    uint32_t crc = mcme_ldb_crc32c(0, &type, 1);
    crc = mcme_ldb_crc32c(crc, payload, len);
    crc = mcme_ldb_mask_crc(crc);
    header[0] = (uint8_t)(crc & 0xff);
    header[1] = (uint8_t)((crc >> 8) & 0xff);
    header[2] = (uint8_t)((crc >> 16) & 0xff);
    header[3] = (uint8_t)((crc >> 24) & 0xff);

    if (fwrite(header, 1, sizeof header, fh) != sizeof header) return false;
    if (len > 0 && fwrite(payload, 1, len, fh) != len) return false;
    return true;
}

bool mcme_ldb_log_append(FILE *fh, const void *record, size_t len,
                         size_t *block_offset)
{
    if (fh == NULL || block_offset == NULL || (record == NULL && len > 0)) {
        mcme_set_error("leveldb log: NULL argument");
        return false;
    }
    if (*block_offset >= MCME_LDB_BLOCK_SIZE) {
        mcme_set_error("leveldb log: block offset %zu past the block end",
                       *block_offset);
        return false;
    }

    const uint8_t *ptr = (const uint8_t *)record;
    size_t left = len;
    bool begin = true;

    do {
        size_t leftover = MCME_LDB_BLOCK_SIZE - *block_offset;
        if (leftover < 7) {
            /* Fill the trailer with exactly the bytes left, so the next
               fragment starts on a block boundary. Skipping them instead
               would leave a reader three bytes of nothing to parse. */
            static const uint8_t zeros[7] = {0};
            if (leftover > 0 &&
                fwrite(zeros, 1, leftover, fh) != leftover) {
                mcme_set_error("leveldb log: short write padding a block");
                return false;
            }
            *block_offset = 0;
            leftover = MCME_LDB_BLOCK_SIZE;
        }

        size_t avail = leftover - 7;
        size_t frag = left < avail ? left : avail;
        bool end = (left == frag);

        uint8_t type = begin && end ? LDB_REC_FULL
                     : begin        ? LDB_REC_FIRST
                     : end          ? LDB_REC_LAST
                                    : LDB_REC_MIDDLE;

        if (!emit_fragment(fh, type, ptr, frag)) {
            mcme_set_error("leveldb log: short write: %s", strerror(errno));
            return false;
        }

        ptr += frag;
        left -= frag;
        *block_offset += 7 + frag;
        begin = false;
    } while (left > 0);

    return true;
}
