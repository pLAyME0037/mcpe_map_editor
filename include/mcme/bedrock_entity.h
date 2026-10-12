/*
 * Bedrock block entities — the data a chunk carries beside its
 * blocks.
 *
 * A chest's inventory, a sign's text, a spawner's spawn rules:
 * none of it is in a subchunk. Each chunk holds one record (key
 * tag 0x31) whose value is one NBT document per entity,
 * concatenated with no framing — the next document simply starts
 * where the last finished, which is the same shape as a block
 * palette and is read the same way, mcme_nbt_parse_prefix().
 *
 * Each document is a compound. The game gives every block entity
 * `id` (a namespaced string), and `x`, `y`, `z` as ints naming the
 * block it belongs to; the rest is the entity's own — `Items` for a
 * container, `Text` for a sign, and so on. mcme reads the document
 * verbatim and adds nothing to it: an editor that rewrites an
 * entity's fields it does not understand is the same hazard as one
 * that guesses at a palette.
 *
 * The read half came first — decisions.md#D9 again, proven before
 * written to — and the write half is built on it. Both agree on one
 * rule: a record any part of which mcme cannot read is one mcme
 * will not answer for or rewrite, because a read that stopped early
 * would disagree with a write that splices.
 */

#ifndef MCME_BEDROCK_ENTITY_H
#define MCME_BEDROCK_ENTITY_H

#include "mcme/core.h"
#include "mcme/leveldb.h"
#include "mcme/nbt.h"
#include "mcme/bedrock_write.h"

/*
 * How a block-entity read ended. The classes match the CLI's exit
 * codes one for one, the way the write refusals do, so one
 * condition is one answer across the verbs.
 */
typedef enum {
    MCME_ENTITY_OK = 0,          /* the document is in `out`      */
    MCME_ENTITY_ABSENT,          /* no entity at that block       */
    MCME_ENTITY_NOT_READABLE,    /* the record will not decode    */
    MCME_ENTITY_STORE,           /* the store could not be read   */
    MCME_ENTITY_USAGE,           /* a null argument               */
    MCME_ENTITY_INTERNAL         /* out of memory                 */
} McmeEntityStatus;

/*
 * The block entity stored at block (`x`, `y`, `z`) in `dimension`.
 *
 * `out` receives that document — the compound the world holds, with
 * `id` and the coordinates inside it, nothing added — and roots in
 * `arena`, which must outlive it.
 *
 * A chunk's record holds several documents, so this is a search for
 * the one whose `x`/`y`/`z` match, and the first match answers.
 * Documents that carry no coordinates are not addressable by
 * position — the reference's own decoder drops them — so they are
 * skipped rather than matched or refused. A record any part of which
 * will not decode fails the whole call: half a record is not an
 * answer, and it is not something the write half rewrites either.
 *
 * Returns MCME_ENTITY_ABSENT when the chunk has no record at all,
 * and when the game has deleted one: removed, so nothing is stored
 * there either.
 */
McmeEntityStatus mcme_bedrock_block_entity(McmeLevelDb *db,
                                           int32_t x, int32_t y,
                                           int32_t z,
                                           int32_t dimension,
                                           McmeArena *arena,
                                           McmeNbtDoc *out);

/*
 * Stores `entity` as the block entity of block (`x`, `y`, `z`) —
 * the edit half of this module.
 *
 * `db_dir` is the world's db/ directory, opened with the exclusive
 * LOCK for the duration of the call, the way mcme_bedrock_set_block()
 * writes. `entity` is borrowed and MODIFIED in place: its `x`, `y`
 * and `z` are set from the position, because a document naming
 * another block would be stored where nothing finds it. It and
 * `arena` must outlive the call; `arena` also receives what the
 * record's other documents decode into.
 *
 * The record is spliced, not re-encoded: the documents beside this
 * one are written back as the exact bytes they arrived as, so an
 * edit to one chest cannot reorder the sign beside it. A chunk whose
 * record the world never stored gets one — writes create, the way
 * `chunk set` creates a subchunk — and a record the game deleted is
 * treated as one it does not have. The whole record must decode
 * first: half a record is not something mcme rewrites.
 *
 * Refuses with MCME_WRITE_REFUSAL_USAGE when `entity` is not a
 * compound or its `id` is not a namespaced string — the form the
 * game writes and the reference requires, so what mcme stores is
 * what both can read back.
 */
bool mcme_bedrock_put_block_entity(const char *db_dir,
                                   int32_t x, int32_t y,
                                   int32_t z,
                                   int32_t dimension,
                                   McmeTag *entity,
                                   McmeArena *arena,
                                   McmeWriteRefusal *refusal);

/*
 * Removes the block entity of block (`x`, `y`, `z`), leaving the
 * documents beside it byte for byte. When it was the last one the
 * record's key is deleted: mcme does not keep a record holding
 * nothing, which is also what the game stores — no key at all.
 *
 * Returns MCME_WRITE_REFUSAL_NOT_THERE when nothing is stored at
 * that block, including a record the game already deleted.
 */
bool mcme_bedrock_del_block_entity(const char *db_dir,
                                   int32_t x, int32_t y,
                                   int32_t z,
                                   int32_t dimension,
                                   McmeWriteRefusal *refusal);

#endif /* MCME_BEDROCK_ENTITY_H */
