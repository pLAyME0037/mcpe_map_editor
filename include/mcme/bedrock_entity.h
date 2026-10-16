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
 * `id` (its savegame id — bare in the game's own records, "Chest",
 * and namespaced in mcme's earlier writes, "minecraft:chest";
 * both read back), and `x`, `y`, `z` as ints naming the
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
 *
 * Since D23 this module also reads the chunk's ENTITY list (tag
 * 0x32) — the older spelling of a chunk's entities, the same walk
 * with a different key — because convert reads it to count what it
 * does not carry: the reference answers nothing for entities
 * (decisions.md#D23), so there is nothing to write and only the
 * count to be exact. The modern spelling (the actor records under
 * their own keys) needs no document walk to count — one key, one
 * record — and is named in bedrock_keys.h.
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
 * Every document of the block-entity record of chunk (`cx`, `cz`)
 * in `dimension` — the whole-record read conversion needs, where
 * the documents' own coordinates are the data rather than a search
 * key.
 *
 * On MCME_ENTITY_OK, `*docs` receives `*count` documents in the
 * order the record stores them, allocated in `arena` (the documents
 * themselves root there too); both are zeroed first, so a caller
 * can reuse one pair of variables across records. MCME_ENTITY_ABSENT
 * means the store has no record — and nothing is read for a record
 * the game deleted.
 *
 * The same rule as the per-position read, kept whole: every
 * document must decode or the call answers MCME_ENTITY_NOT_READABLE
 * for the record. This function adds no coordinate filter — a
 * document with no x/y/z is returned and the caller decides what
 * that means, the way mcme_bedrock_block_entity returns only what
 * it can address but never rewrites half a record.
 */
McmeEntityStatus mcme_bedrock_chunk_block_entities(McmeLevelDb *db,
                                                   int32_t cx,
                                                   int32_t cz,
                                                   int32_t dimension,
                                                   McmeArena *arena,
                                                   McmeNbtDoc **docs,
                                                   size_t *count);

/*
 * Every document of one chunk's entity-list record (tag 0x32) —
 * the same read as the block entities above with the list's own
 * key, added at D23 so convert can count what it does not carry.
 *
 * The whole-record rule stands: every document decodes or the call
 * answers MCME_ENTITY_NOT_READABLE for the record, a record that
 * decodes but holds nothing answers MCME_ENTITY_OK with `*count`
 * 0, and a store with no record for the chunk answers
 * MCME_ENTITY_ABSENT. `docs` roots in `arena` and holds `*count`
 * documents on MCME_ENTITY_OK.
 *
 * The key is rebuilt from the coordinates, the block-entity
 * record's own rule: measured on the store the census read, every
 * list key has one spelling (9 bytes spoken, 13 — the dimension
 * only when it is not 0), so there is no second form for a
 * rebuild to miss (decisions.md#D23).
 */
McmeEntityStatus mcme_bedrock_chunk_entities(McmeLevelDb *db,
                                             int32_t cx,
                                             int32_t cz,
                                             int32_t dimension,
                                             McmeArena *arena,
                                             McmeNbtDoc **docs,
                                             size_t *count);

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
 * compound or its `id` is neither spelling an id takes: the bare
 * savegame id the game itself writes ("Chest") or the namespaced
 * one ("minecraft:chest"), each side of the colon a name of
 * letters, digits, `_`, `-` or `.`. The id is stored exactly as
 * given — mcme checks the shape both spellings share, not the
 * game's registry, which it does not have.
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
