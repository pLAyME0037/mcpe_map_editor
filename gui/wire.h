/*
 * The GUI's side of D6's wire: the core's event queue (mcme_eq_*) under
 * a GLib idle source. Workers keep calling mcme_wire_post from any
 * thread; the GTK main loop wakes at most once per burst and drains the
 * core queue in that visit. The core stays GLib-free; this file is the
 * only place the two meet (wiki/decisions.md#D3: the GUI calls the same
 * code).
 */
#ifndef MCME_GUI_WIRE_H
#define MCME_GUI_WIRE_H

#include <stdbool.h>

#include "mcme/core.h" /* McmeEventQueue, McmeFn */

typedef struct McmeWire McmeWire;

/* The loop thread. */
McmeWire *mcme_wire_new(void);
void mcme_wire_free(McmeWire *wire);

/* Any thread. Queues (fn, userdata) and wakes the loop unless a wake is
 * already pending; false only on a NULL wire or callback. */
bool mcme_wire_post(McmeWire *wire, McmeFn fn, void *userdata);

#endif
