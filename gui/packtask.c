/*
 * See packtask.h. The G2 client, now riding the shared spawn-and-
 * deliver layer (gui/task.c): this file keeps the typed seams — the
 * summarize, the result, the trampoline that hands McmePackResult to
 * the caller's callback on the loop thread.
 */

#include "packtask.h"

#include <glib.h>

#include "mcme/resource.h" /* mcme_pack_open — the CLI's dispatch target */
#include "task.h"

/* One index's rows, joined "ns:x" in its own walk order. A list that
 * will not build whole is no list at all. The two index types have
 * no shared face in C — the twin below keeps its own copy. */
static void pack_models(McmePackResult *r, const char *dir)
{
    McmeModelIndex *models = NULL;
    size_t n, built = 0;

    if (!mcme_model_index_open(dir, &models)) return;
    n = mcme_model_count(models);
    if (n == 0) {
        mcme_model_index_close(models);
        return;
    }
    r->models = g_try_malloc((n + 1) * sizeof *r->models);
    if (r->models != NULL) {
        while (built < n) {
            const char *ns = NULL, *path = NULL;

            if (!mcme_model_at(models, built, &ns, &path)) break;
            r->models[built] = g_strdup_printf("%s:%s", ns, path);
            if (r->models[built] == NULL) break;
            built++;
        }
    }
    if (built == n && r->models != NULL) {
        r->models[n] = NULL; /* the picker wants NULL's end */
        r->model_count = n;
    } else {
        for (size_t i = 0; i < built; ++i) g_free(r->models[i]);
        g_free(r->models);
        r->models = NULL;
        r->model_count = 0;
    }
    mcme_model_index_close(models);
}

static void pack_blocks(McmePackResult *r, const char *dir)
{
    McmeBlockstateIndex *blocks = NULL;
    size_t n, built = 0;

    if (!mcme_blockstate_index_open(dir, &blocks)) return;
    n = mcme_blockstate_count(blocks);
    if (n == 0) {
        mcme_blockstate_index_close(blocks);
        return;
    }
    r->blocks = g_try_malloc((n + 1) * sizeof *r->blocks);
    if (r->blocks != NULL) {
        while (built < n) {
            const char *ns = NULL, *name = NULL;

            if (!mcme_blockstate_at(blocks, built, &ns, &name)) break;
            r->blocks[built] = g_strdup_printf("%s:%s", ns, name);
            if (r->blocks[built] == NULL) break;
            built++;
        }
    }
    if (built == n && r->blocks != NULL) {
        r->blocks[n] = NULL;
        r->block_count = n;
    } else {
        for (size_t i = 0; i < built; ++i) g_free(r->blocks[i]);
        g_free(r->blocks);
        r->blocks = NULL;
        r->block_count = 0;
    }
    mcme_blockstate_index_close(blocks);
}

McmePackResult *mcme_pack_summarize(const char *dir)
{
    McmePackResult *r = g_new0(McmePackResult, 1);
    McmePack *pack = NULL;

    if (!mcme_pack_open(dir, &pack)) {
        /* The refusal, copied before anything else can write the
           core's one error slot: only this thread calls core code
           while the loop thread waits on the wire. */
        r->ok = false;
        r->text = g_strdup(mcme_last_error());
        return r;
    }

    /* The same four lines `mcme pack info` prints in its human shape
       (src/app/main.c cmd_pack_info); the --json half stays the CLI's
       own presentation, and the reading is one implementation either
       way — mcme_pack_open. */
    r->ok = true;
    r->text = g_strdup_printf("%s\n  pack_format  %d\n  description  %s"
                              "\n  pack.png     %s\n",
                              pack->root, (int)pack->format,
                              pack->description,
                              pack->has_icon ? "yes" : "no");
    mcme_pack_free(pack);
    pack_models(r, dir);
    pack_blocks(r, dir);
    return r;
}

void mcme_packresult_free(McmePackResult *result)
{
    if (result == NULL) return;

    g_free(result->text);
    if (result->models != NULL) {
        for (size_t i = 0; i < result->model_count; ++i)
            g_free(result->models[i]);
        g_free(result->models);
    }
    g_free(result);
}

typedef struct {
    char *dir;
} PackWork;

typedef struct {
    McmePackDoneFn fn;
    void *userdata;
} PackDone;

static void *pack_work(void *work_ud)
{
    PackWork *w = work_ud;
    McmePackResult *result = mcme_pack_summarize(w->dir);

    g_free(w->dir);
    g_free(w);
    return result;
}

static void pack_deliver(void *done_ud, void *result)
{
    PackDone *d = done_ud;

    d->fn(d->userdata, (McmePackResult *)result);
    g_free(d);
}

bool mcme_packtask_start(McmeWire *wire, const char *dir,
                         McmePackDoneFn done, void *userdata)
{
    PackWork *w;
    PackDone *d;

    if (wire == NULL || dir == NULL || done == NULL) return false;

    w = g_try_new0(PackWork, 1);
    d = g_try_new0(PackDone, 1);
    if (w == NULL || d == NULL) {
        g_free(w);
        g_free(d);
        return false;
    }
    w->dir = g_strdup(dir);
    d->fn = done;
    d->userdata = userdata;

    if (!mcme_task_start(wire, pack_work, w, pack_deliver, d)) {
        g_free(w->dir);
        g_free(w);
        g_free(d);
        return false;
    }
    return true;
}
