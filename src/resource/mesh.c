#include "mcme/resource.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mcme/core.h"
#include "cJSON.h"

/*
 * The mesh — one folded model turned into vertices, faces and
 * texture coordinates, mirroring the reference's
 * `_load_block_model` rule for rule (decisions.md#D32).
 *
 * The geometry tables are the reference's own (its cube module):
 * the unit box's eight corners in `product(x, y, z)` order — index
 * i reads (x = i>>2, y = (i>>1)&1, z = i&1) — each face using four
 * of them in a winding that faces outward, and a UV corner
 * remapping that turns a face's texture rectangle to match. Every
 * model coordinate is a 0..16 number divided by 16, so unrotated
 * geometry is dyadic and exact.
 *
 * The rules that look like inventions are the reference's:
 *
 *   - A model with textures but no elements answers the missing
 *     BLOCK (a unit cube of the missing image, opaque), not an
 *     empty mesh, and an empty `elements` list counts as none —
 *     the reference tests truthiness, not presence.
 *   - A model with elements but no textures dict at all fails to
 *     mesh: the reference indexes the key outright and raises.
 *   - Texture entries deduplicate by their path under textures/
 *     NAMESPACE-BLIND, so `block/x` and `other:block/x` share one
 *     slot and the first face to use them names the path.
 *   - Only a full-size unrotated element with six or more faces
 *     turns the block translucent and earns the per-face opacity
 *     check; six opaque faces make the block opaque. The check
 *     reads the transparency table — and a texture the table does
 *     not hold RAISES there, so a full block whose texture the
 *     pack lacks does not mesh: mcme refuses it by name.
 *
 * Refusals are where the reference itself raises and its callers
 * degrade the whole selection: a face without a string texture, a
 * self-referencing `#key` chain, a malformed UV, an element whose
 * box, rotation or faces are shapes numpy would refuse. A model
 * the pack does not know is the empty mesh — an answer.
 *
 * The blockstate selection's rotation of the whole mesh (x, y,
 * and the reference's ignored uvlock), the multipart merge and
 * the block-level composition of both are layer D33's — see
 * mcme_mesh_missing_block, mcme_mesh_rotate, mcme_mesh_merge and
 * mcme_mesh_for_block (blockmesh.c) below.
 */

/* The reference's cube tables (its cube module). */
static const int cube_face_lut[6][4] = {
    {0, 4, 5, 1}, /* down  */
    {3, 7, 6, 2}, /* up    */
    {4, 0, 2, 6}, /* north */
    {5, 4, 6, 7}, /* east  */
    {1, 5, 7, 3}, /* south */
    {0, 1, 3, 2}, /* west  */
};
static const uint32_t tri_face[6] = {0, 1, 2, 0, 2, 3};
static const int uv_rotation_lut[8] = {0, 3, 2, 3, 2, 1, 0, 1};

/* Which of the six face directions a key names, or -1. */
static int face_dir_index(const char *name)
{
    static const char *const names[6] = {
        "down", "up", "north", "east", "south", "west" };
    if (!name) return -1;
    for (int i = 0; i < 6; ++i)
        if (strcmp(name, names[i]) == 0) return i;
    return -1;
}

/* Python truthiness of a JSON value, the reference's `not
   elements` test: absent, null, false, zero, "", [] and {} are all
   falsy. */
static bool json_falsy(const struct cJSON *v)
{
    if (!v || cJSON_IsNull(v) || cJSON_IsFalse(v)) return true;
    if (cJSON_IsNumber(v)) return v->valuedouble == 0;
    if (cJSON_IsString(v)) return !v->valuestring || v->valuestring[0] == '\0';
    if (cJSON_IsArray(v)) return v->child == NULL;
    if (cJSON_IsObject(v)) return v->child == NULL;
    return false;
}

/* Does this textures dict name any key besides "particle"? The
   value here is a dict or absent: the fold drops any other shape —
   the reference raises on its `.items()` while folding, recorded
   against D31's own drop. */
static bool textures_beyond_particle(const struct cJSON *tex)
{
    if (!cJSON_IsObject(tex)) return false;
    for (const struct cJSON *kv = tex->child; kv; kv = kv->next)
        if (!kv->string || strcmp(kv->string, "particle") != 0) return true;
    return false;
}

/* Copy `n` bytes of `s` into a fresh NUL-terminated string, or NULL
   on OOM. */
static char *ndup(const char *s, size_t n)
{
    char *out = malloc(n + 1);
    if (!out) return NULL;
    memcpy(out, s, n);
    out[n] = '\0';
    return out;
}

/* ========================================================================
 * The build context: per-bucket growth counters plus the intern
 * table. The public buckets carry vertex and face COUNTS; the
 * arrays grow here under their own counters, which the counts are
 * derived from (12 floats of positions, 8 of texture coordinates,
 * 12 of tint and 6+2 indices per face).
 * ======================================================================== */

typedef struct {
    McmeMesh *mesh;
    size_t v_n[7], v_cap[7];   /* positions, floats  */
    size_t t_n[7], t_cap[7];   /* texture coords     */
    size_t i_n[7], i_cap[7];   /* tint               */
    size_t f_n[7], f_cap[7];   /* face indices       */
    size_t x_n[7], x_cap[7];   /* texture indexes    */
    /* The reference dedups by the path under textures/ while the
       list carries resolved file paths, so the intern table keeps
       both halves. */
    char **rels;
    char **paths;
    size_t count;
    size_t cap;
} BuildCtx;

static void bucket_free(McmeMeshBucket *b)
{
    free(b->verts);
    free(b->tverts);
    free(b->tint_verts);
    free(b->faces);
    free(b->texture_indexes);
    *b = (McmeMeshBucket){ NULL, NULL, NULL, NULL, NULL, 0, 0 };
}

void mcme_mesh_free(McmeMesh *mesh)
{
    if (!mesh) return;
    for (int i = 0; i < 7; ++i) bucket_free(&mesh->buckets[i]);
    for (size_t i = 0; i < mesh->texture_count; ++i)
        free(mesh->textures[i]);
    free(mesh->textures);
    memset(mesh, 0, sizeof *mesh);
    mesh->transparency = MCME_TRANSPARENCY_PARTIAL;
}

static void ctx_free(BuildCtx *ctx)
{
    for (size_t i = 0; i < ctx->count; ++i) {
        free(ctx->rels[i]);
        free(ctx->paths[i]);
    }
    free(ctx->rels);
    free(ctx->paths);
    ctx->rels = ctx->paths = NULL;
    ctx->count = ctx->cap = 0;
}

/* Frees the dedup keys alone — the success paths hand the paths
   half to the mesh and drop the keys. */
static void ctx_free_rels(BuildCtx *ctx)
{
    for (size_t i = 0; i < ctx->count; ++i) free(ctx->rels[i]);
    free(ctx->rels);
    ctx->rels = NULL;
}

/* One number appended, growing its array. Doubles, all of them: the
   reference's numpy keeps float64, and a float32 vertex would drift
   by a visible step the moment rotation made it irrational. */
static bool put_double(double **arr, size_t *n, size_t *cap, double v)
{
    if (*n == *cap) {
        size_t next = *cap ? *cap * 2 : 16;
        double *grown = realloc(*arr, next * sizeof *grown);
        if (!grown) return false;
        *arr = grown;
        *cap = next;
    }
    (*arr)[(*n)++] = v;
    return true;
}

/* One index appended, growing its array. */
static bool put_u32(uint32_t **arr, size_t *n, size_t *cap, uint32_t v)
{
    if (*n == *cap) {
        size_t next = *cap ? *cap * 2 : 16;
        uint32_t *grown = realloc(*arr, next * sizeof *grown);
        if (!grown) return false;
        *arr = grown;
        *cap = next;
    }
    (*arr)[(*n)++] = v;
    return true;
}

/* Interns one face's texture: deduplicated by the path under
   textures/ — namespace-blind, the reference's own key — with the
   resolved file path carried alongside. Returns the index, or
   (uint32_t)-1 on OOM. */
static uint32_t intern_texture(BuildCtx *ctx, const char *rel,
                               const char *path)
{
    for (size_t i = 0; i < ctx->count; ++i)
        if (strcmp(ctx->rels[i], rel) == 0) return (uint32_t)i;

    if (ctx->count == ctx->cap) {
        size_t next = ctx->cap ? ctx->cap * 2 : 8;
        char **r = realloc(ctx->rels, next * sizeof *r);
        if (!r) return (uint32_t)-1;
        ctx->rels = r;
        char **p = realloc(ctx->paths, next * sizeof *p);
        if (!p) return (uint32_t)-1;
        ctx->paths = p;
        ctx->cap = next;
    }
    ctx->rels[ctx->count]  = ndup(rel, strlen(rel));
    ctx->paths[ctx->count] = ndup(path, strlen(path));
    if (!ctx->rels[ctx->count] || !ctx->paths[ctx->count]) {
        free(ctx->rels[ctx->count]);
        free(ctx->paths[ctx->count]);
        return (uint32_t)-1;
    }
    return (uint32_t)ctx->count++;
}

/* ========================================================================
 * Faces
 * ======================================================================== */

/* The reference's find_texture: walk the `#key` chain one '#' at a
   time. A repeated key is refused (the reference raises
   "referenced itself"); a key the textures dict does not hold
   answers itself; a non-string value ends the walk and is handed
   back for the caller's `{sprite: ...}` pass-through. Exactly one
   of `*out` (a heap string) or `*value_out` (a borrowed value) is
   set on success. */
static bool find_texture(const struct cJSON *tex_map, const char *key,
                         char **out, const struct cJSON **value_out)
{
    *out = NULL;
    *value_out = NULL;

    char *cur = ndup(key, strlen(key));
    char **seen = NULL;
    size_t nseen = 0;
    bool ok = cur != NULL;

    while (ok) {
        const char *bare = cur[0] == '#' ? cur + 1 : cur;

        for (size_t i = 0; i < nseen; ++i) {
            if (strcmp(seen[i], bare) == 0) {
                mcme_set_error("mesh: texture '#%.100s' references "
                               "itself", bare);
                ok = false;
                break;
            }
        }
        if (!ok) break;

        char **grown = realloc(seen, (nseen + 1) * sizeof *grown);
        if (!grown) { ok = false; break; }
        seen = grown;
        seen[nseen] = ndup(bare, strlen(bare));
        if (!seen[nseen]) { ok = false; break; }
        nseen++;

        const struct cJSON *v =
            cJSON_GetObjectItemCaseSensitive(tex_map, bare);
        if (!v || cJSON_IsNull(v)) {
            /* The reference answers the key itself: its dict.get
               returns None for an absent key and a JSON null alike. */
            *out = ndup(bare, strlen(bare));
            ok = *out != NULL;
            break;
        }
        if (!cJSON_IsString(v)) { /* pass-through for sprite */
            *value_out = v;
            break;
        }

        char *next = ndup(v->valuestring, strlen(v->valuestring));
        free(cur);
        cur = next;
        if (!cur) { ok = false; break; }
    }

    free(cur);
    for (size_t i = 0; i < nseen; ++i) free(seen[i]);
    free(seen);
    if (!ok) {
        free(*out);
        *out = NULL;
        *value_out = NULL;
    }
    return ok;
}

/* The reference's rotate_3d: a row-vector times the axis matrix,
   about the given origin, optionally rescaled so the rotated box
   keeps its 0..1 bounds. */
static void rotate_3d(double verts[4][3], double rx, double ry, double rz,
                      double dx, double dy, double dz, bool rescale)
{
    double rad[3] = { rx * M_PI / 180.0, ry * M_PI / 180.0,
                      rz * M_PI / 180.0 };
    double sx = sin(rad[0]), cx = cos(rad[0]);
    double sy = sin(rad[1]), cy = cos(rad[1]);
    double sz = sin(rad[2]), cz = cos(rad[2]);
    double m[3][3] = {
        { cz * cy,                sz * cy,                -sy },
        { cz * sy * sx - sz * cx, sz * sy * sx + cz * cx, cy * sx },
        { cz * sy * cx + sz * sx, sz * sy * cx - cz * sx, cy * cx },
    };
    /* max(cos_a * cos_b, 1e-6, key=abs): the larger by absolute
       value, keeping a negative product's sign. */
    double denom[3] = { fabs(cy * cz) > 1e-6 ? cy * cz : 1e-6,
                        fabs(cx * cz) > 1e-6 ? cx * cz : 1e-6,
                        fabs(cx * cy) > 1e-6 ? cx * cy : 1e-6 };

    for (int v = 0; v < 4; ++v) {
        double o[3] = { verts[v][0] - dx, verts[v][1] - dy,
                        verts[v][2] - dz };
        if (rescale) {
            o[0] /= denom[0];
            o[1] /= denom[1];
            o[2] /= denom[2];
        }
        double d[3] = { dx, dy, dz };
        for (int j = 0; j < 3; ++j)
            verts[v][j] = o[0] * m[0][j] + o[1] * m[1][j] +
                          o[2] * m[2][j] + d[j];
    }
}

/* Emits one face into its cull bucket: four vertices with their
   texture coordinates and tint, the two triangles' vertex
   indices, and the face's texture index. `k2` is the UV corner
   cut, already resolved the reference's way. */
static bool emit_face(BuildCtx *ctx, int dir, int cull,
                      const double box[8][3], const double uv[4],
                      int k2, uint32_t tex_index, bool have_rot,
                      const double rot[7])
{
    McmeMesh *mesh = ctx->mesh;
    mesh->present[cull] = true;

    double verts[4][3];
    for (int v = 0; v < 4; ++v)
        for (int j = 0; j < 3; ++j)
            verts[v][j] = box[cube_face_lut[dir][v]][j];
    if (have_rot)
        rotate_3d(verts, rot[0], rot[1], rot[2], rot[3], rot[4], rot[5],
                  rot[6] != 0);

    for (int v = 0; v < 4; ++v) {
        for (int j = 0; j < 3; ++j)
            if (!put_double(&mesh->buckets[cull].verts,
                           &ctx->v_n[cull], &ctx->v_cap[cull],
                           verts[v][j]))
                return false;
        for (int j = 0; j < 2; ++j) {
            int corner = uv_rotation_lut[(k2 + v * 2 + j) % 8];
            if (!put_double(&mesh->buckets[cull].tverts,
                           &ctx->t_n[cull], &ctx->t_cap[cull],
                           uv[corner]))
                return false;
        }
        for (int j = 0; j < 3; ++j)
            if (!put_double(&mesh->buckets[cull].tint_verts,
                           &ctx->i_n[cull], &ctx->i_cap[cull], 1.0))
                return false;
    }

    uint32_t base = (uint32_t)(ctx->v_n[cull] / 3) - 4;
    for (int i = 0; i < 6; ++i)
        if (!put_u32(&mesh->buckets[cull].faces,
                     &ctx->f_n[cull], &ctx->f_cap[cull],
                     tri_face[i] + base))
            return false;
    for (int i = 0; i < 2; ++i)
        if (!put_u32(&mesh->buckets[cull].texture_indexes,
                     &ctx->x_n[cull], &ctx->x_cap[cull], tex_index))
            return false;
    return true;
}

/* ========================================================================
 * Elements and the model
 * ======================================================================== */

/* Reads a three-number coordinate list ([0,0,0] / [16,16,16] /
   a rotation origin), the reference handing numpy an array of
   three and dividing by 16. A shape numpy would refuse is a
   refusal here. */
static bool read_box(const struct cJSON *item, double out[3],
                     const double dflt[3], const char *what)
{
    for (int i = 0; i < 3; ++i) out[i] = dflt[i];
    if (!item) return true;
    if (!cJSON_IsArray(item)) {
        mcme_set_error("mesh: element %s is not a list of three numbers",
                       what);
        return false;
    }
    const struct cJSON *n = item->child;
    for (int i = 0; i < 3; ++i) {
        if (!n || !(cJSON_IsNumber(n) || cJSON_IsBool(n))) {
            mcme_set_error("mesh: element %s is not a list of three "
                           "numbers", what);
            return false;
        }
        out[i] = cJSON_IsBool(n) ? (cJSON_IsTrue(n) ? 1 : 0)
                                 : n->valuedouble;
        n = n->next;
    }
    if (n) {
        /* Too many numbers: the reference's numpy would still have
           built a stranger box from from/to (mcme refuses that
           domain, recorded) and its origin unpack raises — both
           answer as this refusal. */
        mcme_set_error("mesh: element %s is not a list of three numbers",
                       what);
        return false;
    }
    return true;
}

static bool vec_eq(const double v[3], double x, double y, double z)
{
    return v[0] == x && v[1] == y && v[2] == z;
}

/* One element: its box, its rotation, and every face it names. */
static bool build_element(BuildCtx *ctx, const struct cJSON *el,
                          const struct cJSON *tex_map,
                          const McmeTextureIndex *textures)
{
    if (!cJSON_IsObject(el)) {
        mcme_set_error("mesh: an element is not an object");
        return false;
    }

    const struct cJSON *faces =
        cJSON_GetObjectItemCaseSensitive(el, "faces");
    if (faces && !cJSON_IsObject(faces)) {
        mcme_set_error("mesh: element faces is not an object");
        return false;
    }
    size_t face_raw_count = 0;
    if (cJSON_IsObject(faces))
        for (const struct cJSON *f = faces->child; f; f = f->next)
            face_raw_count++;

    static const double dflt_from[3] = { 0, 0, 0 };
    static const double dflt_to[3]   = { 16, 16, 16 };
    double from[3], to[3];
    if (!read_box(cJSON_GetObjectItemCaseSensitive(el, "from"),
                  from, dflt_from, "from") ||
        !read_box(cJSON_GetObjectItemCaseSensitive(el, "to"),
                  to, dflt_to, "to"))
        return false;

    const struct cJSON *rotation =
        cJSON_GetObjectItemCaseSensitive(el, "rotation");
    bool has_rotation = rotation && !cJSON_IsNull(rotation);

    /* The full-block test, the reference's own: not yet opaque,
       no element rotation, exactly the full box, six or more
       faces named (garbage directions included — it counts the
       raw keys). */
    bool check_faces = false;
    if (ctx->mesh->transparency != MCME_TRANSPARENCY_OPAQUE &&
        !has_rotation && vec_eq(from, 0, 0, 0) && vec_eq(to, 16, 16, 16) &&
        face_raw_count >= 6) {
        ctx->mesh->transparency = MCME_TRANSPARENCY_TRANSLUCENT;
        check_faces = true;
    }
    int opaque_face_count = 0;

    /* The box's corners: per-axis sorted from/to scaled to the
       unit cube; corner i reads (x = i>>2, y = (i>>1)&1, z = i&1). */
    double lo[3], hi[3], box[8][3];
    for (int j = 0; j < 3; ++j) {
        lo[j] = (from[j] < to[j] ? from[j] : to[j]) / 16.0;
        hi[j] = (from[j] < to[j] ? to[j] : from[j]) / 16.0;
    }
    for (int i = 0; i < 8; ++i) {
        box[i][0] = ((i >> 2) & 1) ? hi[0] : lo[0];
        box[i][1] = ((i >> 1) & 1) ? hi[1] : lo[1];
        box[i][2] = (i & 1)        ? hi[2] : lo[2];
    }

    /* The element rotation: origin, axis angle, the multi-axis
       overrides a 1.21.11 pack may carry, and rescale. Where the
       reference's numpy would refuse a shape, mcme refuses by
       name. */
    bool have_rot = false;
    double rot[7] = { 0, 0, 0, 0.5, 0.5, 0.5, 0 };
    if (has_rotation) {
        if (!cJSON_IsObject(rotation)) {
            mcme_set_error("mesh: element rotation is not an object");
            return false;
        }
        const struct cJSON *origin =
            cJSON_GetObjectItemCaseSensitive(rotation, "origin");
        if (origin) {
            double o[3];
            static const double dflt_origin[3] = { 8, 8, 8 };
            if (!read_box(origin, o, dflt_origin, "rotation origin"))
                return false;
            rot[3] = o[0] / 16.0;
            rot[4] = o[1] / 16.0;
            rot[5] = o[2] / 16.0;
        }
        double angle = 0;
        const struct cJSON *ang =
            cJSON_GetObjectItemCaseSensitive(rotation, "angle");
        if (ang) {
            if (!cJSON_IsNumber(ang)) {
                mcme_set_error("mesh: element rotation angle is not "
                               "a number");
                return false;
            }
            angle = ang->valuedouble;
        }
        const struct cJSON *axis =
            cJSON_GetObjectItemCaseSensitive(rotation, "axis");
        if (!axis) {
            rot[0] = angle; /* the default axis is "x" */
        } else if (cJSON_IsString(axis)) {
            if (strcmp(axis->valuestring, "x") == 0)      rot[0] = angle;
            else if (strcmp(axis->valuestring, "y") == 0) rot[1] = angle;
            else if (strcmp(axis->valuestring, "z") == 0) rot[2] = angle;
            /* an axis the game does not name leaves all three at
               zero, the reference's if/elif chain falling through */
        }
        /* an axis present but not a string compares equal to no
           direction there; zeros stand */
        static const char *const over[3] = { "x", "y", "z" };
        for (int j = 0; j < 3; ++j) {
            const struct cJSON *v =
                cJSON_GetObjectItemCaseSensitive(rotation, over[j]);
            if (v) {
                if (!cJSON_IsNumber(v)) {
                    mcme_set_error("mesh: element rotation %s is not a "
                                   "number", over[j]);
                    return false;
                }
                rot[j] = v->valuedouble; /* the 1.21.11 overrides */
            }
        }
        rot[6] = cJSON_IsTrue(
            cJSON_GetObjectItemCaseSensitive(rotation, "rescale")) ? 1 : 0;
        have_rot = true;
    }

    /* The reference indexes the textures dict outright for every
       element it reaches; a model with elements and no textures
       dict fails to mesh there, and a value that is not a dict
       fails on its `.get` the moment a face resolves. */
    if (!tex_map || !cJSON_IsObject(tex_map)) {
        mcme_set_error("mesh: the model has elements but no textures");
        return false;
    }

    if (!cJSON_IsObject(faces)) return true; /* no faces: the box alone */

    for (const struct cJSON *kv = faces->child; kv; kv = kv->next) {
        int dir = face_dir_index(kv->string);
        if (dir < 0) continue; /* the reference's own skip */

        const struct cJSON *face = kv;
        if (!cJSON_IsObject(face)) {
            mcme_set_error("mesh: a face of direction '%.40s' is not an "
                           "object", kv->string ? kv->string : "");
            return false;
        }

        int cull = MCME_CULL_NONE;
        const struct cJSON *cullface =
            cJSON_GetObjectItemCaseSensitive(face, "cullface");
        if (cJSON_IsString(cullface)) {
            int c = face_dir_index(cullface->valuestring);
            if (c >= 0) cull = c; /* anything else culls nowhere */
        }

        /* The texture: a #chain, or a {sprite: ...} pass-through.
           A face with neither a string nor that object fails the
           whole mesh — the reference's .split would raise. */
        const struct cJSON *texval =
            cJSON_GetObjectItemCaseSensitive(face, "texture");
        char *rel = NULL;
        const struct cJSON *landed = NULL;
        if (cJSON_IsString(texval)) {
            if (!find_texture(tex_map, texval->valuestring, &rel, &landed))
                return false;
        } else if (cJSON_IsObject(texval)) {
            landed = texval;
        } else {
            mcme_set_error("mesh: the face '%.40s' has no texture "
                           "string", kv->string ? kv->string : "");
            return false;
        }
        if (landed) {
            /* The pass-through: {sprite: ...} names the path, and
               a non-object landing (a number, null) fails the
               mesh — the reference's .split would raise. */
            if (!cJSON_IsObject(landed)) {
                mcme_set_error("mesh: the face '%.40s' resolves to a "
                               "non-string texture",
                               kv->string ? kv->string : "");
                return false;
            }
            const struct cJSON *sprite =
                cJSON_GetObjectItemCaseSensitive(landed, "sprite");
            if (!cJSON_IsString(sprite)) {
                mcme_set_error("mesh: the face '%.40s' has a texture "
                               "object without a sprite string",
                               kv->string ? kv->string : "");
                return false;
            }
            rel = ndup(sprite->valuestring, strlen(sprite->valuestring));
            if (!rel) return false;
        }

        /* Namespace split, minecraft the default; a namespace too
           long for any directory is simply not in the table. */
        char ns[256];
        const char *base;
        const char *colon = strchr(rel, ':');
        bool ns_too_long = false;
        if (colon) {
            size_t n = (size_t)(colon - rel);
            base = colon + 1;
            if (n >= sizeof ns) ns_too_long = true;
            else {
                memcpy(ns, rel, n);
                ns[n] = '\0';
            }
        } else {
            snprintf(ns, sizeof ns, "minecraft");
            base = rel;
        }
        const char *texture_path =
            ns_too_long ? "missing_no"
                        : mcme_texture_path(textures, ns, base);

        if (check_faces) {
            /* The reference reads its transparency table here, and
               a path the table does not hold — the missing image —
               raises: a full block whose texture the pack lacks
               does not mesh. */
            if (strcmp(texture_path, "missing_no") == 0) {
                mcme_set_error("mesh: the full block references "
                               "'%.100s', a texture the pack does not "
                               "hold", rel);
                free(rel);
                return false;
            }
            if (mcme_texture_transparent(textures, texture_path))
                check_faces = false;
            else
                opaque_face_count++;
        }

        /* Interned by the path under textures/ — the split half,
           namespace-blind, the reference's own dedup key. */
        uint32_t tex_index = intern_texture(ctx, base, texture_path);
        if (tex_index == (uint32_t)-1) {
            mcme_set_error("mesh: out of memory");
            free(rel);
            return false;
        }

        /* UV: the face's own four numbers, or the box-derived
           default for the direction. */
        double uv[4];
        const struct cJSON *uvj = cJSON_GetObjectItemCaseSensitive(face, "uv");
        if (!uvj) {
            if (dir == 0 || dir == 1) { /* up / down */
                uv[0] = from[0]; uv[1] = from[2];
                uv[2] = to[0];   uv[3] = to[2];
            } else if (dir == 2 || dir == 4) { /* north / south */
                uv[0] = from[0]; uv[1] = 16 - to[1];
                uv[2] = to[0];   uv[3] = 16 - from[1];
            } else { /* east / west */
                uv[0] = from[2]; uv[1] = 16 - to[1];
                uv[2] = to[2];   uv[3] = 16 - from[1];
            }
            for (int i = 0; i < 4; ++i) uv[i] /= 16.0;
        } else {
            if (!cJSON_IsArray(uvj)) {
                mcme_set_error("mesh: the face '%.40s' has an invalid "
                               "uv value", kv->string ? kv->string : "");
                free(rel);
                return false;
            }
            const struct cJSON *n = uvj->child;
            for (int i = 0; i < 4; ++i) {
                if (!n || !(cJSON_IsNumber(n) || cJSON_IsBool(n))) {
                    mcme_set_error("mesh: the face '%.40s' has an "
                                   "invalid uv value",
                                   kv->string ? kv->string : "");
                    free(rel);
                    return false;
                }
                uv[i] = (cJSON_IsBool(n) ? (cJSON_IsTrue(n) ? 1 : 0)
                                         : n->valuedouble) / 16.0;
                n = n->next;
            }
        }

        /* The face's own rotation: a cut of the corner lut, taken
           the reference's Python way — the slice past either end
           clamps to the whole lut, a negative slice wraps from the
           end. */
        int k2 = 0;
        const struct cJSON *rotj =
            cJSON_GetObjectItemCaseSensitive(face, "rotation");
        if (rotj) {
            if (!cJSON_IsNumber(rotj)) {
                mcme_set_error("mesh: the face '%.40s' has a non-number "
                               "rotation", kv->string ? kv->string : "");
                free(rel);
                return false;
            }
            int k = (int)(rotj->valuedouble / 90); /* truncates, as int() */
            k2 = 2 * k;
            if (k2 >= 8 || k2 <= -8) k2 = 0; /* the clamped slice */
            else k2 = ((k2 % 8) + 8) % 8;
        }

        bool ok = emit_face(ctx, dir, cull, box, uv, k2, tex_index,
                            have_rot, rot);
        free(rel);
        if (!ok) {
            mcme_set_error("mesh: out of memory");
            return false;
        }
    }

    /* Six opaque faces make the block opaque — the reference
       settles it after each element's faces. */
    if (opaque_face_count == 6)
        ctx->mesh->transparency = MCME_TRANSPARENCY_OPAQUE;
    return true;
}

/* The missing BLOCK: the reference's unit cube of the missing
   image, opaque, every face under its own direction. */
bool mcme_mesh_missing_block(McmeMesh *out)
{
    if (!out) {
        mcme_set_error("missing block: null argument");
        return false;
    }
    memset(out, 0, sizeof *out);
    out->transparency = MCME_TRANSPARENCY_PARTIAL;

    BuildCtx ctx;
    memset(&ctx, 0, sizeof ctx);
    ctx.mesh = out;

    double box[8][3];
    for (int i = 0; i < 8; ++i) {
        box[i][0] = ((i >> 2) & 1) ? 1 : 0;
        box[i][1] = ((i >> 1) & 1) ? 1 : 0;
        box[i][2] = (i & 1)        ? 1 : 0;
    }
    double uv[4] = { 0, 0, 1, 1 };
    for (int dir = 0; dir < 6; ++dir) {
        uint32_t tex = intern_texture(&ctx, "missing_no", "missing_no");
        if (tex == (uint32_t)-1 ||
            !emit_face(&ctx, dir, dir, box, uv, 0, tex, false, NULL)) {
            mcme_set_error("mesh: out of memory");
            ctx_free(&ctx);
            return false;
        }
    }
    out->textures = ctx.paths; /* the rels half was only the dedup key */
    out->texture_count = ctx.count;
    ctx_free_rels(&ctx);
    for (int i = 0; i < 7; ++i) {
        out->buckets[i].vert_count = ctx.v_n[i] / 3;
        out->buckets[i].face_count = ctx.f_n[i] / 6;
    }
    out->transparency = MCME_TRANSPARENCY_OPAQUE;
    return true;
}

bool mcme_mesh_build(const McmeModelIndex *models,
                     const McmeTextureIndex *textures,
                     const char *namespace, const char *path,
                     McmeMesh *out)
{
    if (!models || !textures || !path || !out) {
        mcme_set_error("mesh build: null argument");
        return false;
    }

    memset(out, 0, sizeof *out);
    out->transparency = MCME_TRANSPARENCY_PARTIAL;

    struct cJSON *doc = NULL;
    if (!mcme_model_load(models, namespace ? namespace : "minecraft",
                         path, &doc))
        return false; /* the fold refused; its error names it */

    const struct cJSON *tex_map =
        cJSON_GetObjectItemCaseSensitive(doc, "textures");
    const struct cJSON *elements =
        cJSON_GetObjectItemCaseSensitive(doc, "elements");

    if (textures_beyond_particle(tex_map) && json_falsy(elements)) {
        /* The reference answers its missing BLOCK here, not an
           empty mesh: textures, nothing to draw them on. */
        cJSON_Delete(doc);
        return mcme_mesh_missing_block(out);
    }

    BuildCtx ctx;
    memset(&ctx, 0, sizeof ctx);
    ctx.mesh = out;

    if (cJSON_IsArray(elements)) {
        for (const struct cJSON *el = elements->child; el; el = el->next)
            if (!build_element(&ctx, el, tex_map, textures)) {
                mcme_mesh_free(out);
                ctx_free(&ctx);
                cJSON_Delete(doc);
                return false;
            }
    } else if (elements &&
               (!json_falsy(elements) || cJSON_IsNull(elements) ||
                cJSON_IsFalse(elements) ||
                (cJSON_IsNumber(elements) && elements->valuedouble == 0))) {
        /* Present and not a list: the reference's element loop
           reaches its first step either way — a truthy shape yields
           an element that is not an object and raises, and null,
           false and zero are not iterable at all. Only "" and `{}`
           run zero turns there and answer the empty mesh. */
        mcme_set_error("mesh: elements is not a list");
        mcme_mesh_free(out);
        ctx_free(&ctx);
        cJSON_Delete(doc);
        return false;
    }

    /* Hand the intern table's paths to the mesh (the rels half was
       only the dedup key) and settle the counts. */
    out->textures = ctx.paths;
    out->texture_count = ctx.count;
    ctx_free_rels(&ctx);
    for (int i = 0; i < 7; ++i) {
        out->buckets[i].vert_count  = ctx.v_n[i] / 3;
        out->buckets[i].face_count  = ctx.f_n[i] / 6;
    }
    cJSON_Delete(doc);
    return true;
}

/* ========================================================================
 * Rotation and merge — the selection's x/y over a finished mesh,
 * and multipart's many meshes into one (D33)
 * ======================================================================== */

/* Python's `%` on the step numbers: the reference's table walks
   roty_map[roty:] + roty_map[:roty], which a negative slice turns
   the other way. */
static int mod4(int k)
{
    return ((k % 4) + 4) % 4;
}

/* The reference's `_create_cull_map` entry for one (roty, rotx)
   step pair, built the way the reference builds its table: the
   horizontal ring turns by roty first, then the vertical remap —
   whose first and third members are horizontal names, straight
   from the turned ring — turns by rotx. remap[i] is the bucket the
   i-th direction (McmeCullDir order) moves to; "none" maps to
   "none". */
static void cull_remap_build(int rotx, int roty, int remap[7])
{
    static const char *const ring[4] = { "north", "east", "south",
                                         "west" };
    const char *ring_rot[4];
    for (int i = 0; i < 4; ++i)
        ring_rot[i] = ring[mod4(roty + i)];

    const char *vring[4] = { ring_rot[0], "down", ring_rot[2], "up" };
    const char *vring_rot[4];
    for (int i = 0; i < 4; ++i)
        vring_rot[i] = vring[mod4(rotx + i)];

    static const char *const names[7] = { "down", "up", "north",
                                          "east", "south", "west",
                                          NULL };
    for (int i = 0; i < 7; ++i) {
        remap[i] = i; /* "none" maps to "none"; so does an unmoved name */
        if (!names[i]) continue;

        const char *turned = names[i];
        for (int j = 0; j < 4; ++j)
            if (strcmp(ring[j], turned) == 0) {
                turned = ring_rot[j];
                break;
            }
        for (int j = 0; j < 4; ++j)
            if (strcmp(vring[j], turned) == 0) {
                turned = vring_rot[j];
                break;
            }
        for (int j = 0; j < 7; ++j)
            if (names[j] && strcmp(names[j], turned) == 0) {
                remap[i] = j;
                break;
            }
    }
}

bool mcme_mesh_rotate(McmeMesh *m, int32_t rotx, int32_t roty)
{
    if (!m) {
        mcme_set_error("mesh rotate: null argument");
        return false;
    }

    /* The reference's guard is `if rotx or roty and (roty, rotx) in
       cull_remap_all:` — operator precedence gives rotx first
       refusal: a nonzero rotx skips the membership test, and the
       lookup then raises for a pair outside the -3..3 table; rotx
       zero with an out-of-range roty returns the mesh untouched;
       (0, 0) short-circuits. */
    bool in_table = rotx >= -3 && rotx <= 3 && roty >= -3 && roty <= 3;
    if (rotx == 0 && roty == 0) return true;
    if (!in_table) {
        if (rotx == 0) return true; /* the guard's false half: self */
        mcme_set_error("mesh rotate: the steps x %d y %d are outside "
                       "the cull map", (int)rotx, (int)roty);
        return false;
    }

    /* Each present bucket's vertices turn — the X pass then the Y
       pass, both about (0.5, 0.5, 0.5) and both negated — and every
       bucket's data moves to its remapped direction. Texture
       coordinates do not turn: uvlock's TODO in the reference is a
       permanent no-op. The remap is a permutation, so the buckets
       reshuffle under a scratch copy. */
    int remap[7];
    cull_remap_build((int)rotx, (int)roty, remap);

    McmeMeshBucket scratch[7];
    bool was[7];
    for (int i = 0; i < 7; ++i) {
        scratch[i] = m->buckets[i];
        was[i] = m->present[i];
        m->present[i] = false;
    }

    for (int dir = 0; dir < 7; ++dir) {
        if (!was[dir]) continue;
        int dst = remap[dir];
        m->buckets[dst] = scratch[dir]; /* the arrays move */
        m->present[dst] = true;

        McmeMeshBucket *b = &m->buckets[dst];
        for (size_t v = 0; v < b->vert_count; v += 4) {
            rotate_3d((double (*)[3])(&b->verts[v * 3]), -rotx * 90.0, 0, 0,
                      0.5, 0.5, 0.5, false);
            rotate_3d((double (*)[3])(&b->verts[v * 3]), 0, -roty * 90.0, 0,
                      0.5, 0.5, 0.5, false);
        }
    }
    return true;
}

/* qsort's order over the gathered texture list — the reference's
   numpy.unique sorts its strings the same lexicographic way. */
static int cmp_str_ptr(const void *a, const void *b)
{
    return strcmp(*(char *const *)a, *(char *const *)b);
}

/* The position of one texture path in a sorted mesh's list. Every
   merge input was gathered first, so a miss cannot happen; the
   bool is the belt for that unreachable. */
static bool texture_position(const McmeMesh *m, const char *path,
                             uint32_t *out)
{
    size_t lo = 0, hi = m->texture_count;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        int c = strcmp(m->textures[mid], path);
        if (c == 0) { *out = (uint32_t)mid; return true; }
        if (c < 0) lo = mid + 1;
        else       hi = mid;
    }
    return false;
}

bool mcme_mesh_merge(const McmeMesh *const *parts, size_t count,
                     McmeMesh *out)
{
    if (!out || (count > 0 && !parts)) {
        mcme_set_error("mesh merge: null argument");
        return false;
    }

    memset(out, 0, sizeof *out);
    out->transparency = MCME_TRANSPARENCY_PARTIAL;

    /* Every part's textures, gathered in part order and then
       sorted and deduplicated — the reference runs numpy.unique
       over the concatenation — so that each part's indexes can
       remap through the result by string. */
    size_t total = 0;
    for (size_t p = 0; p < count; ++p) total += parts[p]->texture_count;

    char **sorted = total ? malloc(total * sizeof *sorted) : NULL;
    if (total && !sorted) {
        mcme_set_error("mesh merge: out of memory");
        return false;
    }
    size_t nsorted = 0;
    for (size_t p = 0; p < count; ++p) {
        for (size_t i = 0; i < parts[p]->texture_count; ++i) {
            char *copy = ndup(parts[p]->textures[i],
                              strlen(parts[p]->textures[i]));
            if (!copy) {
                while (nsorted) free(sorted[--nsorted]);
                free(sorted);
                mcme_set_error("mesh merge: out of memory");
                return false;
            }
            sorted[nsorted++] = copy;
        }
    }
    /* No parts with textures leaves `sorted` NULL; qsort's base is
       declared never-NULL even at zero elements (UBSan caught it —
       the merge_empty probe). */
    if (nsorted > 1) qsort(sorted, nsorted, sizeof *sorted, cmp_str_ptr);
    size_t kept = 0;
    for (size_t i = 0; i < nsorted; ++i) {
        if (kept && strcmp(sorted[kept - 1], sorted[i]) == 0) {
            free(sorted[i]); /* the set-union drops a repeat */
            continue;
        }
        sorted[kept++] = sorted[i];
    }
    out->textures = sorted;
    out->texture_count = kept;

    /* The most transparent part wins: transparency is the minimum,
       Partial standing when there are none. */
    for (size_t p = 0; p < count; ++p)
        if (parts[p]->transparency < out->transparency)
            out->transparency = parts[p]->transparency;

    /* Per direction: the buckets concatenate, faces offset by the
       running vertex count and texture indexes by each part's own
       list, all under the reference's running counts. */
    for (int dir = 0; dir < 7; ++dir) {
        size_t vtotal = 0, ftotal = 0;
        for (size_t p = 0; p < count; ++p)
            if (parts[p]->present[dir]) {
                vtotal += parts[p]->buckets[dir].vert_count;
                ftotal += parts[p]->buckets[dir].face_count;
            }
        if (!ftotal) continue; /* the reference omits empty buckets */

        McmeMeshBucket *b = &out->buckets[dir];
        out->present[dir] = true;
        b->vert_count = vtotal;
        b->face_count = ftotal;
        b->verts          = malloc(vtotal * 3 * sizeof *b->verts);
        b->tverts         = malloc(vtotal * 2 * sizeof *b->tverts);
        b->tint_verts     = malloc(vtotal * 3 * sizeof *b->tint_verts);
        b->faces          = malloc(ftotal * 6 * sizeof *b->faces);
        b->texture_indexes = malloc(ftotal * 2 * sizeof *b->texture_indexes);
        if (!b->verts || !b->tverts || !b->tint_verts || !b->faces ||
            !b->texture_indexes)
            goto oom;

        size_t voff = 0, foff = 0;
        for (size_t p = 0; p < count; ++p) {
            const McmeMesh *m = parts[p];
            if (!m->present[dir]) continue;
            const McmeMeshBucket *sb = &m->buckets[dir];

            memcpy(b->verts + voff * 3, sb->verts,
                   sb->vert_count * 3 * sizeof *b->verts);
            memcpy(b->tverts + voff * 2, sb->tverts,
                   sb->vert_count * 2 * sizeof *b->tverts);
            memcpy(b->tint_verts + voff * 3, sb->tint_verts,
                   sb->vert_count * 3 * sizeof *b->tint_verts);

            for (size_t i = 0; i < sb->face_count; ++i)
                for (int t = 0; t < 6; ++t)
                    b->faces[(foff + i) * 6 + t] =
                        sb->faces[i * 6 + t] + (uint32_t)voff;

            for (size_t i = 0; i < sb->face_count; ++i)
                for (int t = 0; t < 2; ++t) {
                    uint32_t mapped;
                    if (!texture_position(out,
                                          m->textures[sb->texture_indexes
                                                      [i * 2 + t]],
                                          &mapped))
                        goto oom; /* unreachable; belt */
                    b->texture_indexes[(foff + i) * 2 + t] = mapped;
                }

            voff += sb->vert_count;
            foff += sb->face_count;
        }
    }
    return true;

oom:
    mcme_mesh_free(out);
    mcme_set_error("mesh merge: out of memory");
    return false;
}
