/*
 * G18's worker: the lane's second writer, and the destructive one.
 * The bar's cell (or the --prune flag's chain) becomes the chunk
 * column it stands in — every block of that column's Y, the unit
 * the game saves and the unit this deletes — taken out of the world
 * it stands in, both kinds behind one door: the world's own probe,
 * then mcme_prune, whichever half the probe named (prune.h's
 * contract: Bedrock tombstones the column's keys under the store's
 * exclusive LOCK the way Minecraft opens one, Java zeroes the
 * chunk's slot in its region file — no LOCK there, a Java world is
 * region files, no single store to hold one on).
 *
 * The report rides back as data, in the unit that world counts in
 * (prune.h): Bedrock fills chunks and keys, Java fills chunks and
 * region files, and the field the world does not have stays zero —
 * the report names what that world lost without inventing a unit
 * the other one counts in. An empty selection (a chunk the world
 * never stored) is a success that wrote nothing: ok, all fields
 * zero.
 *
 * Every refusal crosses the wire in mcme_last_error()'s own
 * words, the ask's never-a-naked status rule kept (G7).
 *
 * Data crosses; no core call happens off the worker (the G2
 * discipline, kept).
 */
#ifndef MCME_GUI_PRUNETASK_H
#define MCME_GUI_PRUNETASK_H

#include <stdbool.h>
#include <stdint.h>

#include "wire.h"

typedef struct {
    bool ok;
    char *text;         /* the refusal when !ok — the core's own words */
    int64_t chunks;     /* the report, when ok (prune.h's own fields)  */
    int64_t keys;
    int64_t region_files;
} McmePruneResult;

typedef void (*McmePruneDoneFn)(void *userdata, McmePruneResult *result);

/* Worker: delete the chunk column the cell at (x, z) stands in, in
 * the dimension, deliver on the loop thread. The world directory is
 * copied at post (G13's rule); Bedrock's LOCK is held for prune's
 * call alone. Y is never passed: a chunk is all of its Y (D13). */
bool mcme_prunetask_start(McmeWire *wire, const char *world_dir,
                          int32_t dimension, long x, long z,
                          McmePruneDoneFn done, void *userdata);

void mcme_pruneresult_free(McmePruneResult *result);

#endif
