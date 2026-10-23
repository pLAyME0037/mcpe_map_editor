/*
 * See statstask.h. Same shape as converttask over gui/task.c: the
 * worker owns its arguments, the result crosses as data, the done
 * callback runs on the loop thread.
 */

#include "statstask.h"

#include <string.h>

#include <glib.h>

#include "mcme/core.h"  /* mcme_last_error — one global, copied now */
#include "mcme/world.h" /* the probe, the inventory's engine          */
#include "task.h"

void mcme_statsresult_free(McmeStatsResult *result)
{
    if (result == NULL) return;

    g_free(result->text);
    g_free(result->name);
    g_free(result->by_dim);
    g_free(result);
}

typedef struct {
    char *world_dir;
} StatsWork;

typedef struct {
    McmeStatsDoneFn fn;
    void *userdata;
} StatsDone;

/* The refusal, in the core's own words when it spoke — copied now,
   the one global's contract (G2), the ask's same fallback. */
static char *refuse_text(void)
{
    char *t = g_strdup(mcme_last_error());

    if (t != NULL && t[0] != '\0') return t;
    g_free(t);
    return g_strdup("the world's store would not answer");
}

static void *stats_work(void *data)
{
    StatsWork *w = data;
    McmeStatsResult *r = g_new0(McmeStatsResult, 1);
    McmeWorldInfo info;

    /* The probe speaks for the directory first — the door's own
       words before anything looks at a store. */
    if (!mcme_world_probe(&info, w->world_dir)) {
        r->text = refuse_text();
        goto out;
    }
    r->kind = info.kind;
    r->name = g_strdup(info.name);

    if (info.kind == MCME_WORLD_UNKNOWN) {
        /* cmd_info's own line for a directory that probes but holds
           no world: the CLI's info is where this door's truth
           lives, so the door speaks the verb's sentence (the G19
           door rule — one truth, no guess). */
        r->text = g_strdup("Not a Minecraft world: no db/ (Bedrock) "
                           "and no region/ or session.lock (Java)");
        mcme_world_info_free(&info);
        goto out;
    }

    if (info.kind == MCME_WORLD_JAVA) {
        /* The probe spoke for identity; the count is the inventory's
           (G22): every dimension tree — region/, DIM-1/region,
           DIM1/region — through the same scanner the CLI's info and
           chunks read, so the door and the shell cannot disagree.
           Java counting never fails the way opening a store can: a
           world without a nether simply holds no nether rows. */
        McmeJavaInventory jinv;

        mcme_java_inventory(&jinv, info.path);
        r->region_files = jinv.region_files;
        r->chunks = jinv.chunks;
        r->dims = jinv.dims;
        if (jinv.dims > 0) {
            r->by_dim = g_new(McmeDimensionCount, jinv.dims);
            memcpy(r->by_dim, jinv.by_dim, jinv.dims * sizeof *r->by_dim);
        }
        mcme_java_inventory_free(&jinv);
        mcme_world_info_free(&info);
        r->ok = true;
        goto out;
    }

    if (!info.has_db || info.db_path == NULL) {
        /* A Bedrock world with no store beside it: nothing to
           walk. The verb would count nothing too, and the door
           says so in its own words rather than printing an empty
           count as if the walk had spoken. */
        r->text = g_strdup("this world has no store mcme can count");
        mcme_world_info_free(&info);
        goto out;
    }

    {
        McmeBedrockInventory inv;

        if (!mcme_bedrock_inventory(&inv, info.db_path)) {
            r->text = refuse_text();
            mcme_world_info_free(&info);
            goto out;
        }
        r->keys = inv.keys;
        r->chunks = inv.chunks;
        r->dims = inv.dims;
        if (inv.dims > 0) {
            r->by_dim = g_new(McmeDimensionCount, inv.dims);
            memcpy(r->by_dim, inv.by_dim, inv.dims * sizeof *r->by_dim);
        }
        mcme_bedrock_inventory_free(&inv);
        mcme_world_info_free(&info);
        r->ok = true;
    }

out:
    g_free(w->world_dir);
    g_free(w);
    return r;
}

static void stats_deliver(void *done_ud, void *result)
{
    StatsDone *d = done_ud;

    d->fn(d->userdata, (McmeStatsResult *)result);
    g_free(d);
}

bool mcme_statstask_start(McmeWire *wire, const char *world_dir,
                          McmeStatsDoneFn done, void *userdata)
{
    StatsWork *w;
    StatsDone *d;

    if (wire == NULL || world_dir == NULL || done == NULL)
        return false;

    w = g_try_new0(StatsWork, 1);
    d = g_try_new0(StatsDone, 1);
    if (w == NULL || d == NULL) {
        g_free(w);
        g_free(d);
        return false;
    }
    w->world_dir = g_strdup(world_dir);
    d->fn = done;
    d->userdata = userdata;

    if (!mcme_task_start(wire, stats_work, w, stats_deliver, d)) {
        g_free(w->world_dir);
        g_free(w);
        g_free(d);
        return false;
    }
    return true;
}
