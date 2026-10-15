/*
 * Bedrock subchunk payloads — the bytes under a 0x2f / 0x2c chunk key.
 *
 * A chunk is stored as one key per 16x16x16 subchunk rather than one record
 * per chunk (see bedrock_keys.h for the key layout). The value is a small
 * binary wrapper around one or more *paletted storage* layers, each holding
 * 4096 block indices plus the palette those indices point into. This module
 * decodes that payload into a model and encodes a model back into payload
 * bytes; the two are inverses, byte for byte, which is what the golden
 * fixtures assert against the reference's own payloads.
 *
 * The format, as settled by minecraft.wiki's Bedrock level-format page, by
 * Tommaso Checchi's paletted-storage write-up, and by the two independent
 * disk readers this tree was validated against (amulet-core 1.9.50 and
 * mcbe-leveldb 1.23.0 — all three agree, including on the odd cases):
 *
 *   byte 0        subchunk version: 1, 8 or 9
 *                 0-7 are the legacy pre-palette format (raw id/data planes)
 *                 and are REFUSED here, loudly, rather than misparsed
 *   byte 1        storage-layer count       (version 1 omits this: it is 1)
 *   byte 2        signed subchunk Y index   (version 9 only, 1.18.2+)
 *
 * then, per storage layer:
 *
 *   byte          header = (bits_per_index << 1) | runtime_flag
 *                 bits_per_index is one of 0,1,2,3,4,5,6,8,16; other widths
 *                 never occur on disk and are refused
 *                 runtime_flag is 0 on disk (persistent NBT palettes); 1
 *                 means network runtime IDs, which are session-relative and
 *                 meaningless in a world file — refused
 *   bits > 0:     ceil(4096 / floor(32/bits)) 32-bit little-endian words.
 *                 Each word holds floor(32/bits) indices packed from its
 *                 least-significant bit upward; the bits above the last index
 *                 are padding (present, zero in practice, not validated).
 *                 Index i addresses block (x, z, y) = (i>>8, (i>>4)&15, i&15)
 *                 — XZY order with Y fastest, unlike Java's YZX.
 *                 Then a 32-bit little-endian palette length, then that many
 *                 little-endian NBT compounds, concatenated, no framing.
 *                 The encoder writes the padding bits zero, as every writer
 *                 seen here does; see the note under mcme_bedrock_subchunk_encode.
 *   bits == 0:    no words AND no length field — a single NBT compound
 *                 follows the header directly and fills the layer. This is
 *                 the detail two sources get wrong by analogy (the length
 *                 field is NOT here) and both disk readers agree it is not;
 *                 it is pinned by tests/golden/subchunks/v8-bpb0.
 *
 * A layer count of 0 (version 8/9) is a legal "this subchunk is all air"
 * marker and decodes to zero layers.
 *
 * The payload reaching this module is the store value verbatim:
 * chunk payload values are stored uncompressed (the fork's
 * compression lives inside the database's table blocks, not in
 * the values — see world.md), so there is no framing to strip
 * here.
 *
 * Strictness is deliberate. Everything is bounds-checked, and anything this
 * decoder does not fully understand — legacy versions, unknown widths,
 * runtime palettes, an empty palette, trailing bytes after the last layer —
 * is an error naming what was found, not a best-effort guess. An editor that
 * guesses here writes the guess back to someone's world. The cost is that a
 * real world containing one of these refuses to decode that subchunk with a
 * message; that is the trade, and it is the same one the region reader makes.
 *
 * Ownership: everything returned lives in the arena the caller passes.
 * Freeing the arena frees the whole model. The input bytes are borrowed for
 * the duration of the call only.
 */

#ifndef MCME_BEDROCK_CHUNK_H
#define MCME_BEDROCK_CHUNK_H

#include "mcme/core.h"
#include "mcme/nbt.h"

#define MCME_BEDROCK_SUBCHUNK_BLOCKS 4096u

typedef struct {
    uint8_t    bits;        /* stored width; 0 means one entry fills the layer */
    uint16_t  *indices;     /* 4096 palette indices in XZY order; never NULL  */
    size_t     palette_len; /* >= 1                                           */
    McmeNbtDoc *palette;    /* palette_len little-endian NBT documents        */
} McmeBedrockLayer;

typedef struct {
    uint8_t  version;       /* 1, 8 or 9 as stored */
    bool     has_index;     /* version 9 carries the subchunk's own Y index */
    int8_t   index;         /* valid when has_index; not range-checked here */
    size_t   layer_count;   /* 0 means the all-air marker */
    McmeBedrockLayer *layers;
} McmeBedrockSubchunk;

/*
 * Decodes one subchunk payload.
 *
 * `data`/`size` are the raw store value. Returns false on
 * anything malformed or unsupported; mcme_last_error() says what was found
 * anything malformed or unsupported; mcme_last_error() says what was found
 * and where. On failure `*out` is zeroed and owns nothing.
 *
 * The indices of a bits==0 layer are materialised as 4096 zeros rather than
 * left NULL, so callers have exactly one shape to handle; `bits` still says
 * what the file stored, which is what an encoder needs to reproduce it.
 */
bool mcme_bedrock_subchunk_decode(McmeBedrockSubchunk *out, McmeArena *arena,
                                  const void *data, size_t size);

/*
 * Encodes a model back into payload bytes, appended to `out`.
 *
 * The contract is inversion: for every payload decode() accepts,
 * encode(decode(payload)) reproduces it byte for byte, padding excepted
 * (below). That holds because the encoder writes what the model *records*
 * rather than re-deriving it: `bits` is the stored width, never recomputed
 * from the palette — a two-entry palette stored at width 16 round-trips as
 * width 16 — and palette documents are forced little-endian regardless of
 * the endianness they carry. The golden fixtures assert the property by
 * comparing this function's output with the reference payload itself, not
 * with a re-parse of it.
 *
 * Refused before a byte is appended, with the finding in mcme_last_error():
 * versions outside {1, 8, 9}; a shape the wrapper cannot carry (version 1
 * with anything but exactly one layer and no index, version 8 with an index,
 * version 9 without one, more than 255 layers); widths outside the enum; a
 * zero-length palette or one whose documents are missing their root; a
 * bits==0 layer that is not a single all-zero entry; and any index the
 * layer's width cannot hold. Whether an index points *inside* its palette is
 * the write path's business, not this one's: the decoder does not enforce it
 * either, and this function exists to invert that decoder.
 *
 * The one documented non-inversion: padding bits. The model carries block
 * indices, not the unused high bits of each word, so they come back zero —
 * which is what the reference encoder and every payload this tree has seen
 * store there anyway. A payload carrying garbage in padding decodes and
 * re-encodes with zeros; nothing else behaves that way.
 *
 * On refusal nothing has been appended to `out`. A buffer allocation failure
 * mid-write returns false with a partial buffer; the caller frees it.
 */
bool mcme_bedrock_subchunk_encode(const McmeBedrockSubchunk *sc,
                                  McmeBuffer *out);

/*
 * Sets one block in a decoded subchunk, in place.
 *
 * The palette entry for `block` is found or appended to layer zero —
 * the block layer; deeper layers are fluids and are not touched —
 * the index at (x, y, z) is pointed at it, and the layer's stored
 * width is grown to fit the largest index now in use — and only
 * grown: a layer stored wider than its palette needs stays that
 * way, so a no-op edit leaves every byte of the payload alone.
 * Coordinates are within the subchunk, each 0 to 15.
 *
 * The width follows the reference's rule — the bit length of the
 * largest index in use, floored at 1, with 7 and 9..15 snapped to
 * 8 and 16 — except that a layer still holding exactly one entry
 * at index 0 keeps width 0, the compact form the reference never
 * writes but real worlds contain. Setting a block to the value it
 * already holds therefore changes nothing, and encoding reproduces
 * the payload byte for byte.
 *
 * Palette entries the indices no longer reference are kept: the
 * reference prunes them on its own saves, but dropping them here
 * would rewrite indices this edit did not touch.
 *
 * `block` and everything it references is borrowed for the call;
 * encode the subchunk before the block's arena is freed.
 */
bool mcme_bedrock_subchunk_set(McmeBedrockSubchunk *sc,
                              McmeArena *arena,
                              int32_t x, int32_t y, int32_t z,
                              const McmeNbtDoc *block);

#define MCME_BEDROCK_BIOME_CELLS      64u /* one 4x4x4 cell block      */
#define MCME_BEDROCK_HEIGHTMAP_COLS  256u /* the record's column heights */
#define MCME_BEDROCK_BIOME_MAX_ARRAYS 24u /* one world height of them  */

/*
 * The biome half of a Data3D record (tag 0x2b): the heightmap, and one
 * array of biome ids per subchunk Y counted up from the world floor.
 *
 * The payload is `heightmap` (256 little-endian int16 column heights,
 * carried but not written anywhere yet) followed by the arrays. Each
 * array is either a single id (width 0: one id fills the whole 4x4x4)
 * or the same packed-word storage a subchunk layer uses — 4096 ids at
 * up to 16 bits — whose every 4th cell along each axis is the cell
 * value, plus a palette of plain little-endian int32 ids. `cells` holds
 * those 64 values already in Java's biome-container order, x + z*4 +
 * y*16, so a writer packs them straight.
 *
 * The record's trailing 0xFF padding (the game pads; the reference
 * walks it off one byte at a time because its header is read signed,
 * and this decoder does the same by the high bit) is skipped. Refused
 * with the finding in mcme_last_error(): a record shorter than the
 * heightmap; an array count past one world height; a packed width
 * outside 1..16; a zero-length palette; an index past its palette;
 * truncation anywhere. One refusal is the whole record's — the caller
 * counts it once (decisions.md#D19) rather than trusting a half.
 *
 * Ownership: everything returned lives in the arena; the input bytes
 * are borrowed for the call only.
 */
struct McmeBedrockBiomeArray {
    int8_t   cy;
    uint32_t cells[MCME_BEDROCK_BIOME_CELLS];
};

typedef struct {
    int16_t heightmap[256];
    struct McmeBedrockBiomeArray *arrays;
    size_t count;            /* arrays used; array i sits at floor_cy + i */
} McmeBedrockBiomes;

bool mcme_bedrock_biome_decode(McmeBedrockBiomes *out, McmeArena *arena,
                               const void *data, size_t size, int8_t floor_cy);

#endif /* MCME_BEDROCK_CHUNK_H */
