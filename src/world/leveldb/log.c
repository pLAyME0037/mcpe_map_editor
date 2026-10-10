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
