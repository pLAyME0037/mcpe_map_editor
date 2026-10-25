/*
 * G23's worker: the cell's census — what blocks fill the bar's
 * cell and how many of each, through the wire the ask carries.
 * The CLI verb is `chunk stats`; its engine (mcme_bedrock_count_box
 * in world.c — the box's positions tallied by spec, holes counted
 * as air, their subchunks named in `missing`) was core's long
 * before this door, so no promotion was owed: the door adds the
 * wire, the result and the sentence's data — the engine's second
 * client, not its mover.
 *
 * Bedrock reads its store; java has no census engine anywhere (the
 * CLI verb refuses it too), so a java world is refused in the
 * VERB's own sentence — one truth, both shells (the G19 door
 * rule).
 *
 * Data crosses — the tally in the engine's order (most frequent
 * first, ties by spec), strings copied out of the arena because
 * the arena dies with the worker — and the loop thread spells the
 * sentence. The readers' family (D66's half): this door breaks
 * nothing, asks nothing back, and its answer stands when it lands.
 */
#ifndef MCME_GUI_CENSUSTASK_H
#define MCME_GUI_CENSUSTASK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "wire.h"

typedef struct {
    char *block;   /* the spec, copied out of the arena (owned) */
    int64_t count; /* positions of the box this spec fills      */
} McmeCensusRow;

typedef struct {
    bool ok;
    char *text;           /* the refusal when !ok                */
    size_t rows;          /* tallied specs                       */
    McmeCensusRow *tally; /* most frequent first (the engine's
                             order, ties by spec), owned         */
    int64_t missing;      /* subchunks the store does not hold;
                             their positions count as air        */
} McmeCensusResult;

typedef void (*McmeCensusDoneFn)(void *userdata,
                                 McmeCensusResult *result);

/* Worker: probe the world, open the store, census the 16x16x16
 * cell holding block (wx, wy, wz) in dimension — deliver on the
 * loop thread. A non-Bedrock world is refused in `chunk stats`'
 * own words. */
bool mcme_censusstask_start(McmeWire *wire, const char *world_dir,
                            int32_t wx, int32_t wy, int32_t wz,
                            int32_t dimension, McmeCensusDoneFn done,
                            void *userdata);

void mcme_censusresult_free(McmeCensusResult *result);

#endif
