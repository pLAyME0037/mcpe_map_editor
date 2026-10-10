#include "mcme/core.h"

#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#include "stb_ds.h"

/*
 * Event queue.
 *
 * This is the C replacement for the original app's wx.CallAfter / wx.PostEvent.
 * It is the load-bearing piece of the threading contract (decisions.md#D6):
 *
 *   - worker threads push results in with mcme_eq_post()
 *   - the main loop pulls them out with mcme_eq_drain()
 *   - workers never touch OpenGL
 *
 * Implementation is deliberately plain: a growable array under one mutex.
 * Drain snapshots the array, swaps it empty, then runs the snapshot *unlocked*
 * so a handler may safely post new events without deadlocking.
 */

struct McmeEventQueue {
    pthread_mutex_t mu;
    struct {
        McmeFn fn;
        void  *userdata;
    } *items;          /* stb_ds dynamic array */
};

McmeEventQueue *mcme_eq_new(void)
{
    McmeEventQueue *q = calloc(1, sizeof(McmeEventQueue));
    if (!q) return NULL;
    if (pthread_mutex_init(&q->mu, NULL) != 0) {
        free(q);
        return NULL;
    }
    return q;
}

bool mcme_eq_post(McmeEventQueue *q, McmeFn fn, void *userdata)
{
    if (!q || !fn) return false;

    pthread_mutex_lock(&q->mu);
    arrput(q->items, ((typeof(q->items[0])){ .fn = fn, .userdata = userdata }));
    pthread_mutex_unlock(&q->mu);
    return true;
}

size_t mcme_eq_drain(McmeEventQueue *q)
{
    if (!q) return 0;

    /* Snapshot under the lock, run outside it. */
    pthread_mutex_lock(&q->mu);
    size_t n = arrlen(q->items);
    if (n == 0) {
        pthread_mutex_unlock(&q->mu);
        return 0;
    }

    /* Steal the buffer. A fresh NULL array is left behind. */
    __typeof__(q->items) batch = q->items;
    q->items = NULL;
    pthread_mutex_unlock(&q->mu);

    for (size_t i = 0; i < n; ++i) {
        batch[i].fn(batch[i].userdata);
    }
    arrfree(batch);
    return n;
}

size_t mcme_eq_pending(const McmeEventQueue *q)
{
    if (!q) return 0;
    /* Cast away const: stb_ds arrlen only reads arrlen header field. */
    pthread_mutex_lock(&((McmeEventQueue *)q)->mu);
    size_t n = arrlen(((McmeEventQueue *)q)->items);
    pthread_mutex_unlock(&((McmeEventQueue *)q)->mu);
    return n;
}

void mcme_eq_free(McmeEventQueue *q)
{
    if (!q) return;
    arrfree(q->items);
    pthread_mutex_destroy(&q->mu);
    free(q);
}
