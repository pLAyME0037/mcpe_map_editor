/*
 * G15's worker: the lane's first writer. The bar's cell (or the
 * --fill flag's chain) becomes one cell written to the world it
 * stands in — both kinds behind one door (G17): the world's own
 * probe, the verbs' shared block-spec parser
 * (mcme_block_parse_spec — the core's own, the CLI's wrapper and
 * this one the same grammar), then the road the kind names.
 *
 * Bedrock's is the CLI `chunk fill`'s down to a single cell: the
 * palette-entry document the reference itself writes
 * (mcme_bedrock_block_entry — name, states, version 17694720, the
 * version the reference assigns a block that arrives without one),
 * and mcme_bedrock_set_block, which opens the store under its
 * exclusive LOCK the way Minecraft opens one and creates an
 * unstored subchunk the reference's way before editing it.
 *
 * Java's is the CLI's own fill road, moved into the core whole
 * (G17): the cell's box through mcme_java_fill_check_world then
 * mcme_java_fill_world — every region file the chunk lands in
 * checked before any is written, so a chunk the world does not
 * have refuses the whole fill with nothing written anywhere. No
 * LOCK there: a Java world is region files, no single store to
 * hold one on.
 *
 * Every refusal crosses the wire in mcme_last_error()'s own
 * words, the ask's never-a-naked status rule kept (G7).
 *
 * Data crosses; no core call happens off the worker (the G2
 * discipline, kept).
 */
#ifndef MCME_GUI_FILLTASK_H
#define MCME_GUI_FILLTASK_H

#include <stdbool.h>
#include <stdint.h>

#include "wire.h"

typedef struct {
    bool ok;
    char *text; /* the refusal when !ok — the core's own words */
} McmeFillResult;

typedef void (*McmeFillDoneFn)(void *userdata, McmeFillResult *result);

/* Worker: write the cell at (x, y, z) in the dimension with the
 * block spec (ns:name, the verbs' grammar), deliver on the loop
 * thread. The world directory is copied at post (G13's rule); the
 * store's LOCK is held for set_block's call alone. */
bool mcme_filltask_start(McmeWire *wire, const char *world_dir,
                         int32_t dimension, long x, long y, long z,
                         const char *block,
                         McmeFillDoneFn done, void *userdata);

void mcme_fillresult_free(McmeFillResult *result);

#endif
