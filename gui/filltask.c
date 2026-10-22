/*
 * See filltask.h. Same shape as chunktask over gui/task.c: the
 * worker owns its arguments, the result crosses as data, the done
 * callback runs on the loop thread.
 */

#include "filltask.h"

#include <limits.h>
#include <string.h>

#include <glib.h>

#include "mcme/bedrock_write.h" /* the single-cell write doors */
#include "mcme/core.h"          /* mcme_last_error — one global, copied now */
#include "mcme/resource.h"      /* mcme_block_parse_spec — the verbs' parser */
#include "mcme/world.h"
#include "task.h"

void mcme_fillresult_free(McmeFillResult *result)
{
    if (result == NULL) return;

    g_free(result->text);
    g_free(result);
}

typedef struct {
    char *world_dir;
    char *block;
    int32_t dim;
    long x, y, z;
} FillWork;

typedef struct {
    McmeFillDoneFn fn;
    void *userdata;
} FillDone;

/* The refusal, in the core's own words when it spoke — copied now,
   the one global's contract (G2), the ask's same fallback. */
static char *refuse_text(void)
{
    char *t = g_strdup(mcme_last_error());

    if (t != NULL && t[0] != '\0') return t;
    g_free(t);
    return g_strdup("the world's store would not answer");
}

static void *fill_work(void *data)
{
    FillWork *w = data;
    McmeFillResult *r = g_new0(McmeFillResult, 1);
    McmeWorldInfo info;
    McmeArena *arena = NULL;
    char name[256];
    const McmeTag *states = NULL;
    McmeNbtDoc entry;
    McmeWriteRefusal refusal = MCME_WRITE_REFUSAL_NONE;

    if (w->x < INT32_MIN || w->x > INT32_MAX ||
        w->y < INT32_MIN || w->y > INT32_MAX ||
        w->z < INT32_MIN || w->z > INT32_MAX) {
        r->text = g_strdup("the fill's coordinates are outside the "
                           "range a key can name");
        goto out;
    }

    /* The probe speaks for the directory before the store's LOCK
       ever sees it — a fill must not create a store where no world
       stands. */
    if (!mcme_world_probe(&info, w->world_dir)) {
        r->text = refuse_text();
        goto out;
    }
    if (info.kind == MCME_WORLD_JAVA) {
        r->text = g_strdup("the fill writes Bedrock stores — this "
                           "world is Java");
        mcme_world_info_free(&info);
        goto out;
    }
    if (info.kind != MCME_WORLD_BEDROCK || info.db_path == NULL) {
        r->text = g_strdup("that directory is not a Bedrock world");
        mcme_world_info_free(&info);
        goto out;
    }

    arena = mcme_arena_new();
    if (arena == NULL) {
        r->text = g_strdup("out of memory");
        mcme_world_info_free(&info);
        goto out;
    }
    /* The verbs' own grammar, through the core's one parser — the
       CLI's parse_block_spec wraps this very call. */
    if (!mcme_block_parse_spec(w->block, arena, name, sizeof name,
                               &states)) {
        r->text = refuse_text();
        mcme_world_info_free(&info);
        goto out;
    }
    /* The reference's own palette entry: name, states, version
       17694720 — the document set_block writes. */
    if (!mcme_bedrock_block_entry(arena, name, states, &entry)) {
        r->text = refuse_text();
        mcme_world_info_free(&info);
        goto out;
    }
    /* One cell under the store's exclusive LOCK (the box road's
       fill_check/fill pair opens the same door; set_block takes
       the db/ directory and holds the LOCK for the call). The
       document and the arena outlive the call by contract. */
    if (!mcme_bedrock_set_block(info.db_path, (int32_t)w->x,
                                (int32_t)w->y, (int32_t)w->z,
                                w->dim, &entry, &refusal)) {
        r->text = refuse_text();
        mcme_world_info_free(&info);
        goto out;
    }
    mcme_world_info_free(&info);
    r->ok = true;

out:
    if (arena != NULL) mcme_arena_free(arena);
    g_free(w->world_dir);
    g_free(w->block);
    g_free(w);
    return r;
}

static void fill_deliver(void *done_ud, void *result)
{
    FillDone *d = done_ud;

    d->fn(d->userdata, (McmeFillResult *)result);
    g_free(d);
}

bool mcme_filltask_start(McmeWire *wire, const char *world_dir,
                         int32_t dimension, long x, long y, long z,
                         const char *block,
                         McmeFillDoneFn done, void *userdata)
{
    FillWork *w;
    FillDone *d;

    if (wire == NULL || world_dir == NULL || block == NULL ||
        done == NULL)
        return false;

    w = g_try_new0(FillWork, 1);
    d = g_try_new0(FillDone, 1);
    if (w == NULL || d == NULL) {
        g_free(w);
        g_free(d);
        return false;
    }
    w->world_dir = g_strdup(world_dir);
    w->block = g_strdup(block);
    w->dim = dimension;
    w->x = x;
    w->y = y;
    w->z = z;
    d->fn = done;
    d->userdata = userdata;

    if (!mcme_task_start(wire, fill_work, w, fill_deliver, d)) {
        g_free(w->world_dir);
        g_free(w->block);
        g_free(w);
        g_free(d);
        return false;
    }
    return true;
}
