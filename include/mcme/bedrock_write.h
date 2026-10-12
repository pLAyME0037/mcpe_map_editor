/*
 * Writing Bedrock chunks — the first edit the store layer
 * carries.
 *
 * The read half of the chunk layer was proven first
 * (decisions.md#D9): payloads decode against the reference,
 * and encode inverts decode byte for byte. This is the first
 * write path built on that, and it is one operation on
 * purpose: set one block. Everything a larger edit needs —
 * the key, the store handle, the decode, the mutation, the
 * encode, the put — is exercised here at single-block
 * scale, where a failure costs one block and not a chunk.
 *
 * Payload values are stored uncompressed (see world.md), so
 * the encoded bytes go into the LevelDB value verbatim.
 */

#ifndef MCME_BEDROCK_WRITE_H
#define MCME_BEDROCK_WRITE_H

#include "mcme/core.h"
#include "mcme/leveldb.h"
#include "mcme/nbt.h"

/*
 * Builds the key of the subchunk a block coordinate falls
 * in — the key mcme_bedrock_set_block writes. The chunk is
 * floor(x/16), floor(z/16) and the subchunk is floor(y/16),
 * which for a negative coordinate is the chunk below it,
 * not the one C's truncating division would name.
 *
 * Returns false, with mcme_last_error() saying why, when the
 * subchunk's Y does not fit the key's signed byte or the
 * buffer arguments are bad.
 */
bool mcme_bedrock_subchunk_key(int32_t x, int32_t y,
                               int32_t z, int32_t dimension,
                               uint8_t *out, size_t cap,
                               size_t *len);

/*
 * Sets one block in a Bedrock world's store.
 *
 * `db_dir` is the world's db/ directory; `x`, `y`, `z` are
 * block coordinates in `dimension`. `block` is the palette
 * entry for the new block — a document whose root is a
 * compound of `name` (string), `states` (compound, possibly
 * empty) and `version` (int), in that order, the shape the
 * reference writes; build it with mcme_bedrock_block_entry().
 *
 * The subchunk the block falls in must already exist: this
 * edit rewrites a stored payload. Creating chunks — the
 * version record, the heightmap, the records a real save
 * also touches — is a larger operation than this, and a
 * missing subchunk is refused, not created.
 *
 * The store is opened with the exclusive LOCK for the
 * duration of the call, the way Minecraft opens one. The
 * block document, and everything it references, must stay
 * valid until the call returns.
 *
 * Returns false with mcme_last_error() saying what was
 * refused.
 */
bool mcme_bedrock_set_block(const char *db_dir,
                            int32_t x, int32_t y, int32_t z,
                            int32_t dimension,
                            const McmeNbtDoc *block);

/*
 * Builds a palette-entry document for `name` with `states`
 * (a compound tag, or NULL for none) and 17694720 — the
 * version the reference assigns a block that arrives
 * without one.
 *
 * The document is rooted in `arena`; `states` itself is
 * borrowed, not copied, and must outlive the document's
 * use. The compound's keys are written in the order the
 * reference writes them: name, states, version.
 */
bool mcme_bedrock_block_entry(McmeArena *arena,
                              const char *name,
                              const McmeTag *states,
                              McmeNbtDoc *out);

/*
 * A box of block positions. Both corners are inside — the box is
 * inclusive, and the corners may be given in any order; fill
 * normalises them per axis.
 */
typedef struct {
    int32_t x1, y1, z1;
    int32_t x2, y2, z2;
} McmeBox;

/*
 * Checks that every subchunk the box touches is stored, naming
 * the first one that is not.
 *
 * Fill writes nothing when a subchunk is missing, so this is the
 * whole question it can fail before it starts. Any open handle
 * answers it; the LOCK is not needed to ask — but taking it for
 * the check and the fill together is what removes the window in
 * which the store changes between them.
 *
 * A box whose Y reaches past the key's signed byte is refused
 * here too: those subchunks cannot be stored, so they cannot be
 * found.
 */
bool mcme_bedrock_fill_check(McmeLevelDb *db, const McmeBox *box,
                             int32_t dimension);

/*
 * Sets every block of the box to `entry` — one payload write per
 * subchunk the box touches, the positions outside the box in
 * those subchunks left as they were. `entry` has the shape
 * mcme_bedrock_block_entry() builds.
 *
 * The store must be open for writing: the caller holds the LOCK
 * for the whole fill. Every subchunk must already exist —
 * mcme_bedrock_fill_check() says, and this refuses before
 * writing anything if one does not. A subchunk that exists but
 * cannot be edited (undecodable payload, the all-air marker)
 * fails the fill with the subchunks before it already written:
 * there is no transaction across puts.
 *
 * The block document, and everything it references, must stay
 * valid until the call returns.
 */
bool mcme_bedrock_fill(McmeLevelDb *db, const McmeBox *box,
                       int32_t dimension,
                       const McmeNbtDoc *entry);

#endif /* MCME_BEDROCK_WRITE_H */
