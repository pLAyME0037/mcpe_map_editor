/*
 * mcme Java Anvil region reader. See include/mcme/region.h for the layout.
 *
 * The whole file is loaded once and every access is bounds-checked against
 * `size`. Region files are not trusted input: a hand-edited or corrupted .mca
 * is a normal thing for a map editor to be pointed at, and a hostile one is a
 * thing to survive.
 */

#include "mcme/region.h"

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
