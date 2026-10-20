/*
 * See wire.h. The scheduled flag is the coalescing: a post racing the
 * idle source's visit leaves its item for the source the winner (or the
 * visit's own clear) already arranged — the core queue's snapshot keeps
 * the drain from growing inside itself, and the clear-before-drain
 * keeps a handler's post from being stranded without a wake-up.
 */

#include "wire.h"

#include <glib.h>

struct McmeWire {
    McmeEventQueue *q;
    int scheduled; /* an idle source is pending; set and tested atomically */
};

static gboolean on_idle(void *data)
{
    McmeWire *wire = data;

    g_atomic_int_set(&wire->scheduled, 0);
    (void)mcme_eq_drain(wire->q);
    return G_SOURCE_REMOVE;
}

McmeWire *mcme_wire_new(void)
{
    McmeWire *wire = g_new(McmeWire, 1);

    wire->q = mcme_eq_new();
    if (wire->q == NULL) {
        g_slice_free(McmeWire, wire);
        return NULL;
    }
    wire->scheduled = 0;
    return wire;
}

void mcme_wire_free(McmeWire *wire)
{
    if (wire == NULL) return;

    /* Removes a source still waiting on the loop; none is harmless. */
    g_idle_remove_by_data(wire);
    mcme_eq_free(wire->q);
    g_slice_free(McmeWire, wire);
}

bool mcme_wire_post(McmeWire *wire, McmeFn fn, void *userdata)
{
    if (wire == NULL || !mcme_eq_post(wire->q, fn, userdata))
        return false;

    /* The loser of the race leaves its item for the source the winner
     * already scheduled — g_idle_add is safe from any thread. */
    if (g_atomic_int_compare_and_exchange(&wire->scheduled, 0, 1))
        g_idle_add(on_idle, wire);
    return true;
}
