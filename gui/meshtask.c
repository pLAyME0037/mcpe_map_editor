/*
 * See meshtask.h. Same shape as the pack task, over gui/task.c.
 */

#include "meshtask.h"

#include <string.h>

#include <glib.h>

#include "mcme/core.h"   /* mcme_last_error — one global, copied now */
#include "mcme/resource.h"
#include "task.h"

void mcme_meshresult_free(McmeMeshResult *result)
{
    if (result == NULL) return;

    g_free(result->text);
    if (result->ok) {
        mcme_mesh_free(&result->mesh);
        if (result->has_atlas) mcme_atlas_free(&result->atlas);
        g_free(result->tex_slots);
    }
    g_free(result);
}

/* The seam the core left standing: the mesh's textures[] are the
   index's own file paths ("the rels half was only the dedup key"),
   and the page's names are those very paths fed in — so the slot is
   a straight name match. The word "missing_no" is the page's seeded
   first entry, and it is also the mesh's answer for a texture the
   pack lacks, so both routes land on the missing image. Nothing
   matches, the missing image answers. */
static uint32_t atlas_slot(const McmeAtlas *atlas, const char *name)
{
    if (strcmp(name, "missing_no") == 0) return 0;
    for (size_t j = 0; j < atlas->count; ++j)
        if (strcmp(atlas->names[j], name) == 0)
            return (uint32_t)j;
    return 0;
}

/* The editor's page over the index's whole table (D34/D35: the
   missing image first, then the pack's files in index order). A page
   that will not build leaves the result atlas-less — the tint-only
   viewport dresses the mesh it already has. Shared with the chunk
   worker (G9): the same page dresses every mesh the window is
   handed. */
void mcme_meshtask_pack_atlas(McmeMeshResult *r,
                              const McmeTextureIndex *tex)
{
    size_t n = mcme_texture_count(tex);
    const char **files = g_try_malloc((n ? n : 1) * sizeof *files);

    if (files == NULL) return;
    for (size_t i = 0; i < n; ++i)
        files[i] = mcme_texture_at(tex, i);
    if (mcme_atlas_build((const char *const *)files, n, &r->atlas)) {
        r->has_atlas = true;
        r->tex_slots = g_try_malloc((r->mesh.texture_count
                                     ? r->mesh.texture_count : 1)
                                    * sizeof *r->tex_slots);
        if (r->tex_slots == NULL) {
            mcme_atlas_free(&r->atlas);
            r->has_atlas = false;
        } else {
            for (size_t i = 0; i < r->mesh.texture_count; ++i)
                r->tex_slots[i] = atlas_slot(&r->atlas,
                                             r->mesh.textures[i]);
        }
    }
    g_free(files);
}

typedef struct {
    char *pack_dir;
    char *ns;
    char *path;
    bool  by_block; /* G7: the blockmesh layer, not the model one */
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
    McmeBlockstateIndex *blockstates = NULL;
    bool built;

    if (!mcme_model_index_open(w->pack_dir, &models) ||
        !mcme_texture_index_open(w->pack_dir, &textures)) {
        built = false;
    } else if (w->by_block) {
        /* G7's twin: the blockmesh layer's total answer (D33) — an
           unknown block is the missing mesh, not a refusal, and a
           bare name carries no states. */
        built = mcme_blockstate_index_open(w->pack_dir, &blockstates) &&
                mcme_mesh_for_block(blockstates, models, textures,
                                    w->ns, w->path, NULL, 0,
                                    &r->mesh);
    } else {
        built = mcme_mesh_build(models, textures, w->ns, w->path,
                                &r->mesh);
    }
    if (built) {
        r->ok = true;
        mcme_meshtask_pack_atlas(r, textures);
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
    mcme_blockstate_index_close(blockstates);
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

static bool meshtask_start(McmeWire *wire, const char *pack_dir,
                           const char *ns, const char *path,
                           bool by_block, McmeMeshDoneFn done,
                           void *userdata)
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
    w->by_block = by_block;
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

bool mcme_meshtask_start(McmeWire *wire, const char *pack_dir,
                         const char *ns, const char *path,
                         McmeMeshDoneFn done, void *userdata)
{
    return meshtask_start(wire, pack_dir, ns, path, false, done,
                          userdata);
}

bool mcme_meshtask_start_block(McmeWire *wire, const char *pack_dir,
                               const char *ns, const char *name,
                               McmeMeshDoneFn done, void *userdata)
{
    return meshtask_start(wire, pack_dir, ns, name, true, done,
                          userdata);
}
