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
 * chunk data without identifying a chunk, such as 0x2b (Data3D), 0x2d (the
 * older biome/heightmap spelling) and 0x36 (chunk version), are recognised by
 * name but are not chunk markers: counting them would count some chunks twice
 * and others not at all.
 *
 * The two entity keys — the actor records and the digp index — are outside
 * this shape entirely (a prefix, not coordinates first) and are named below
 * since D23, which is what decisions.md#D13 said the entity work would do.
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
   record as a second would double it.

   0x2b (Data3D) is the record the game writes now: the 512-byte heightmap
   plus one biome array per subchunk height, measured on one real store as
   36,848 overworld records of 568 to 21,040 bytes each — the arrays run
   from the world floor up to each chunk's own top, so sizes track terrain.
   Convert carries its biome half into the Java sections since D19. 0x2d is
   the older 768-byte spelling (heightmap plus a flat 16x16 id table) of
   worlds this store does not hold; its keys parse and, since D26, its
   records are biome records too — carried when the chunk holds no Data3D
   record (the reference's own precedence), counted when it does. */
#define MCME_BEDROCK_TAG_BIOME_3D 0x2bu /* Data3D: heightmap + 3D biomes  */
#define MCME_BEDROCK_TAG_BIOME 0x2du /* the older 768-byte 2D spelling    */

/* A chunk's entity list: the older spelling of a chunk's entities, one
   record per chunk holding one NBT document per entity concatenated with
   no framing — the same shape as the block-entity record. NOT a chunk
   marker either (a list without blocks is still not a chunk), and its
   documents have carried nothing into Java since D23: the reference
   answers nothing for entities, so convert reads the record to count
   what it does not carry. */
#define MCME_BEDROCK_TAG_ENTITIES 0x32u

/* The actor records (D23) — the game's modern spelling of entities: one
   document per record under a key of its own instead of one record per
   chunk, addressed through an index:

     actorprefix (11 bytes)  id (8 LE)                    one entity's record
     digp (4)  x (4)  z (4)  [dimension (4 LE)]           the chunk's index

   The digp record of a chunk names its actors — its value is their 8-byte
   ids concatenated — and the dimension, omitted for the overworld exactly
   like a chunk key's, lives only in that index: an actorprefix key alone
   says nothing about where the record sits. Measured on one real store
   (decisions.md#D23): 740,591 actor records against 38,752 index records,
   of which 6,381 ids are reachable — 196 named by an index the store no
   longer holds a record for — and the other 734,406 actor records belong
   to chunks the game pruned, the set D13 left standing until this work
   named the keys.

   Neither key parses as a chunk key — at those lengths no chunk shape
   matches — so neither marks a chunk, and the index is not a document:
   its documents are the actor records, counted where they stand. The
   index's own keys therefore count nowhere: counting the pointer as well
   as the record would count the same entity twice, and counting only the
   pointer would count one the store does not hold — never twice, never
   zero. */
bool mcme_bedrock_actor_key(const uint8_t *key, size_t len);
bool mcme_bedrock_digp_key(const uint8_t *key, size_t len);

/* The two keys' builders — the inverse of the predicates above, and the
   shape any write to them must spell (D13 refused prune a tag number to
   guess; these are the measured ones). The actor key is 19 bytes; the
   digp key is 12 with the overworld's dimension omitted and 16 with one,
   the chunk key's own rule. On success `*len` is the length written. */
bool mcme_bedrock_actor_key_build(uint8_t *out, size_t cap, size_t *len,
                                  const uint8_t id[8]);
bool mcme_bedrock_digp_key_build(uint8_t *out, size_t cap, size_t *len,
                                 int32_t x, int32_t z, int32_t dimension);

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

/* Short name for a tag byte: "subchunk", "subchunk-old", "version", "biome",
   "biome3d", "block-entity".
   Returns NULL for a tag this build does not know, which callers should print
   as a hex value rather than hide — an unrecognised tag in a store is exactly
   what someone debugging a version mismatch needs to see. */
const char *mcme_bedrock_tag_name(uint8_t tag);

#endif /* MCME_BEDROCK_KEYS_H */
