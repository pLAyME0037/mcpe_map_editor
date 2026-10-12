/*
 * mcme Java Anvil region reader.
 *
 * A Minecraft Java world stores chunks in 32x32-chunk region files named
 * r.<rx>.<rz>.mca, where (rx, rz) is floor(chunk / 32). Each region file is a
 * flat, sector-allocated container:
 *
 *   bytes      0 ..  4095   location table: 1024 entries of 3 bytes
 *                           (sector offset, big-endian 24-bit) + 1 byte
 *                           (sector count). Entry i addresses chunk i.
 *   bytes   4096 ..  8191   timestamps: 1024 entries of 4 bytes, big-endian,
 *                           seconds since the Unix epoch.
 *   bytes   8192 ..         chunk payloads, each starting on a 4096-byte
 *                           sector boundary.
 *
 * Chunk i is at index (cx & 31) + (cz & 31) * 32. An entry whose sector offset
 * is zero means "no chunk here" — that is how a region advertises holes.
 *
 * A chunk payload is:
 *   4 bytes  length, big-endian, counting the compression byte plus the
 *            compressed data (so the data itself is length - 1 bytes)
 *   1 byte   compression: 1 = gzip, 2 = zlib, 3 = stored uncompressed,
 *            127 = the data lives in an external file named by the following
 *            bytes
 *   N bytes  the compressed NBT for the chunk
 *
 * The reader loads the whole region file into memory. A region is at most
 * ~32 MiB (8192 sectors), so this is a bounded, simple choice. The writer
 * appends the new payload after the file's current end and repoints the
 * slot's entry, touching nothing else — sector allocation in place is what
 * a compactor is for. See wiki/world.md.
 */

#ifndef MCME_REGION_H
#define MCME_REGION_H

#include "mcme/compress.h"
#include "mcme/core.h"
#include "mcme/nbt.h"
/* The write-refusal vocabulary every write verb answers with — one
   condition, one exit code across the CLI. It lives in bedrock_write.h
   because that is where it was born; region carries no Bedrock
   dependency beyond the name. */
#include "mcme/bedrock_write.h"

#define MCME_REGION_SECTOR     4096u
#define MCME_REGION_ENTRIES    1024u   /* 32 x 32 chunks */
#define MCME_REGION_HEADER     (2u * MCME_REGION_SECTOR)
#define MCME_REGION_MAX_SECTORS 8192u  /* 4096-byte sectors per region file */

/* Compression byte values found in a chunk header. */
#define MCME_ANVIL_COMP_GZIP     1
#define MCME_ANVIL_COMP_ZLIB     2
#define MCME_ANVIL_COMP_STORED   3
#define MCME_ANVIL_COMP_EXTERNAL 127

typedef struct {
    uint32_t sector_offset;  /* in 4096-byte sectors; 0 means "absent" */
    uint8_t  sector_count;
} McmeChunkLoc;

typedef struct {
    uint8_t *data;           /* the whole file; owned */
    size_t   size;
    char    *path;           /* owned, for error messages */
} McmeRegion;

/* Loads and validates the region header. Fails on a file shorter than the
   8 KiB header, or any location entry pointing outside the file. */
bool mcme_region_open(McmeRegion *r, const char *path);
void mcme_region_close(McmeRegion *r);

/* Chunk coordinates may be any int; only the low 5 bits select the slot, so a
   caller can pass world coordinates directly. */
bool     mcme_region_chunk_present(const McmeRegion *r, int cx, int cz);
McmeChunkLoc mcme_region_chunk_loc(const McmeRegion *r, int cx, int cz);
uint32_t mcme_region_chunk_timestamp(const McmeRegion *r, int cx, int cz);

/* How many chunks the region actually holds. */
size_t mcme_region_chunk_count(const McmeRegion *r);

/*
 * Enumerate the chunks that are present, writing their local slot coordinates
 * (0..31 in each axis) into `out_slots`. Returns how many were written.
 * `cap` bounds the write. Pass a 1024-entry array to enumerate everything.
 */
size_t mcme_region_slots(const McmeRegion *r, int *out_slots, size_t cap);

/*
 * What the container says about one chunk without opening it: the
 * five-byte payload header. `length` is the payload's own byte count —
 * the file's length field minus the compression byte it counts — and
 * `compression` is the file's byte, one of MCME_ANVIL_COMP_*. A byte
 * mcme does not know is reported, not refused: an inventory answers
 * what the file holds, and read_chunk is what decides what mcme can
 * open.
 *
 * Returns false when the slot holds nothing, or when the header is one
 * mcme cannot read — a length of zero, or a payload that would run
 * past the end of the file — with mcme_last_error() saying which.
 */
typedef struct {
    uint32_t length;      /* payload bytes, from the header       */
    uint8_t  compression; /* the file's own compression byte      */
} McmeChunkPayload;

bool mcme_region_chunk_payload(const McmeRegion *r, int cx, int cz,
                               McmeChunkPayload *out);

/*
 * Extract one chunk: decompress its payload into `out` (appended) and report
 * which codec was used in `out_codec` when non-NULL.
 *
 * `out` receives the raw NBT bytes, ready for mcme_nbt_parse.
 *
 * A ceiling of MCME_REGION_CHUNK_MAX applies: a chunk payload claiming to
 * expand past it is refused rather than inflated. Real chunks are far smaller.
 */
#define MCME_REGION_CHUNK_MAX ((size_t)32u * 1024u * 1024u)

bool mcme_region_read_chunk(const McmeRegion *r, int cx, int cz,
                            McmeCompression *out_codec, McmeBuffer *out);

/* Path helpers. `out` must be at least MCME_REGION_PATH_MAX. */
#define MCME_REGION_PATH_MAX 4096
const char *mcme_region_path(char *out, size_t cap,
                             const char *world_dir, const char *dimension,
                             int rx, int rz);

/*
 * The region file a chunk belongs to: floor(chunk / 32) on each axis, the
 * sharding rule the file names are written by. Either output may be NULL
 * when that axis is not wanted.
 */
void mcme_region_of(int cx, int cz, int *out_rx, int *out_rz);

/*
 * Replaces one chunk's payload — the write half of the reader above.
 *
 * `file` is the full path of the region file (build it with
 * mcme_region_path()); `root` is the chunk's document, rooted in a
 * compound; `arena` is the one that owns `root`. `timestamp` goes into
 * the slot's timestamp entry verbatim — the caller passes the wall
 * clock, a test passes a fixed value.
 *
 * The new payload is compressed as zlib (the codec Java writes),
 * appended after the file's current end (zero-padded to a sector
 * boundary first), and the slot's location entry repointed at it.
 * Nothing else in the file is touched: the old payload becomes slack
 * for a compactor to reclaim. A slot that holds nothing gets a chunk,
 * and a region file that does not exist is created with its 8 KiB
 * header — but the region directory must already be there: mcme writes
 * into a store the world has, it does not invent one, and it will not
 * rewrite a region file it cannot read.
 *
 * Returns false with mcme_last_error() saying what was refused, and
 * `refusal` (when not NULL) which class: USAGE for a null argument, a
 * non-compound root, or a payload the reader's own ceiling would
 * refuse; NOT_THERE when the region directory is missing;
 * NOT_READABLE when the file exists but is not a region mcme reads;
 * STORE for the file's own failures; INTERNAL for encoding and
 * arithmetic.
 */
bool mcme_region_set_chunk(const char *file, int cx, int cz,
                           const McmeTag *root, McmeArena *arena,
                           uint32_t timestamp, McmeWriteRefusal *refusal);

#endif /* MCME_REGION_H */
