/*
 * `mcme chunk prune`: whole chunks, selected by a box of columns.
 *
 * The chunk is the unit the game saves and the unit this deletes: the
 * box's X/Z name the chunk columns it reaches (floor division by 16,
 * the rule every chunk verb speaks), its corners may be in any order,
 * and its Y is parsed and ignored — a chunk is all of its Y, so the
 * selection is a box of columns (decisions.md#D13).
 *
 * Whichever world half the probe names answers: Bedrock tombstones
 * every key the chunk owns as batches of one log record each, under
 * the LOCK the game takes; Java zeroes the chunk's slot after every
 * region file the box reaches has opened. Neither half reclaims
 * space — db compact and region compact do that — so pruning never
 * rewrites a payload, never re-encodes NBT, and never touches a key
 * mcme cannot name as a chunk's.
 */

#ifndef MCME_PRUNE_H
#define MCME_PRUNE_H

#include "mcme/bedrock_write.h" /* McmeBox */
#include "mcme/core.h"
#include "mcme/world.h"

/*
 * What a prune deleted. Bedrock fills `chunks` and `keys`; Java fills
 * `chunks` and `region_files`; the field each world does not have
 * stays 0, so the report names what that world lost without inventing
 * a unit the other one counts in.
 */
typedef struct {
    int64_t chunks;        /* chunks deleted (both halves)        */
    int64_t keys;          /* keys tombstoned (Bedrock only)      */
    int64_t region_files;  /* region files punched (Java only)    */
} McmePruneReport;

/*
 * Deletes every chunk the box's columns touch from the world at
 * `world`, whichever half `mcme_world_probe` names, and fills
 * `report` (which may be NULL) with what was deleted. An empty
 * selection — columns the world never stored — is a success that
 * writes nothing, and the same box run twice is 0 chunks the second
 * time.
 *
 * `dimension` is the number the CLI parses: 0 overworld, 1 nether,
 * 2 end. Bedrock compares it to the key's own dimension field, Java
 * to its directory (DIM-1, DIM1), so one flag speaks both worlds.
 *
 * On false, `refusal` (which may be NULL) says which class of
 * refusal and mcme_last_error() says what; nothing has been written
 * except where the refusal says otherwise. The house exit codes map
 * from the refusal: NOT_THERE for a world or dimension that is not
 * there, NOT_READABLE for a region file that will not read (checked
 * before any punch), STORE for the LOCK held or a write that failed,
 * USAGE for a dimension number Java keeps no directory for.
 */
bool mcme_prune(const char *world, const McmeBox *box, int32_t dimension,
                McmePruneReport *report, McmeWriteRefusal *refusal);

#endif /* MCME_PRUNE_H */
