/*
 * See chunktask.h. Same shape as the mesh task over gui/task.c: the
 * worker owns its arguments, the result crosses as data, the done
 * callback runs on the loop thread.
 */

#include "chunktask.h"

#include <stdio.h>
#include <string.h>

#include <glib.h>

#include "mcme/core.h"    /* mcme_last_error — one global, copied now */
#include "mcme/resource.h"
#include "mcme/world.h"
#include "task.h"

void mcme_chunkresult_free(McmeChunkResult *result)
{
    if (result == NULL) return;

    g_free(result->text);
    g_free(result->spec);
    mcme_meshresult_free(result->mesh); /* NULL is a no-op */
    g_free(result);
}

typedef struct {
    char *world_dir;
    char *pack_dir;
    int32_t dim;
    long x, y, z;
} ChunkWork;

typedef struct {
    McmeChunkDoneFn fn;
    void *userdata;
} ChunkDone;

/* The refusal, in the store's own words when it spoke (G7's rule:
   never a naked status — the store said which part would not
   read). */
static char *refuse_text(void)
{
    char *t = g_strdup(mcme_last_error());

    if (t != NULL && t[0] != '\0') return t;
    g_free(t);
    return g_strdup("the world's store would not answer");
}

/* The states compound as the blockmesh layer asks them: name/value
   pairs, string values only — the walk cmd_blockstate has always
   done, its cap kept. */
#define CHUNK_MAX_STATES 64

static bool state_pairs(const McmeTag *states, McmeState *props,
                        size_t *count_out)
{
    *count_out = 0;
    if (states == NULL) return true;
    for (size_t i = 0; i < states->compound.len; ++i) {
        const McmeCompoundEntry *e = &states->compound.entries[i];

        if (*count_out >= CHUNK_MAX_STATES) return false;
        props[*count_out].name = e->name.data;
        props[*count_out].value =
            e->value->type == MCME_TAG_String ? e->value->string.data
                                              : NULL;
        (*count_out)++;
    }
    return true;
}

static void *chunk_work(void *work_ud)
{
    ChunkWork *w = work_ud;
    McmeChunkResult *r = g_new0(McmeChunkResult, 1);
    McmeWorldInfo info;
    McmeBuffer spec;
    McmeArena *arena = NULL;
    char name[256], ns[256];
    const McmeTag *states = NULL;
    const char *base;
    McmeState props[CHUNK_MAX_STATES];
    size_t prop_count = 0;
    McmeBlockstateIndex *blockstates = NULL;
    McmeModelIndex *models = NULL;
    McmeTextureIndex *textures = NULL;

    mcme_buffer_init(&spec);
    if (!mcme_world_probe(&info, w->world_dir)) {
        r->ok = false;
        r->text = refuse_text();
        goto out;
    }

    /* The world's own answer, in its own words — the spec the
       verbs' road always read (G9's move). */
    if (mcme_world_block_spec(&info, w->dim, w->x, w->y, w->z, &spec)
            != MCME_WORLDBLOCK_OK) {
        r->ok = false;
        r->text = refuse_text();
        mcme_world_info_free(&info);
        goto out;
    }
    mcme_world_info_free(&info);
    r->spec = g_strndup((const char *)spec.data, spec.len);
    if (r->spec == NULL) {
        r->ok = false;
        r->text = g_strdup("out of memory");
        goto out;
    }

    /* The spec, parsed the way the verbs parse it — and split the
       name the way they split it (namespace defaults to the
       game's). */
    arena = mcme_arena_new();
    if (arena == NULL) {
        r->ok = false;
        r->text = g_strdup("out of memory");
        goto out;
    }
    if (!mcme_block_parse_spec(r->spec, arena, name, sizeof name,
                               &states)) {
        r->ok = false;
        r->text = refuse_text();
        goto out;
    }
    if (!state_pairs(states, props, &prop_count)) {
        r->ok = false;
        r->text = g_strdup("the block's states are too many");
        goto out;
    }
    {
        const char *colon = strchr(name, ':');

        if (colon != NULL) {
            size_t ns_len = (size_t)(colon - name);

            if (ns_len == 0 || ns_len >= sizeof ns) {
                r->ok = false;
                r->text = g_strdup("the block's namespace is empty "
                                   "or too long");
                goto out;
            }
            memcpy(ns, name, ns_len);
            ns[ns_len] = '\0';
            base = colon + 1;
        } else {
            snprintf(ns, sizeof ns, "minecraft");
            base = name;
        }
    }

    /* The blockmesh layer's total answer (D33): the pack's own
       selection for this very state — never a refusal the pack
       could cause. */
    if (!mcme_blockstate_index_open(w->pack_dir, &blockstates) ||
        !mcme_model_index_open(w->pack_dir, &models) ||
        !mcme_texture_index_open(w->pack_dir, &textures)) {
        r->ok = false;
        r->text = refuse_text();
        goto out;
    }
    r->mesh = g_new0(McmeMeshResult, 1);
    if (!mcme_mesh_for_block(blockstates, models, textures, ns, base,
                             props, prop_count, &r->mesh->mesh)) {
        r->ok = false;
        r->text = refuse_text();
        g_clear_pointer(&r->mesh, mcme_meshresult_free);
        goto out;
    }
    /* The mesh half's own ok — set_mesh gates its stage on it (the
       meshtask's r->ok, one layer down here). */
    r->mesh->ok = true;
    r->ok = true;
    mcme_meshtask_pack_atlas(r->mesh, textures);

out:
    mcme_blockstate_index_close(blockstates);
    mcme_model_index_close(models);
    mcme_texture_index_close(textures);
    if (arena != NULL) mcme_arena_free(arena);
    mcme_buffer_free(&spec);
    g_free(w->world_dir);
    g_free(w->pack_dir);
    g_free(w);
    return r;
}

static void chunk_deliver(void *done_ud, void *result)
{
    ChunkDone *d = done_ud;

    d->fn(d->userdata, (McmeChunkResult *)result);
    g_free(d);
}

bool mcme_chunktask_start(McmeWire *wire, const char *world_dir,
                          const char *pack_dir, int32_t dimension,
                          long x, long y, long z,
                          McmeChunkDoneFn done, void *userdata)
{
    ChunkWork *w;
    ChunkDone *d;

    if (wire == NULL || world_dir == NULL || pack_dir == NULL ||
        done == NULL)
        return false;

    w = g_try_new0(ChunkWork, 1);
    d = g_try_new0(ChunkDone, 1);
    if (w == NULL || d == NULL) {
        g_free(w);
        g_free(d);
        return false;
    }
    w->world_dir = g_strdup(world_dir);
    w->pack_dir = g_strdup(pack_dir);
    w->dim = dimension;
    w->x = x;
    w->y = y;
    w->z = z;
    d->fn = done;
    d->userdata = userdata;

    if (!mcme_task_start(wire, chunk_work, w, chunk_deliver, d)) {
        g_free(w->world_dir);
        g_free(w->pack_dir);
        g_free(w);
        g_free(d);
        return false;
    }
    return true;
}
