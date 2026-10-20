/*
 * See meshtask.h. Same shape as the pack task, over gui/task.c.
 */

#include "meshtask.h"

#include <glib.h>

#include "mcme/core.h"   /* mcme_last_error — one global, copied now */
#include "mcme/resource.h"
#include "task.h"

void mcme_meshresult_free(McmeMeshResult *result)
{
    if (result == NULL) return;

    g_free(result->text);
    if (result->ok) mcme_mesh_free(&result->mesh);
    g_free(result);
}

typedef struct {
    char *pack_dir;
    char *ns;
    char *path;
} MeshWork;

typedef struct {
    McmeMeshDoneFn fn;
    void *userdata;
} MeshDone;

static void *mesh_work(void *work_ud)
{
    MeshWork *w = work_ud;
    McmeMeshResult *r = g_new0(McmeMeshResult, 1);
    McmeModelIndex *models = NULL;
    McmeTextureIndex *textures = NULL;

    if (mcme_model_index_open(w->pack_dir, &models) &&
        mcme_texture_index_open(w->pack_dir, &textures) &&
        mcme_mesh_build(models, textures, w->ns, w->path, &r->mesh)) {
        r->ok = true;
    } else {
        /* The refusal, copied at once: the core's error slot is one
           global, and only this thread calls core code. */
        r->ok = false;
        r->text = g_strdup(mcme_last_error());
        if (r->text[0] == '\0') {
            g_free(r->text);
            r->text = g_strdup("the pack's indices would not open");
        }
    }
    mcme_texture_index_close(textures);
    mcme_model_index_close(models);
    g_free(w->pack_dir);
    g_free(w->ns);
    g_free(w->path);
    g_free(w);
    return r;
}

static void mesh_deliver(void *done_ud, void *result)
{
    MeshDone *d = done_ud;

    d->fn(d->userdata, (McmeMeshResult *)result);
    g_free(d);
}

bool mcme_meshtask_start(McmeWire *wire, const char *pack_dir,
                         const char *ns, const char *path,
                         McmeMeshDoneFn done, void *userdata)
{
    MeshWork *w;
    MeshDone *d;

    if (wire == NULL || pack_dir == NULL || ns == NULL || path == NULL ||
        done == NULL)
        return false;

    w = g_try_new0(MeshWork, 1);
    d = g_try_new0(MeshDone, 1);
    if (w == NULL || d == NULL) {
        g_free(w);
        g_free(d);
        return false;
    }
    w->pack_dir = g_strdup(pack_dir);
    w->ns = g_strdup(ns);
    w->path = g_strdup(path);
    d->fn = done;
    d->userdata = userdata;

    if (!mcme_task_start(wire, mesh_work, w, mesh_deliver, d)) {
        g_free(w->pack_dir);
        g_free(w->ns);
        g_free(w->path);
        g_free(w);
        g_free(d);
        return false;
    }
    return true;
}
