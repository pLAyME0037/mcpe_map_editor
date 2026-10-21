/*
 * The shell's first client call (D3): the worker reads a pack with the
 * core's own mcme_pack_open — the function `mcme pack info` dispatches
 * to — formats the same four lines the command prints, and posts the
 * result through the wire for the loop thread to hand to a widget.
 * Workers produce; only the visit touches GTK (D6).
 */
#ifndef MCME_GUI_PACKTASK_H
#define MCME_GUI_PACKTASK_H

#include <stdbool.h>

#include "wire.h"

typedef struct {
    bool ok;    /* the pack opened; false carries the refusal in text */
    char *text; /* the summary or the refusal; free the whole result  */
    /* What the pack can mesh — every model its index holds, joined
       "ns:path", the index's own walk order (D53): the picker's rows
       and the mesh argument's shape in one string. NULL when the pack
       refused or the list would not build (never half a list). */
    char **models;
    size_t model_count;
} McmePackResult;

typedef void (*McmePackDoneFn)(void *userdata, McmePackResult *result);

/* Worker thread: reads <dir>/pack.mcmeta through mcme_pack_open and
 * returns the summary — or the refusal mcme_last_error named, copied
 * at once because the core's error slot is one global. */
McmePackResult *mcme_pack_summarize(const char *dir);

/* NULL is a no-op; frees the text and the record. */
void mcme_packresult_free(McmePackResult *result);

/* Any thread: starts a worker that summarizes `dir` and then posts
 * (done, userdata, result) through the wire. false only on OOM — the
 * caller keeps its userdata and no result ever arrives. */
bool mcme_packtask_start(McmeWire *wire, const char *dir,
                         McmePackDoneFn done, void *userdata);

#endif
