/*
 * See prunetask.h. Same shape as filltask over gui/task.c: the
 * worker owns its arguments, the result crosses as data, the done
 * callback runs on the loop thread.
 */

#include "prunetask.h"

#include <limits.h>
#include <string.h>

#include <glib.h>

#include "mcme/bedrock_write.h" /* McmeBox, McmeWriteRefusal */
#include "mcme/core.h"          /* mcme_last_error — one global, copied now */
#include "mcme/prune.h"         /* mcme_prune — both halves, one call */
#include "mcme/world.h"         /* the probe's own words first */
#include "task.h"

void mcme_pruneresult_free(McmePruneResult *result)
{
    if (result == NULL) return;

    g_free(result->text);
    g_free(result);
}

typedef struct {
    char *world_dir;
    int32_t dim;
    long x, z;
} PruneWork;

typedef struct {
    McmePruneDoneFn fn;
    void *userdata;
} PruneDone;

/* The refusal, in the core's own words when it spoke — copied now,
   the one global's contract (G2), the ask's same fallback. */
static char *refuse_text(void)
{
    char *t = g_strdup(mcme_last_error());

    if (t != NULL && t[0] != '\0') return t;
    g_free(t);
    return g_strdup("the world's store would not answer");
}

static void *prune_work(void *data)
{
    PruneWork *w = data;
    McmePruneResult *r = g_new0(McmePruneResult, 1);
    McmeWorldInfo info;
    McmeBox box;
    McmePruneReport report;
    McmeWriteRefusal refusal = MCME_WRITE_REFUSAL_NONE;

    if (w->x < INT32_MIN || w->x > INT32_MAX ||
        w->z < INT32_MIN || w->z > INT32_MAX) {
        r->text = g_strdup("the prune's coordinates are outside the "
                           "range a column can name");
        goto out;
    }

    /* The probe speaks for the directory before the store's LOCK
       ever sees it — a prune must not open a store where no world
       stands. */
    if (!mcme_world_probe(&info, w->world_dir)) {
        r->text = refuse_text();
        goto out;
    }
    mcme_world_info_free(&info);

    /* The chunk column the cell stands in: the box of columns a
       single cell names, floor division by 16 the chunk verbs'
       own rule (D13) — mcme_prune takes it from the box's corners.
       The Y is parsed and ignored; a column is all of its Y, so
       the box's own Y is 0 to 0. */
    box.x1 = box.x2 = (int32_t)w->x;
    box.y1 = box.y2 = 0;
    box.z1 = box.z2 = (int32_t)w->z;
    memset(&report, 0, sizeof report);
    if (!mcme_prune(w->world_dir, &box, w->dim, &report, &refusal)) {
        r->text = refuse_text();
        goto out;
    }
    r->ok = true;
    r->chunks = report.chunks;
    r->keys = report.keys;
    r->region_files = report.region_files;

out:
    g_free(w->world_dir);
    g_free(w);
    return r;
}

static void prune_deliver(void *done_ud, void *result)
{
    PruneDone *d = done_ud;

    d->fn(d->userdata, (McmePruneResult *)result);
    g_free(d);
}

bool mcme_prunetask_start(McmeWire *wire, const char *world_dir,
                          int32_t dimension, long x, long z,
                          McmePruneDoneFn done, void *userdata)
{
    PruneWork *w;
    PruneDone *d;

    if (wire == NULL || world_dir == NULL || done == NULL)
        return false;

    w = g_try_new0(PruneWork, 1);
    d = g_try_new0(PruneDone, 1);
    if (w == NULL || d == NULL) {
        g_free(w);
        g_free(d);
        return false;
    }
    w->world_dir = g_strdup(world_dir);
    w->dim = dimension;
    w->x = x;
    w->z = z;
    d->fn = done;
    d->userdata = userdata;

    if (!mcme_task_start(wire, prune_work, w, prune_deliver, d)) {
        g_free(w->world_dir);
        g_free(w);
        g_free(d);
        return false;
    }
    return true;
}
