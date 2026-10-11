/*
 * Bedrock subchunk payload decoder. The format and the strictness contract
 * are documented in mcme/bedrock_chunk.h; this file is the enforcement.
 *
 * Every read is bounds-checked against `size` before it happens, and every
 * refusal names what it found — an error message is the only diagnosis
 * someone gets from a world that will not open.
 */

#include "mcme/bedrock_chunk.h"

#include <stdio.h>
#include <string.h>

/* The widths that occur on disk. 7 and 9..15 never appear: the game's own
   enum jumps 6 -> 8 -> 16, and both disk readers reject the gaps. 127 is the
   header some biome records use for "copy the previous palette"; mcbe-leveldb
   flags its meaning for subchunks as unverified, so it is refused here rather
   than guessed at. */
static bool valid_bits(unsigned bits)
{
    switch (bits) {
    case 0: case 1: case 2: case 3: case 4: case 5: case 6: case 8: case 16:
        return true;
    default:
        return false;
    }
}

static bool need(const uint8_t *data, size_t size, size_t pos, size_t n,
                 const char *what)
{
    if (pos + n <= size && pos + n >= pos) return true;
    mcme_set_error("subchunk: truncated payload: %s needs %zu bytes at "
                   "offset %zu of %zu", what, n, pos, size);
    return false;
}

bool mcme_bedrock_subchunk_decode(McmeBedrockSubchunk *out, McmeArena *arena,
                                  const void *data, size_t size)
{
    if (!out || !arena || (!data && size > 0)) {
        mcme_set_error("subchunk: null argument");
        return false;
    }
    memset(out, 0, sizeof *out);

    const uint8_t *p = (const uint8_t *)data;
    size_t pos = 0;

    /* --- Wrapper ------------------------------------------------------- */
    if (!need(p, size, 0, 1, "the version byte")) return false;
    uint8_t version = p[0];
    size_t layer_count;
    bool has_index = false;
    int8_t index = 0;

    switch (version) {
    case 1:
        /* The brief post-palettization format: one layer, no count byte.
           See the gist the wiki cites; amulet's decoder treats it the same. */
        layer_count = 1;
        pos = 1;
        break;
    case 8:
        if (!need(p, size, 1, 1, "the storage-layer count")) return false;
        layer_count = p[1];
        pos = 2;
        break;
    case 9:
        if (!need(p, size, 1, 2, "the storage-layer count and Y index"))
            return false;
        layer_count = p[1];
        index = (int8_t)p[2];
        has_index = true;
        pos = 3;
        break;
    case 0: case 2: case 3: case 4: case 5: case 6: case 7:
        mcme_set_error("subchunk: legacy pre-palette version %u (raw id/data "
                       "planes) is not supported", version);
        return false;
    default:
        mcme_set_error("subchunk: unknown version %u", version);
        return false;
    }

    McmeBedrockLayer *layers = NULL;
    if (layer_count > 0) {
        layers = mcme_arena_alloc(arena, layer_count * sizeof *layers);
        if (!layers) return false;
        memset(layers, 0, layer_count * sizeof *layers);
    }

    /* --- Layers -------------------------------------------------------- */
    for (size_t i = 0; i < layer_count; ++i) {
        if (!need(p, size, pos, 1, "a layer header")) return false;
        uint8_t header = p[pos];

        if (header & 1u) {
            mcme_set_error("subchunk: layer %zu has header 0x%02x: runtime "
                           "palettes are network data, not a stored world",
                           i, header);
            return false;
        }
        unsigned bits = header >> 1;
        if (!valid_bits(bits)) {
            mcme_set_error("subchunk: layer %zu stores %u bits per index; "
                           "valid widths are 0,1,2,3,4,5,6,8,16", i, bits);
            return false;
        }
        pos += 1;

        size_t palette_len;
        if (bits > 0) {
            size_t per_word = 32u / bits;
            size_t words = (MCME_BEDROCK_SUBCHUNK_BLOCKS + per_word - 1)
                           / per_word;
            if (!need(p, size, pos, words * 4, "the index words")) return false;

            uint16_t *indices =
                mcme_arena_alloc(arena, MCME_BEDROCK_SUBCHUNK_BLOCKS
                                        * sizeof *indices);
            if (!indices) return false;

            const uint8_t *w = p + pos;
            uint32_t mask = (1u << bits) - 1u;
            for (size_t u = 0; u < MCME_BEDROCK_SUBCHUNK_BLOCKS; ++u) {
                size_t word = u / per_word;
                uint32_t v = (uint32_t)w[word * 4]
                           | ((uint32_t)w[word * 4 + 1] << 8)
                           | ((uint32_t)w[word * 4 + 2] << 16)
                           | ((uint32_t)w[word * 4 + 3] << 24);
                indices[u] = (uint16_t)((v >> ((u % per_word) * bits)) & mask);
            }
            pos += words * 4;
            layers[i].indices = indices;

            if (!need(p, size, pos, 4, "the palette length")) return false;
            palette_len = (size_t)p[pos]
                        | ((size_t)p[pos + 1] << 8)
                        | ((size_t)p[pos + 2] << 16)
                        | ((size_t)p[pos + 3] << 24);
            pos += 4;

            if (palette_len == 0) {
                mcme_set_error("subchunk: layer %zu has %u-bit indices but an "
                               "empty palette", i, bits);
                return false;
            }
            /* Each entry is at least four bytes of NBT (type, empty name,
               TAG_End), so a length that cannot fit is refused here rather
               than driving an allocation. */
            if (palette_len > (size - pos) / 4) {
                mcme_set_error("subchunk: layer %zu palette claims %zu "
                               "entries but only %zu bytes remain",
                               i, palette_len, size - pos);
                return false;
            }
        } else {
            /* bits == 0: no words, no length field — the single NBT entry IS
               the palette, and every index is 0. Both disk readers agree;
               the golden fixture pins it. Materialise the zeros so callers
               see one shape. */
            uint16_t *indices =
                mcme_arena_alloc(arena, MCME_BEDROCK_SUBCHUNK_BLOCKS
                                        * sizeof *indices);
            if (!indices) return false;
            memset(indices, 0,
                   MCME_BEDROCK_SUBCHUNK_BLOCKS * sizeof *indices);
            layers[i].indices = indices;
            palette_len = 1;
        }

        McmeNbtDoc *palette = mcme_arena_alloc(arena, palette_len
                                                    * sizeof *palette);
        if (!palette) return false;

        for (size_t j = 0; j < palette_len; ++j) {
            size_t consumed = 0;
            if (!mcme_nbt_parse_prefix(&palette[j], arena, p + pos, size - pos,
                                       true /* little endian */, &consumed)) {
                /* Re-wrap with the position in subchunk terms. The NBT error
                   is relative to the slice it was handed. */
                char why[256];
                snprintf(why, sizeof why, "%s", mcme_last_error());
                mcme_set_error("subchunk: layer %zu palette entry %zu at "
                               "offset %zu: %s", i, j, pos, why);
                return false;
            }
            pos += consumed;
        }

        layers[i].bits = (uint8_t)bits;
        layers[i].palette_len = palette_len;
        layers[i].palette = palette;
    }

    /* --- Nothing may follow the last layer --------------------------------
       A decoder that tolerates a tail cannot round-trip: the tail would be
       silently dropped on the way back out. Refusing is how it gets noticed. */
    if (pos != size) {
        mcme_set_error("subchunk: %zu trailing bytes after %zu layer(s)",
                       size - pos, layer_count);
        return false;
    }

    out->version = version;
    out->has_index = has_index;
    out->index = index;
    out->layer_count = layer_count;
    out->layers = layers;
    return true;
}
