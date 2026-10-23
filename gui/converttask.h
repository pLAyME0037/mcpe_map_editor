/*
 * G19's worker: the lane's third writer, and the one that creates.
 * The open world read whole, every carried block through the
 * translate map, a Java world written BESIDE it: the output
 * directory is derived — the input trimmed of its trailing slashes
 * with "_java" appended, a sibling never a child (the CLI's own
 * "outside its input" rule, baked into the derivation so no path
 * can break it) — and mcme_convert_bedrock_java refuses to write
 * over anything: `<out>` must not exist, and a failed run removes
 * it whole, so an output world is finished or absent (convert.h's
 * contract, D12).
 *
 * Only Bedrock input converts today: a Java input is refused at
 * the door in cmd_convert's own sentence ("... is a java world;
 * only Bedrock input converts today"), the kind name the core
 * spells (mcme_world_kind_name) — the verdict lives in the verb,
 * so the door speaks the verb's truth rather than guessing. The
 * input is never touched: this writer creates, it does not
 * destroy, which is why its button asks back nothing (D66).
 *
 * The whole McmeConvertReport rides back as data — every carried
 * headline and every dropped count — and the label spells the
 * drops through mcme_convert_drops_text, the sentence both
 * clients print. A refusal crosses the wire in mcme_last_error()'s
 * own words, the ask's never-a-naked status rule kept (G7).
 *
 * Data crosses; no core call happens off the worker (the G2
 * discipline, kept).
 */
#ifndef MCME_GUI_CONVERTTASK_H
#define MCME_GUI_CONVERTTASK_H

#include <stdbool.h>
#include <stdint.h>

#include "mcme/convert.h" /* McmeConvertReport — the answer, whole  */
#include "wire.h"

typedef struct {
    bool ok;
    char *text;               /* the refusal when !ok              */
    char *out_dir;            /* where it wrote, when ok (owned)   */
    McmeConvertReport report; /* whole: every count the run made   */
} McmeConvertResult;

typedef void (*McmeConvertDoneFn)(void *userdata,
                                  McmeConvertResult *result);

/* Worker: convert the Bedrock world at world_dir into the Java
 * world beside it (the derived "<dir trimmed>_java"), all three
 * dimensions, deliver on the loop thread. The world directory is
 * copied at post (G13's rule). */
bool mcme_converttask_start(McmeWire *wire, const char *world_dir,
                            McmeConvertDoneFn done, void *userdata);

void mcme_convertresult_free(McmeConvertResult *result);

#endif
