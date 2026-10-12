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
 * This module is the read half. Writing an entity's record means
 * rewriting every document in the chunk to add, change or remove
 * one — a larger and more dangerous operation than a block edit,
 * and one that should be built on a proven reader.
 */

#ifndef MCME_BEDROCK_ENTITY_H
#define MCME_BEDROCK_ENTITY_H

#include "mcme/core.h"
#include "mcme/leveldb.h"
#include "mcme/nbt.h"

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
 * skipped rather than matched or refused. A record that will not
 * decode fails the whole call: that is world data mcme cannot read,
 * not the absence of an entity.
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

#endif /* MCME_BEDROCK_ENTITY_H */
