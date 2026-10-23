/*
 * G21's worker: the read door. What the world label answers when
 * the hand asks "what is this world?" — the walk the CLI's `info`
 * and `chunks` speak through, promoted to core when its second
 * client arrived (mcme_bedrock_inventory — the whole-store key walk
 * moved from src/app/main.c into world.c, the G11 rule: a second
 * client makes the engine core's, one count both shells read).
 * Java needs no move: the probe already counts its region/ files
 * and their chunks on McmeWorldInfo, so both doors print the same
 * numbers from the same struct.
 *
 * The result carries DATA — kind, name, the counts, the
 * per-dimension rows — and the door (the loop thread) spells the
 * sentence, the fill/prune/convert words' same arrangement: data
 * crosses, words belong to the face they are shown on. A refusal
 * crosses in mcme_last_error()'s own words (G7's never-a-naked
 * status), except two verdicts that are sentences already: the
 * probe's "Not a Minecraft world: ..." — cmd_info's own line,
 * because the CLI's info is where this door's truth lives — and
 * the door's own sentence for a Bedrock world with no store beside
 * it, where the walk has nothing to count.
 *
 * This door only reads: it touches no world and asks nothing back
 * (the readers' half of D66's family — the button that creates
 * asks nothing, and the button that only looks asks even less).
 *
 * Data crosses; no core call happens off the worker (the G2
 * discipline, kept).
 */
#ifndef MCME_GUI_STATSTASK_H
#define MCME_GUI_STATSTASK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "mcme/world.h" /* McmeWorldKind, McmeDimensionCount — the answer */
#include "wire.h"

typedef struct {
    bool ok;
    char *text;               /* the refusal when !ok              */
    McmeWorldKind kind;       /* what the probe named the directory */
    char *name;               /* the world's own name, or NULL (owned) */
    size_t keys;              /* bedrock: every live key (java: 0) */
    size_t chunks;            /* bedrock: its store's distinct
                                 chunks; java: the probe's region/
                                 count — info's own number         */
    size_t region_files;      /* java's region files (bedrock: 0)  */
    size_t dims;              /* rows in by_dim (java: 0)          */
    McmeDimensionCount *by_dim; /* owned copy of the walk's rows   */
} McmeStatsResult;

typedef void (*McmeStatsDoneFn)(void *userdata, McmeStatsResult *result);

/* Worker: probe the directory and count what it holds — the
 * bedrock store walked key by key, the java probe's own totals —
 * deliver on the loop thread. */
bool mcme_statstask_start(McmeWire *wire, const char *world_dir,
                          McmeStatsDoneFn done, void *userdata);

void mcme_statsresult_free(McmeStatsResult *result);

#endif
