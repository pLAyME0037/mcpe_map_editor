/*
 * mcme Java Anvil region reader. See include/mcme/region.h for the layout.
 *
 * The whole file is loaded once and every access is bounds-checked against
 * `size`. Region files are not trusted input: a hand-edited or corrupted .mca
 * is a normal thing for a map editor to be pointed at, and a hostile one is a
 * thing to survive.
 */

#include "mcme/region.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ========================================================================
 * Byte readers
 *
 * Anvil is big-endian throughout. These read out of a byte array rather than
 * via a packed struct so there is no alignment or aliasing question.
 * ======================================================================== */

static uint32_t rd_u24be(const uint8_t *p)
{
    return ((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | (uint32_t)p[2];
}

static uint32_t rd_u32be(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8)  | (uint32_t)p[3];
}

/* Slot index for a chunk coordinate. Only the low 5 bits matter, which is what
   lets callers pass world coordinates straight through. */
static size_t slot_for(int cx, int cz)
{
    return (size_t)((cx & 31) + (cz & 31) * 32);
}

/* ========================================================================
 * Open / close
 * ======================================================================== */

bool mcme_region_open(McmeRegion *r, const char *path)
{
    if (r == NULL || path == NULL) {
        mcme_set_error("region: NULL argument");
        return false;
    }

    memset(r, 0, sizeof *r);

    void *data = NULL;
    size_t size = 0;
    if (!mcme_fs_read(path, &data, &size)) {
        mcme_set_error("region: cannot read %s", path);
        return false;
    }

    if (size < MCME_REGION_HEADER) {
        mcme_set_error("region: %s is %zu bytes, shorter than the %u-byte header",
                       path, size, MCME_REGION_HEADER);
        free(data);
        return false;
    }

    /* Validate the location table before handing the file to anyone else. A
       slot that claims to live past the end of the file is a corruption we can
       detect now rather than a wild read later. Sector counts are NOT checked:
       real-world region files routinely carry stale counts, and refusing those
       would reject worlds that load fine everywhere else. The offset is what
       determines where we read, so the offset is what must be sound. */
    const uint8_t *loc = (const uint8_t *)data;
    for (size_t i = 0; i < MCME_REGION_ENTRIES; ++i) {
        uint32_t sector_offset = rd_u24be(loc + i * 4);
        if (sector_offset == 0) continue;

        size_t byte_offset = (size_t)sector_offset * MCME_REGION_SECTOR;
        if (byte_offset + 5 > size) {
            mcme_set_error("region: %s slot %zu points to byte %zu, past the %zu-byte file",
                           path, i, byte_offset, size);
            free(data);
            return false;
        }
    }

    r->data = (uint8_t *)data;
    r->size = size;
    r->path = strdup(path);
    if (r->path == NULL) {
        free(data);
        memset(r, 0, sizeof *r);
        mcme_set_error("region: out of memory");
        return false;
    }
    return true;
}

void mcme_region_close(McmeRegion *r)
{
    if (r == NULL) return;
    free(r->data);
    free(r->path);
    memset(r, 0, sizeof *r);
}

/* ========================================================================
 * Header accessors
 * ======================================================================== */

McmeChunkLoc mcme_region_chunk_loc(const McmeRegion *r, int cx, int cz)
{
    McmeChunkLoc loc = { 0, 0 };
    if (r == NULL || r->data == NULL || r->size < MCME_REGION_HEADER) return loc;

    const uint8_t *p = r->data + slot_for(cx, cz) * 4;
    loc.sector_offset = rd_u24be(p);
    loc.sector_count  = p[3];
    return loc;
}

bool mcme_region_chunk_present(const McmeRegion *r, int cx, int cz)
{
    return mcme_region_chunk_loc(r, cx, cz).sector_offset != 0;
}

uint32_t mcme_region_chunk_timestamp(const McmeRegion *r, int cx, int cz)
{
    if (r == NULL || r->data == NULL || r->size < MCME_REGION_HEADER) return 0;
    return rd_u32be(r->data + MCME_REGION_SECTOR + slot_for(cx, cz) * 4);
}

size_t mcme_region_chunk_count(const McmeRegion *r)
{
    size_t n = 0;
    for (int cz = 0; cz < 32; ++cz) {
        for (int cx = 0; cx < 32; ++cx) {
            if (mcme_region_chunk_present(r, cx, cz)) ++n;
        }
    }
    return n;
}

size_t mcme_region_slots(const McmeRegion *r, int *out_slots, size_t cap)
{
    size_t n = 0;
    for (int cz = 0; cz < 32 && n < cap; ++cz) {
        for (int cx = 0; cx < 32 && n < cap; ++cx) {
            if (!mcme_region_chunk_present(r, cx, cz)) continue;
            out_slots[n * 2 + 0] = cx;
            out_slots[n * 2 + 1] = cz;
            ++n;
        }
    }
    return n;
}

/* ========================================================================
 * Chunk payload
 * ======================================================================== */

bool mcme_region_chunk_payload(const McmeRegion *r, int cx, int cz,
                               McmeChunkPayload *out)
{
    if (r == NULL || r->data == NULL) {
        mcme_set_error("region: not open");
        return false;
    }
    if (out == NULL) {
        mcme_set_error("region: output is NULL");
        return false;
    }

    McmeChunkLoc loc = mcme_region_chunk_loc(r, cx, cz);
    if (loc.sector_offset == 0) {
        mcme_set_error("region: no chunk at (%d, %d) in %s", cx, cz,
                       r->path ? r->path : "?");
        return false;
    }

    size_t at = (size_t)loc.sector_offset * MCME_REGION_SECTOR;

    /* mcme_region_open proved at + 5 <= size, same as read_chunk below. */
    if (at + 5 > r->size) {
        mcme_set_error("region: chunk at (%d, %d) is past the end of the file", cx, cz);
        return false;
    }

    uint32_t length = rd_u32be(r->data + at);

    /* The header read stops where read_chunk starts validating compression:
       an inventory reports the file's own byte whether or not mcme can open
       what it names, but the length must still describe a payload that fits
       this file, or it would report a size that does not exist. */
    if (length == 0) {
        mcme_set_error("region: chunk at (%d, %d) declares length 0", cx, cz);
        return false;
    }
    if (length > MCME_REGION_MAX_SECTORS * MCME_REGION_SECTOR) {
        mcme_set_error("region: chunk at (%d, %d) declares %u bytes, over the region limit",
                       cx, cz, length);
        return false;
    }
    if (at + 5 + (size_t)length - 1u > r->size) {
        mcme_set_error("region: chunk at (%d, %d) needs %zu bytes at offset %zu, "
                       "past the %zu-byte file",
                       cx, cz, (size_t)length - 1u, at + 5, r->size);
        return false;
    }

    out->length = length - 1u;
    out->compression = r->data[at + 4];
    return true;
}

bool mcme_region_read_chunk(const McmeRegion *r, int cx, int cz,
                            McmeCompression *out_codec, McmeBuffer *out)
{
    if (r == NULL || r->data == NULL) {
        mcme_set_error("region: not open");
        return false;
    }
    if (out == NULL) {
        mcme_set_error("region: output buffer is NULL");
        return false;
    }

    McmeChunkLoc loc = mcme_region_chunk_loc(r, cx, cz);
    if (loc.sector_offset == 0) {
        mcme_set_error("region: no chunk at (%d, %d) in %s", cx, cz,
                       r->path ? r->path : "?");
        return false;
    }

    size_t at = (size_t)loc.sector_offset * MCME_REGION_SECTOR;

    /* mcme_region_open already proved at + 5 <= size, so this is a redundant
       check kept because read_chunk is the function that would dereference it
       if that ever changed. */
    if (at + 5 > r->size) {
        mcme_set_error("region: chunk at (%d, %d) is past the end of the file", cx, cz);
        return false;
    }

    uint32_t length = rd_u32be(r->data + at);
    uint8_t comp_byte = r->data[at + 4];

    /* `length` counts the compression byte too, so the payload is length - 1.
       A length of zero would make that underflow, and is never legal. */
    if (length == 0) {
        mcme_set_error("region: chunk at (%d, %d) declares length 0", cx, cz);
        return false;
    }
    if (length > MCME_REGION_MAX_SECTORS * MCME_REGION_SECTOR) {
        mcme_set_error("region: chunk at (%d, %d) declares %u bytes, over the region limit",
                       cx, cz, length);
        return false;
    }

    size_t payload_len = (size_t)length - 1u;
    size_t payload_at = at + 5;
    if (payload_at + payload_len > r->size) {
        mcme_set_error("region: chunk at (%d, %d) needs %zu bytes at offset %zu, "
                       "past the %zu-byte file",
                       cx, cz, payload_len, payload_at, r->size);
        return false;
    }

    const uint8_t *payload = r->data + payload_at;

    McmeCompression codec;
    switch (comp_byte) {
    case MCME_ANVIL_COMP_GZIP:   codec = MCME_COMPRESS_GZIP; break;
    case MCME_ANVIL_COMP_ZLIB:   codec = MCME_COMPRESS_ZLIB; break;
    case MCME_ANVIL_COMP_STORED: codec = MCME_COMPRESS_STORE; break;
    case MCME_ANVIL_COMP_EXTERNAL:
        mcme_set_error("region: chunk at (%d, %d) is stored externally (%.*s), "
                       "which mcme does not support",
                       cx, cz, (int)payload_len, (const char *)payload);
        return false;
    default:
        mcme_set_error("region: chunk at (%d, %d) has unknown compression byte %u",
                       cx, cz, comp_byte);
        return false;
    }

    if (out_codec != NULL) *out_codec = codec;
    return mcme_decompress(codec, payload, payload_len, out, MCME_REGION_CHUNK_MAX);
}

/* ========================================================================
 * Path building
 * ======================================================================== */

void mcme_region_of(int cx, int cz, int *out_rx, int *out_rz)
{
    /* floor(chunk / 32): C division truncates toward zero, so the
       negative side needs the adjustment — chunk -1 belongs in region
       -1, where the mask in slot_for will find it at slot 31. */
    if (out_rx != NULL) *out_rx = cx / 32 - (cx % 32 < 0);
    if (out_rz != NULL) *out_rz = cz / 32 - (cz % 32 < 0);
}

const char *mcme_region_path(char *out, size_t cap,
                             const char *world_dir, const char *dimension,
                             int rx, int rz)
{
    if (out == NULL || cap == 0) return NULL;

    /* Java worlds keep the overworld under region/, the nether under
       DIM-1/region/, and the end under DIM1/region/. A NULL dimension means
       the overworld, which is the plain region/ directory. */
    int n;
    if (dimension == NULL || dimension[0] == '\0') {
        n = snprintf(out, cap, "%s/region/r.%d.%d.mca", world_dir, rx, rz);
    } else {
        n = snprintf(out, cap, "%s/%s/region/r.%d.%d.mca",
                     world_dir, dimension, rx, rz);
    }

    if (n < 0 || (size_t)n >= cap) return NULL;
    return out;
}

/* ========================================================================
 * Writing — the write half
 *
 * The reader above proves what the container holds; this replaces one
 * chunk in it. The strategy is append-only: the new payload lands after
 * the file's current end, the slot's entry repoints at it, and every
 * other byte of the file is left exactly as it is. Reallocating sectors
 * in place would mean trusting the one field in the header the reader
 * deliberately ignores — sector counts go stale in real files — and a
 * map editor does not gamble a world's chunks on it.
 * ======================================================================== */

/* The twins of rd_u24be / rd_u32be, for the three integers written back. */
static void wr_u24be(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 16);
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)v;
}

static void wr_u32be(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

static void set_refusal(McmeWriteRefusal *refusal, McmeWriteRefusal why)
{
    if (refusal != NULL) *refusal = why;
}

/* The directory a path lives in: everything before the last slash, or "."
   when there is none. mcme_region_path always writes a slash, so the bare
   name is only for a caller that hands us one. Returns NULL when the path
   does not fit `out`. */
static const char *dir_of(const char *file, char *out, size_t cap)
{
    const char *slash = strrchr(file, '/');
    if (slash == NULL) {
        snprintf(out, cap, ".");
        return out;
    }
    if (slash == file) {
        snprintf(out, cap, "/");
        return out;
    }
    size_t n = (size_t)(slash - file);
    if (n >= cap) return NULL;
    memcpy(out, file, n);
    out[n] = '\0';
    return out;
}

bool mcme_region_set_chunk(const char *file, int cx, int cz,
                           const McmeTag *root, McmeArena *arena,
                           uint32_t timestamp, McmeWriteRefusal *refusal)
{
    set_refusal(refusal, MCME_WRITE_REFUSAL_NONE);

    if (file == NULL || root == NULL || arena == NULL) {
        mcme_set_error("region: null argument");
        set_refusal(refusal, MCME_WRITE_REFUSAL_USAGE);
        return false;
    }
    if (root->type != MCME_TAG_Compound) {
        mcme_set_error("region: a chunk document is rooted in a compound");
        set_refusal(refusal, MCME_WRITE_REFUSAL_USAGE);
        return false;
    }

    /* The region directory must already be there: writing one into
       existence would be inventing a store the world does not have. */
    char dir[MCME_REGION_PATH_MAX];
    if (dir_of(file, dir, sizeof dir) == NULL) {
        mcme_set_error("region: the path to '%s' is too long", file);
        set_refusal(refusal, MCME_WRITE_REFUSAL_INTERNAL);
        return false;
    }
    if (!mcme_fs_exists(dir)) {
        mcme_set_error("region: there is no region directory at '%s'", dir);
        set_refusal(refusal, MCME_WRITE_REFUSAL_NOT_THERE);
        return false;
    }

    /* Encode before the file is touched: a document that will not encode
       has written nothing. Region files are Java's, so the bytes are
       big-endian, and a chunk gets the empty root name — the one a modern
       save carries, and the one SNBT's round trip produces anyway. */
    McmeNbtDoc doc;
    doc.arena = arena;
    doc.root = (McmeTag *)root;   /* the encoder reads; it does not write
                                     through the pointer */
    doc.root_name.data = (char *)"";
    doc.root_name.len = 0;

    McmeBuffer encoded;
    mcme_buffer_init(&encoded);
    if (!mcme_nbt_write_binary_as(&doc, &encoded, false)) {
        mcme_buffer_free(&encoded);
        set_refusal(refusal, MCME_WRITE_REFUSAL_INTERNAL); /* the encoder
                                                              said why */
        return false;
    }

    McmeBuffer z;
    mcme_buffer_init(&z);
    if (!mcme_compress(MCME_COMPRESS_ZLIB, encoded.data, encoded.len, &z)) {
        mcme_buffer_free(&encoded);
        mcme_buffer_free(&z);
        set_refusal(refusal, MCME_WRITE_REFUSAL_INTERNAL); /* out of memory */
        return false;
    }
    mcme_buffer_free(&encoded);

    /* A slot's sector count is one byte: 255 sectors is the largest chunk
       a region file can claim. Larger is refused rather than truncated —
       and it is far under the reader's ceiling too, so what this writes,
       this reads. */
    uint32_t sectors = (uint32_t)((5u + z.len + MCME_REGION_SECTOR - 1u)
                                  / MCME_REGION_SECTOR);
    if (sectors > 255u) {
        mcme_set_error("region: the encoded chunk needs %u sectors; "
                       "a slot can claim 255", sectors);
        mcme_buffer_free(&z);
        set_refusal(refusal, MCME_WRITE_REFUSAL_USAGE);
        return false;
    }

    /* Where the payload goes: the file's current end rounded up to a
       sector — or sector 2 of a file that does not exist yet, whose first
       8 KiB this write provides as the header. A file that exists but is
       not a region mcme can read is not rewritten. */
    bool existed = mcme_fs_exists(file);
    size_t on_disk = 0;
    size_t base = MCME_REGION_HEADER;
    if (existed) {
        McmeRegion reg;
        if (!mcme_region_open(&reg, file)) {
            mcme_buffer_free(&z);
            set_refusal(refusal, MCME_WRITE_REFUSAL_NOT_READABLE);
            return false;          /* open said what the file's problem is */
        }
        on_disk = reg.size;
        base = reg.size;
        mcme_region_close(&reg);
    }

    size_t payload_at =
        (base + MCME_REGION_SECTOR - 1u) / MCME_REGION_SECTOR * MCME_REGION_SECTOR;
    uint32_t start_sector = (uint32_t)(payload_at / MCME_REGION_SECTOR);
    if ((size_t)start_sector + sectors > 0xFFFFFFu) {
        mcme_set_error("region: '%s' would grow past the sector table's reach",
                       file);
        mcme_buffer_free(&z);
        set_refusal(refusal, MCME_WRITE_REFUSAL_INTERNAL);
        return false;
    }

    FILE *fh = fopen(file, existed ? "r+b" : "w+b");
    if (fh == NULL) {
        mcme_set_error("region: cannot open '%s' for writing: %s",
                       file, strerror(errno));
        mcme_buffer_free(&z);
        set_refusal(refusal, MCME_WRITE_REFUSAL_STORE);
        return false;
    }

    bool ok = true;

    /* Pad the file out to the payload's sector — for a file that did not
       exist this writes the 8 KiB header, zeros and all. */
    if (payload_at > on_disk) {
        uint8_t zeros[MCME_REGION_SECTOR] = {0};
        size_t pad = payload_at - on_disk;
        ok = fseek(fh, (long)on_disk, SEEK_SET) == 0;
        while (ok && pad > 0) {
            size_t take = pad < sizeof zeros ? pad : sizeof zeros;
            ok = fwrite(zeros, 1, take, fh) == take;
            pad -= take;
        }
    }

    /* The frame the reader parses: length, compression byte, data. */
    uint8_t head[5];
    wr_u32be(head, (uint32_t)z.len + 1u);
    head[4] = MCME_ANVIL_COMP_ZLIB;
    ok = ok && fseek(fh, (long)payload_at, SEEK_SET) == 0;
    ok = ok && fwrite(head, 1, sizeof head, fh) == sizeof head;
    ok = ok && fwrite(z.data, 1, z.len, fh) == z.len;

    /* The slot's entry and timestamp, written last: a reader racing this
       write finds the old chunk whole until the entry flips to the new. */
    size_t slot = slot_for(cx, cz);
    uint8_t entry[4];
    wr_u24be(entry, start_sector);
    entry[3] = (uint8_t)sectors;
    ok = ok && fseek(fh, (long)(slot * 4), SEEK_SET) == 0;
    ok = ok && fwrite(entry, 1, sizeof entry, fh) == sizeof entry;

    uint8_t stamp[4];
    wr_u32be(stamp, timestamp);
    ok = ok && fseek(fh, (long)(MCME_REGION_SECTOR + slot * 4), SEEK_SET) == 0;
    ok = ok && fwrite(stamp, 1, sizeof stamp, fh) == sizeof stamp;

    if (fclose(fh) != 0) ok = false;
    mcme_buffer_free(&z);

    if (!ok) {
        mcme_set_error("region: cannot write '%s': %s", file, strerror(errno));
        set_refusal(refusal, MCME_WRITE_REFUSAL_STORE);
        return false;
    }
    return true;
}
