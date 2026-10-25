/*
 * See censusstask.h. Same shape as statstask over gui/task.c: the
 * worker owns its arguments, the result crosses as data, the done
 * callback runs on the loop thread.
 */

#include "censusstask.h"

#include <stdio.h>
#include <string.h>

#include <glib.h>

#include "mcme/bedrock_write.h" /* mcme_bedrock_count_box — the engine */
#include "mcme/core.h"          /* the arena, mcme_last_error           */
#include "mcme/leveldb.h"       /* the store open the verb does         */
#include "mcme/world.h"         /* the probe                             */
#include "task.h"

void mcme_censusresult_free(McmeCensusResult *result)
{
    if (result == NULL) return;

    g_free(result->text);
    if (result->tally != NULL) {
        for (size_t i = 0; i < result->rows; ++i)
            g_free(result->tally[i].block);
        g_free(result->tally);
    }
    g_free(result);
}

typedef struct {
    char *world_dir;
    int32_t wx, wy, wz;
    int32_t dimension;
} CensusWork;

typedef struct {
    McmeCensusDoneFn fn;
    void *userdata;
} CensusDone;

/* The refusal, in the core's own words when it spoke — copied now,
   the one global's contract (G2), the ask's same fallback. */
static char *refuse_text(void)
{
    char *t = g_strdup(mcme_last_error());

    if (t != NULL && t[0] != '\0') return t;
    g_free(t);
    return g_strdup("the cell would not answer");
}

static void *census_work(void *data)
{
    CensusWork *w = data;
    McmeCensusResult *r = g_new0(McmeCensusResult, 1);
    McmeWorldInfo info;
    McmeArena *arena = NULL;
    McmeLevelDb *db = NULL;
    McmeBlockCount *tally = NULL;
    size_t tally_len = 0;
    int64_t missing = 0;
    McmeBox box;

    /* The probe speaks for the directory first — the door's own
       words before anything looks at a store (statstask's road). */
    if (!mcme_world_probe(&info, w->world_dir)) {
        r->text = refuse_text();
        goto out;
    }

    if (info.kind == MCME_WORLD_UNKNOWN) {
        /* cmd_info's own line, borrowed the way the stats door
           borrows it: the CLI's info is where this truth lives. */
        r->text = g_strdup("Not a Minecraft world: no db/ (Bedrock) "
                           "and no region/ or session.lock (Java)");
        mcme_world_info_free(&info);
        goto out;
    }

    if (info.kind != MCME_WORLD_BEDROCK || info.db_path == NULL) {
        /* `chunk stats`' refusal, in its own words: the verb needs
           a Bedrock store and so does this door — java has no
           census engine anywhere (no core road, no verb, no
           fixture), and the door says so with the road's sentence
           rather than inventing one. */
        char buf[512];

        snprintf(buf, sizeof buf, "chunk: '%s' is not a Bedrock world",
                 w->world_dir);
        r->text = g_strdup(buf);
        mcme_world_info_free(&info);
        goto out;
    }

    if (!mcme_leveldb_open(&db, info.db_path)) {
        r->text = refuse_text();
        mcme_world_info_free(&info);
        goto out;
    }

    arena = mcme_arena_new();
    if (arena == NULL) {
        r->text = g_strdup("out of memory");
        goto out;
    }

    /* The cell's own 16^3 — the ask's floor, the fill's box: the
       base is the chunk the bar's block stands in, on every axis
       (the same math gui/main.c's label speaks). */
    box.x1 = (w->wx >> 4) * 16;
    box.y1 = (w->wy >> 4) * 16;
    box.z1 = (w->wz >> 4) * 16;
    box.x2 = box.x1 + 15;
    box.y2 = box.y1 + 15;
    box.z2 = box.z1 + 15;

    if (!mcme_bedrock_count_box(db, &box, w->dimension, arena,
                                &tally, &tally_len, &missing)) {
        r->text = refuse_text();
        goto out;
    }

    /* The tally roots in the arena, which dies with this worker:
       the strings cross into the result's own memory so the door
       can spell them after the engine forgets them. */
    if (tally_len > 0) {
        r->tally = g_new(McmeCensusRow, tally_len);
        for (size_t i = 0; i < tally_len; ++i) {
            r->tally[i].block = g_strdup(tally[i].block);
            r->tally[i].count = tally[i].count;
        }
        r->rows = tally_len;
    }
    r->missing = missing;
    r->ok = true;

out:
    if (arena != NULL) mcme_arena_free(arena);
    if (db != NULL) mcme_leveldb_close(db);
    g_free(w->world_dir);
    g_free(w);
    return r;
}

static void census_deliver(void *done_ud, void *result)
{
    CensusDone *d = done_ud;

    d->fn(d->userdata, (McmeCensusResult *)result);
    g_free(d);
}

bool mcme_censusstask_start(McmeWire *wire, const char *world_dir,
                            int32_t wx, int32_t wy, int32_t wz,
                            int32_t dimension, McmeCensusDoneFn done,
                            void *userdata)
{
    CensusWork *w;
    CensusDone *d;

    if (wire == NULL || world_dir == NULL || done == NULL)
        return false;

    w = g_try_new0(CensusWork, 1);
    d = g_try_new0(CensusDone, 1);
    if (w == NULL || d == NULL) {
        g_free(w);
        g_free(d);
        return false;
    }
    w->world_dir = g_strdup(world_dir);
    w->wx = wx;
    w->wy = wy;
    w->wz = wz;
    w->dimension = dimension;
    d->fn = done;
    d->userdata = userdata;

    if (!mcme_task_start(wire, census_work, w, census_deliver, d)) {
        g_free(w->world_dir);
        g_free(w);
        g_free(d);
        return false;
    }
    return true;
}
