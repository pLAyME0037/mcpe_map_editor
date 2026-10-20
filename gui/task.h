/*
 * Spawn-and-deliver — G2's pattern, now shared by every task.
 *
 * The work runs on a fresh worker thread; its result rides the wire to
 * the loop thread, where the done callback runs — the only thread that
 * may touch a widget (D6). The wire must outlive every task started on
 * it: main.c waits its tasks out through the loop before freeing.
 *
 * The work function owns `work_userdata` (it frees it if heap) and
 * returns the result, which passes to `done` — which must consume or
 * free it. A NULL anywhere and the task does not start: false is
 * returned, no thread spawns, no done callback ever runs.
 */
#ifndef MCME_GUI_TASK_H
#define MCME_GUI_TASK_H

#include <stdbool.h>

#include "wire.h"

typedef void *(*McmeTaskWorkFn)(void *work_userdata);          /* worker */
typedef void (*McmeTaskDoneFn)(void *done_userdata, void *result); /* loop */

bool mcme_task_start(McmeWire *wire, McmeTaskWorkFn work, void *work_ud,
                     McmeTaskDoneFn done, void *done_ud);

#endif
