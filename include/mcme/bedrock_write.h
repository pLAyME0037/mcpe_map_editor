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

#endif /* MCME_BEDROCK_WRITE_H */
