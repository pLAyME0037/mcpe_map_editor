/*
 * G15's worker: the lane's first writer. The bar's cell (or the
 * --fill flag's chain) becomes one cell written to a Bedrock
 * world's store — the CLI `chunk fill`'s road down to a single
 * cell: the world's own probe, the verbs' shared block-spec parser
 * (mcme_block_parse_spec — the core's own, the CLI's wrapper and
 * this one the same grammar), the palette-entry document the
 * reference itself writes (mcme_bedrock_block_entry — name, states,
 * version 17694720, the version the reference assigns a block that
 * arrives without one), and mcme_bedrock_set_block, which opens the
 * store under its exclusive LOCK the way Minecraft opens one and
 * creates an unstored subchunk the reference's way before editing
 * it.
 *
 * A Java world is refused at the door: the GUI's writer opens on
 * the Bedrock store only — the CLI's java_fill is the reference
 * road, the GUI's turn coming. Every other refusal crosses the
 * wire in mcme_last_error()'s own words, the ask's never-a-naked
 * status rule kept (G7).
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
