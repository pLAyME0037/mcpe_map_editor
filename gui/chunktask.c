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

/* The name split the verbs own: the bytes before the colon are the
   namespace (the game's when the spec names none), the rest the
   base — into the caller's buffers, no allocation. */
static bool split_ns(const char *name, char *ns, size_t ns_cap,
                     const char **base_out)
{
    const char *colon = strchr(name, ':');

    if (colon != NULL) {
        size_t ns_len = (size_t)(colon - name);

        if (ns_len == 0 || ns_len >= ns_cap) return false;
        memcpy(ns, name, ns_len);
        ns[ns_len] = '\0';
        *base_out = colon + 1;
        return true;
    }
    snprintf(ns, ns_cap, "minecraft");
    *base_out = name;
    return true;
}

/* The mesh's faces as the settle line counts them: every bucket's
   own, present or not none (G10 — the cull's before and after). */
static size_t mesh_faces(const McmeMesh *mesh)
{
    size_t n = 0;

    for (int i = 0; i < 7; ++i)
        if (mesh->present[i]) n += mesh->buckets[i].face_count;
    return n;
}

static void *chunk_work(void *work_ud)
{
    ChunkWork *w = work_ud;
    McmeChunkResult *r = g_new0(McmeChunkResult, 1);
    McmeWorldInfo info;
    McmeArena *arena = NULL;
    char *text[7] = { NULL };
    char name[256], ns[256];
    const McmeTag *states = NULL;
    const char *base;
    McmeState props[CHUNK_MAX_STATES];
    size_t prop_count = 0;
    McmeBlockstateIndex *blockstates = NULL;
    McmeModelIndex *models = NULL;
    McmeTextureIndex *textures = NULL;

    if (!mcme_world_probe(&info, w->world_dir)) {
        r->text = refuse_text();
        goto out;
    }

    /* The block's own cell, then the six the cull asks (G10): the
       verbs' seven reads over one probe, mcme_world_block_spec one
       road — the target's refusal stands, a side outside the
       stored world reads air (the world's edge, partial, firing
       neither clause). */
    for (int s = 0; s < 7; ++s) {
        int dx = 0, dy = 0, dz = 0;
        McmeBuffer buf;
        McmeWorldBlockStatus st;

        if (s != 0) mcme_cull_offset(s - 1, &dx, &dy, &dz);
        mcme_buffer_init(&buf);
        st = mcme_world_block_spec(&info, w->dim, w->x + dx,
                                   w->y + dy, w->z + dz, &buf);
        if (st == MCME_WORLDBLOCK_NOT_FOUND && s != 0) {
            mcme_clear_error();
            if (!mcme_buffer_append_cstr(&buf, "minecraft:air")) {
                mcme_buffer_free(&buf);
                r->text = g_strdup("out of memory");
                mcme_world_info_free(&info);
                goto out;
            }
            st = MCME_WORLDBLOCK_OK;
        }
        if (st == MCME_WORLDBLOCK_OK)
            text[s] = g_strndup((const char *)buf.data, buf.len);
        mcme_buffer_free(&buf);
        if (st != MCME_WORLDBLOCK_OK) {
            r->text = refuse_text();
            mcme_world_info_free(&info);
            goto out;
        }
        if (text[s] == NULL) {
            r->text = g_strdup("out of memory");
            mcme_world_info_free(&info);
            goto out;
        }
    }
    mcme_world_info_free(&info);
    r->spec = g_strdup(text[0]);
    if (r->spec == NULL) {
        r->text = g_strdup("out of memory");
        goto out;
    }

    /* The spec, parsed the way the verbs parse it — and split the
       name the way they split it (namespace defaults to the
       game's). */
    arena = mcme_arena_new();
    if (arena == NULL) {
        r->text = g_strdup("out of memory");
        goto out;
    }
    if (!mcme_block_parse_spec(text[0], arena, name, sizeof name,
                               &states)) {
        r->text = refuse_text();
        goto out;
    }
    if (!state_pairs(states, props, &prop_count)) {
        r->text = g_strdup("the block's states are too many");
        goto out;
    }
    if (!split_ns(name, ns, sizeof ns, &base)) {
        r->text = g_strdup("the block's namespace is empty or too "
                           "long");
        goto out;
    }

    /* The blockmesh layer's total answer (D33): the pack's own
       selection for this very state — never a refusal the pack
       could cause. */
    if (!mcme_blockstate_index_open(w->pack_dir, &blockstates) ||
        !mcme_model_index_open(w->pack_dir, &models) ||
        !mcme_texture_index_open(w->pack_dir, &textures)) {
        r->text = refuse_text();
        goto out;
    }
    r->mesh = g_new0(McmeMeshResult, 1);
    if (!mcme_mesh_for_block(blockstates, models, textures, ns, base,
                             props, prop_count, &r->mesh->mesh)) {
        r->text = refuse_text();
        g_clear_pointer(&r->mesh, mcme_meshresult_free);
        goto out;
    }

    /* The six neighbours' transparencies through the same answers
       (G10): each side's own spec meshed for the one flag the
       clauses read — `chunk mesh`'s seven preludes over this one
       open set of indices. */
    {
        McmeTransparency beside[MCME_CULL_NONE];

        for (int i = 0; i < MCME_CULL_NONE; ++i)
            beside[i] = MCME_TRANSPARENCY_PARTIAL;
        for (int s = 1; s < 7; ++s) {
            McmeMesh nm;

            memset(&nm, 0, sizeof nm);
            if (!mcme_block_parse_spec(text[s], arena, name,
                                       sizeof name, &states)) {
                r->text = refuse_text();
                goto out;
            }
            if (!state_pairs(states, props, &prop_count)) {
                r->text = g_strdup("the block's states are too many");
                goto out;
            }
            if (!split_ns(name, ns, sizeof ns, &base)) {
                r->text = g_strdup("the block's namespace is empty "
                                   "or too long");
                goto out;
            }
            if (!mcme_mesh_for_block(blockstates, models, textures,
                                     ns, base, props, prop_count,
                                     &nm)) {
                mcme_mesh_free(&nm);
                r->text = refuse_text();
                goto out;
            }
            beside[s - 1] = nm.transparency;
            mcme_mesh_free(&nm);
        }

        /* The world's own cull (G10): both halves of the answer —
           the faces the block brought, the faces its sides keep —
           counted before and after for the settle line. */
        r->faces_total = mesh_faces(&r->mesh->mesh);
        if (!mcme_mesh_cull_dirs(&r->mesh->mesh, beside)) {
            r->text = g_strdup("out of memory");
            goto out;
        }
        r->faces = mesh_faces(&r->mesh->mesh);
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
    for (int s = 0; s < 7; ++s) g_free(text[s]);
    if (arena != NULL) mcme_arena_free(arena);
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
