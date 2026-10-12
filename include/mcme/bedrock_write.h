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
#include "mcme/bedrock_chunk.h"

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
 * Why a write refused. The reasons are kept apart so a caller
 * can report which class of failure it was — the world is
 * missing something, the world is holding something mcme cannot
 * read — while mcme_last_error() names the subchunk. Every
 * write verb answers with the same list, so one condition is
 * one exit code across the CLI.
 */
typedef enum {
    MCME_WRITE_REFUSAL_NONE = 0,     /* nothing stood in the way  */
    MCME_WRITE_REFUSAL_NOT_THERE,    /* the game deleted it       */
    MCME_WRITE_REFUSAL_NOT_READABLE, /* undecodable payload, or
                                        the all-air marker, which
                                        has no layer to fill      */
    MCME_WRITE_REFUSAL_STORE,        /* the store could not be
                                        read or written           */
    MCME_WRITE_REFUSAL_USAGE,        /* a null argument, or a
                                        coordinate the key cannot
                                        name                      */
    MCME_WRITE_REFUSAL_INTERNAL      /* out of memory             */
} McmeWriteRefusal;

/*
 * Sets one block in a Bedrock world's store.
 *
 * `db_dir` is the world's db/ directory; `x`, `y`, `z` are
 * block coordinates in `dimension`. `block` is the palette
 * entry for the new block — a document whose root is a
 * compound of `name` (string), `states` (compound, possibly
 * empty) and `version` (int), in that order, the shape the
 * reference writes; build it with mcme_bedrock_block_entry().
 * `refusal`, when it is not NULL, receives why the edit did
 * not happen.
 *
 * A subchunk the block falls in that the world has not stored
 * is created: all air, in the version its neighbours use, and
 * then edited like any other. mcme writes the subchunk payload
 * and nothing else — not the version record, not the heightmap,
 * not the other keys a real save touches — so this edits a
 * stored world rather than generating one.
 *
 * The store is opened with the exclusive LOCK for the
 * duration of the call, the way Minecraft opens one. The
 * block document, and everything it references, must stay
 * valid until the call returns.
 *
 * Returns false with mcme_last_error() saying what was
 * refused, and `refusal` saying which class it was.
 */
bool mcme_bedrock_set_block(const char *db_dir,
                            int32_t x, int32_t y, int32_t z,
                            int32_t dimension,
                            const McmeNbtDoc *block,
                            McmeWriteRefusal *refusal);

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
 * An all-air subchunk, built the way the reference writes a new
 * one: `version` 8 or 9, a single layer whose palette is just
 * minecraft:air, and — only for version 9 — the Y index byte
 * `index`, since that is the version that carries it.
 *
 * Everything roots in `arena`. The caller edits it and encodes
 * it; mcme never writes one it has not filled in.
 */
bool mcme_bedrock_subchunk_air(McmeArena *arena, uint8_t version,
                               int8_t index, McmeBedrockSubchunk *sc);

/*
 * The subchunk at (`cx`, `cz`, `cy`), creating it in memory when
 * the world has not stored it: a stored subchunk decodes exactly
 * as it is, an absent one comes back all-air in the version its
 * neighbours use — 9 if a neighbour carries the Y index, 8
 * otherwise, which is what the reference writes for a new
 * subchunk.
 *
 * A deleted subchunk is an error, not an absence: the game
 * removed it, and mcme does not quietly resurrect it — which
 * is what `refusal` distinguishes, when it is not NULL, from a
 * payload that will not decode or a store that would not read.
 *
 * `sc`, and everything it points at, roots in `arena`. Encoding
 * and writing it are the caller's business.
 */
bool mcme_bedrock_subchunk_get_or_create(McmeLevelDb *db,
                                         int32_t cx, int32_t cz,
                                         int32_t cy,
                                         int32_t dimension,
                                         McmeArena *arena,
                                         McmeBedrockSubchunk *sc,
                                         McmeWriteRefusal *refusal);

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
 * Checks that every subchunk the box touches can be edited,
 * naming the first one that cannot and saying why through
 * `refusal`, which may be NULL when the caller only wants yes or
 * no. Subchunks the world has not stored are not a problem —
 * fill creates those — so this no longer asks whether they
 * exist.
 *
 * Any open handle answers it; the LOCK is not needed to ask —
 * but taking it for the check and the fill together is what
 * removes the window in which the store changes between them.
 *
 * A box whose Y reaches past the key's signed byte is refused
 * here too: those subchunks cannot be stored at all.
 */
bool mcme_bedrock_fill_check(McmeLevelDb *db, const McmeBox *box,
                             int32_t dimension,
                             McmeWriteRefusal *refusal);

/*
 * Sets every block of the box to `entry` — one payload write per
 * subchunk the box touches, the positions outside the box in
 * those subchunks left as they were. `entry` has the shape
 * mcme_bedrock_block_entry() builds.
 *
 * The store must be open for writing: the caller holds the LOCK
 * for the whole fill. A subchunk the box touches that the world
 * has not stored is created — all air, in its neighbours'
 * version, then filled — so a box may reach into ground that was
 * never saved. mcme_bedrock_fill_check() names a subchunk that
 * cannot be edited, and this refuses before writing anything if
 * one turns up. A subchunk that fails part-way leaves the
 * subchunks before it already written: there is no transaction
 * across puts.
 *
 * The block document, and everything it references, must stay
 * valid until the call returns. `refusal`, when it is not NULL,
 * receives why the fill did not happen.
 */
bool mcme_bedrock_fill(McmeLevelDb *db, const McmeBox *box,
                       int32_t dimension,
                       const McmeNbtDoc *entry,
                       McmeWriteRefusal *refusal);

/*
 * Renders a palette entry as the block spec the CLI speaks: the
 * name, with states in braces when the entry has any — the text
 * `chunk get` prints and `chunk set` accepts. The entry is the
 * compound a palette holds (name, states, version).
 *
 * Appends to `out`, which may already hold bytes. Returns false
 * with mcme_last_error() when the entry is not a compound with a
 * name.
 */
bool mcme_bedrock_block_spec(const McmeTag *entry, McmeBuffer *out);

/*
 * One row of a box tally: a block, and how many of the box's
 * positions hold it. `block` is a spec (mcme_bedrock_block_spec)
 * — arena-rooted, or the literal "minecraft:air".
 */
typedef struct {
    const char *block;
    int64_t     count;
} McmeBlockCount;

/*
 * Counts what a box is made of — layer zero, the layer the
 * editing verbs act on — into a tally sorted by count (most
 * first), ties by spec.
 *
 * A subchunk the box touches that is not stored, is deleted, or
 * is stored as the all-air marker counts its positions of the
 * box as `minecraft:air`; the not-stored and deleted ones are
 * reported through `missing` as well. Reading a hole is
 * answerable — it is empty — where writing one is not.
 *
 * The tally and the spec strings root in `arena` and must not
 * outlive it. A payload that does not decode, or an index
 * outside its palette, fails the call: that is world data mcme
 * cannot read, not an absence.
 */
bool mcme_bedrock_count_box(McmeLevelDb *db, const McmeBox *box,
                            int32_t dimension, McmeArena *arena,
                            McmeBlockCount **out, size_t *out_len,
                            int64_t *missing);

#endif /* MCME_BEDROCK_WRITE_H */
