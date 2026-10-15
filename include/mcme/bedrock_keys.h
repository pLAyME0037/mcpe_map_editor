/*
 * Bedrock chunk keys.
 *
 * A Bedrock store is one flat keyspace of NBT payloads, binary blobs and
 * bookkeeping records, all addressed by a key whose shape says what it is.
 * Nothing else in the key tells you, and there is no directory structure to
 * fall back on — so every count, listing and edit starts by deciding which
 * keys are chunk keys and which are not.
 *
 * The shape is:
 *
 *   x (4 LE)   z (4 LE)   [dimension (4 LE)]   tag (1 byte)   [index]
 *
 * with the dimension **omitted entirely when it is 0**, the overworld. That
 * omission is the single most awkward thing about the format: a reader cannot
 * tell from the length alone whether a 9-byte key is an overworld chunk or a
 * 13-byte key with four bytes missing, and must look at the tag to decide.
 *
 * The tag set here is deliberately not guessed at. It matches what Amulet
 * treats as marking a chunk (tags 0x2c and 0x76) and adds 0x2f, which is the
 * same subchunk prefix under a later version of the game — leaving it out
 * would report a modern world as having no chunks at all. Tags that appear on
 * chunk data without identifying a chunk, such as 0x2d (biome/heightmap) and
 * 0x36 (chunk version), are recognised by name but are not chunk markers:
 * counting them would count some chunks twice and others not at all.
 */

#ifndef MCME_BEDROCK_KEYS_H
#define MCME_BEDROCK_KEYS_H

#include "mcme/core.h"

/* Chunk-marker tags. Both spellings of the subchunk prefix are here because
   the game changed it, and a reader that only knows one of them silently
   reports the other era's worlds as empty. */
#define MCME_BEDROCK_TAG_SUBCHUNK_OLD 0x2cu  /* pre-1.18 subchunk prefix  */
#define MCME_BEDROCK_TAG_SUBCHUNK     0x2fu  /* 1.18+ subchunk prefix     */
#define MCME_BEDROCK_TAG_CHUNK_VERSION 0x76u /* chunk version record      */

/* A chunk's block entities: one record per chunk holding one NBT
   document per entity, concatenated with no framing. It carries no
   subchunk index, and it is NOT a chunk marker — a record of
   entities says a chunk exists only because something else already
   stored its blocks, and counting it would report chunks the game
   has half-written. */
#define MCME_BEDROCK_TAG_BLOCK_ENTITY 0x31u

/* The tick records: one per chunk, a `currentTick` plus a `tickList`
   of {blockState, time, x/y/z} entries. 0x33 holds the world's own
   schedules — measured: water and lava flows, overdue and pending;
   0x3a a second schedule in the same shape, measured holding fire
   entries with their own future times. Both carry into the chunk's
   Java lists, split by content (D18). Neither marks a chunk on its
   own: a record without subchunks is still not a chunk. */
#define MCME_BEDROCK_TAG_PENDING_TICKS 0x33u
#define MCME_BEDROCK_TAG_RANDOM_TICKS  0x3au

/* Tags that belong to a chunk without identifying one. Reported by name so a
   listing can say what a key holds, but never counted as a chunk: a chunk with
   a biome record and no subchunk is still one chunk, and counting the biome
   record as a second would double it. */
#define MCME_BEDROCK_TAG_BIOME 0x2du /* biome / heightmap, 768 bytes */

typedef struct {
    bool    is_chunk;        /* tag marks a chunk; x/z/dimension are valid */
    bool    has_dimension;   /* false means the key omitted a zero one */
    int32_t dimension;
    int32_t x, z;
    uint8_t tag;
} McmeBedrockKey;

/*
 * Decodes one store key.
 *
 * Returns false when the key is not chunk-shaped at all — too short, too long,
 * or a length that cannot belong to any chunk key. Such keys are a normal part
 * of a store (level metadata, portallists, actor records), so this is a
 * "not a chunk", not an error, and mcme_last_error() is left alone.
 */
bool mcme_bedrock_key_parse(const uint8_t *key, size_t len, McmeBedrockKey *out);

/*
 * Builds one chunk key — the inverse of mcme_bedrock_key_parse.
 *
 * The dimension is written only when nonzero, because that is what
 * the game does: a zero dimension is omitted, never stored as four
 * zero bytes. `has_index` appends the signed subchunk Y byte, which
 * only the two subchunk prefixes carry; asking for one on any other
 * tag is refused, because no other record has a byte to spend.
 *
 * On success `*len` is the length written (9, 10, 13 or 14) and the
 * bytes are in `out`. Returns false, with mcme_last_error() saying
 * why, when the arguments are NULL, the tag cannot carry an index,
 * or `cap` is too small to hold the key.
 */
bool mcme_bedrock_key_build(uint8_t *out, size_t cap, size_t *len,
                            int32_t x, int32_t z, int32_t dimension,
                            uint8_t tag, bool has_index, int8_t index);

/*
 * The chunk a block coordinate falls in, and the block's offset
 * inside it (0 to 15). Every coordinate rule in the chunk layer is
 * this one — floor division by 16 — because a negative block is in
 * the chunk BELOW the one C's truncating division names. It lives
 * here so the rule is written once.
 */
int32_t mcme_bedrock_chunk_of(int32_t block);
int32_t mcme_bedrock_offset_in(int32_t block);

/* Short name for a tag byte: "subchunk", "subchunk-old", "version", "biome".
   Returns NULL for a tag this build does not know, which callers should print
   as a hex value rather than hide — an unrecognised tag in a store is exactly
   what someone debugging a version mismatch needs to see. */
const char *mcme_bedrock_tag_name(uint8_t tag);

#endif /* MCME_BEDROCK_KEYS_H */
