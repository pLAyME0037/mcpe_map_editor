#include "mcme/resource.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "mcme/core.h"

/*
 * The block-level mesh — one block state walked to its winning
 * applies, each meshed and turned by the selection's x/y —
 * mirroring the reference's `_get_model` composed of
 * `_load_blockstate_model` (D33, decisions.md#D33).
 *
 * The reference catches every load failure per apply, so the walk
 * skips what does not mesh instead of failing: a variant whose
 * model refuses is passed over for the next matching key, a
 * multipart case whose model refuses merges nothing, and an apply
 * without a model stands in as the missing BLOCK — accepted as it
 * stands, never turned. Only a selection nothing survived (or none
 * matched at all) answers the missing block whole; a multipart
 * that matched nothing answers the EMPTY mesh, its merge of no
 * models.
 *
 * The rotation steps are degrees floored the Python way (x -45 is
 * step -1, not the0 C's truncation hands back) and carry the
 * reference's guard quirks through mcme_mesh_rotate: an out-of-
 * range step with a nonzero x raises there — logged, skipped, the
 * walk carries on.
 */

/* One winning apply, meshed. */
struct BlockMeshCtx {
    const McmeModelIndex  *models;
    const McmeTextureIndex *textures;
    McmeMesh *parts;
    size_t    count, cap;
    /* The block's tint (D44): its colour for every apply's build,
       and the reference's own raise flag when the lookup raised —
       then every apply refuses, exactly its caught exception. */
    double    tint[3];
    bool      tint_raised;
};

static void parts_free(struct BlockMeshCtx *ctx)
{
    for (size_t i = 0; i < ctx->count; ++i) mcme_mesh_free(&ctx->parts[i]);
    free(ctx->parts);
    ctx->parts = NULL;
    ctx->count = ctx->cap = 0;
}

/* The reference's `_load_blockstate_model` for one apply: its
   model meshed and turned, or false when either raised there —
   the caller's walk skips the apply either way. */
static bool take_apply(const McmeModelRef *ref, void *ud)
{
    struct BlockMeshCtx *ctx = ud;

    if (ctx->tint_raised) {
        /* The reference's `_get_block_tint(block)` raised inside
           this apply's own try: caught, logged, this apply
           refused (D44). Every apply refusing is the fall-through
           the walk already answers — the missing block for
           variants, the empty merge for multipart. */
        return false;
    }

    McmeMesh mesh;
    if (!ref->model) {
        /* No model: the reference answers its missing mesh here —
           unturned, because its rotate call sits under the model
           load. */
        if (!mcme_mesh_missing_block(&mesh)) return false;
    } else {
        /* The model reference splits the way the fold does: the
           first ':' separates, absence means minecraft. */
        const char *colon = strchr(ref->model, ':');
        char *ns = NULL, *path = NULL;
        if (colon) {
            size_t n = (size_t)(colon - ref->model);
            ns = malloc(n + 1);
            if (!ns) return false;
            memcpy(ns, ref->model, n);
            ns[n] = '\0';
            path = malloc(strlen(colon + 1) + 1);
            if (path) strcpy(path, colon + 1);
            if (!path) { free(ns); return false; }
        } else {
            path = malloc(strlen(ref->model) + 1);
            if (!path) return false;
            strcpy(path, ref->model);
        }

        bool built = mcme_mesh_build_tinted(ctx->models, ctx->textures,
                                            ns, path, ctx->tint, &mesh);
        free(ns);
        free(path);
        if (!built) return false; /* the reference's caught load */

        /* Degrees to quarter-turns, floored the way Python floors:
           -45 is step -1, the turn the reference would make. */
        int32_t rotx = (int32_t)floor((double)ref->x / 90.0);
        int32_t roty = (int32_t)floor((double)ref->y / 90.0);
        if (!mcme_mesh_rotate(&mesh, rotx, roty)) {
            mcme_mesh_free(&mesh);
            mcme_clear_error(); /* the reference's KeyError, caught:
                                   this apply is skipped, not fatal */
            return false;
        }
    }

    if (ctx->count == ctx->cap) {
        size_t next = ctx->cap ? ctx->cap * 2 : 4;
        McmeMesh *grown = realloc(ctx->parts, next * sizeof *grown);
        if (!grown) {
            mcme_mesh_free(&mesh);
            return false;
        }
        ctx->parts = grown;
        ctx->cap = next;
    }
    ctx->parts[ctx->count++] = mesh;
    return true;
}

bool mcme_mesh_for_block(const McmeBlockstateIndex *blockstates,
                         const McmeModelIndex *models,
                         const McmeTextureIndex *textures,
                         const char *namespace, const char *base_name,
                         const McmeState *states, size_t state_count,
                         McmeMesh *out)
{
    if (!blockstates || !models || !textures || !namespace ||
        !base_name || !out) {
        mcme_set_error("block mesh: null argument");
        return false;
    }

    struct BlockMeshCtx ctx = { models, textures, NULL, 0, 0,
                                { 1.0, 1.0, 1.0 }, false };
    /* The block's own tint before any selection (D44): the
       reference evaluates it inside each matched branch's try, and
       a raise there is caught per apply — this flag refuses every
       apply below, which the walk already turns into its missing
       block (variants) or empty merge (multipart). */
    if (!mcme_block_tint(namespace, base_name, states, state_count,
                         ctx.tint))
        ctx.tint_raised = true;
    McmeSelectKind kind;
    if (!mcme_blockstate_select(blockstates, namespace, base_name, states,
                                state_count, take_apply, &ctx, &kind)) {
        parts_free(&ctx);
        return false; /* null argument, already reported */
    }

    bool ok;
    switch (kind) {
    case MCME_SELECT_VARIANTS:
        /* Exactly one apply was accepted: the first that meshed. */
        *out = ctx.parts[0];
        free(ctx.parts);
        return true;

    case MCME_SELECT_MULTIPART: {
        /* The parts are one owned array; merge wants them by
           pointer. */
        const McmeMesh **byref = ctx.count
            ? malloc(ctx.count * sizeof *byref) : NULL;
        if (ctx.count && !byref) {
            parts_free(&ctx);
            mcme_set_error("block mesh: out of memory");
            return false;
        }
        for (size_t i = 0; i < ctx.count; ++i) byref[i] = &ctx.parts[i];
        ok = mcme_mesh_merge(byref, ctx.count, out);
        free(byref);
        parts_free(&ctx);
        return ok;
    }

    case MCME_SELECT_MISSING:
    default:
        parts_free(&ctx);
        return mcme_mesh_missing_block(out);
    }
}
