#include "mcme/resource.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mcme/core.h"
#include "stb_image.h"
#include "stb_image_write.h"

/*
 * Images and the atlas — every texture a pack ships on one square
 * page, and where each one landed (D34, decisions.md#D34). The
 * reference is the editor's own textureatlas module (the model
 * reader has no atlas; the app's OpenGL layer packs), and mcme
 * mirrors it rule for rule:
 *
 *   - Frames load whole; textures sort by perimeter (2w + 2h) in
 *     non-increasing order, STABLY — equal perimeters keep the
 *     input order, and a pack of 16x16 textures is one long tie,
 *     so the input order decides the layout.
 *   - The page starts at max(max width, max height, the smallest
 *     power of two at least ceil(sqrt(total pixels))) and doubles
 *     until the packer takes everything.
 *   - The packer is a binary-split region tree: the first free
 *     region that holds an item takes it at its top-left, and the
 *     region splits into the strip below the item and the strip
 *     right of it.
 *   - Bounds are (x/size, y/size, (x+w)/size, (y + min(h, w)) /
 *     size) — the top edge using the SMALLER of the frame's sides,
 *     the module's own quirk, not an invention.
 *
 * The module supports a packing border; `create_atlas` uses the
 * zero default, and so does mcme.
 *
 * The page always carries the missing image first — the manager
 * seeds its table with it before the pack's files (D35) — and the
 * layer below maps mesh coordinates into the rectangles the page
 * answers.
 */

/* ========================================================================
 * Images
 * ======================================================================== */

bool mcme_image_load(const char *path, McmeImage *out)
{
    if (!path || !out) {
        mcme_set_error("image load: null argument");
        return false;
    }
    int w = 0, h = 0, comp = 0;
    unsigned char *px = stbi_load(path, &w, &h, &comp, 4);
    if (!px) {
        /* The reference's Pillow raise kills the whole atlas call;
           mcme refuses by name — a pack that will not load is not
           answerable either way (D34). */
        mcme_set_error("image load: '%s' will not load: %s", path,
                       stbi_failure_reason());
        return false;
    }
    out->width = w;
    out->height = h;
    out->rgba = px;
    return true;
}

void mcme_image_free(McmeImage *image)
{
    if (!image) return;
    stbi_image_free(image->rgba);
    image->rgba = NULL;
    image->width = image->height = 0;
}

bool mcme_image_write_png(const char *path, const McmeImage *image)
{
    if (!path || !image || !image->rgba) {
        mcme_set_error("image write: null argument");
        return false;
    }
    if (!stbi_write_png(path, image->width, image->height, 4,
                        image->rgba, image->width * 4)) {
        mcme_set_error("image write: cannot write '%s'", path);
        return false;
    }
    return true;
}

/* ========================================================================
 * The atlas
 * ======================================================================== */

/* One loaded frame and where the packer put it. `rgba` is borrowed
   from stb until the build frees it. */
typedef struct {
    const unsigned char *rgba;
    int x, y, w, h;
    double perimeter;
} AtlasFrame;

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

/* The missing image — the reader ships its own file, and the game's
   canonical missing texture is what is in it: a 16x16
   magenta-and-black checker, the 8x8 quadrants alternating by
   (x < 8) == (y < 8), magenta f800f8. The manager seeds its table
   with this image before the pack's files, so the app's page
   carries the rectangle a mesh that names a texture the pack lacks
   draws from; mcme spells the same pixels out (D35). The buffer is
   malloc'd so the frames' stbi free path releases it. */
static bool missing_image(McmeImage *out)
{
    uint8_t *rgba = malloc(16 * 16 * 4);
    if (!rgba) return false;
    for (int y = 0; y < 16; ++y) {
        for (int x = 0; x < 16; ++x) {
            uint8_t *px = rgba + ((size_t)y * 16 + (size_t)x) * 4;
            bool magenta = (x < 8) == (y < 8);
            px[0] = magenta ? 0xf8u : 0x00u;
            px[1] = 0x00u;
            px[2] = magenta ? 0xf8u : 0x00u;
            px[3] = 0xffu;
        }
    }
    out->width = out->height = 16;
    out->rgba = rgba;
    return true;
}

/* A region of the page; `below` and `right` exist once something
   is packed here, exactly as the reference splits on placement. */
typedef struct PackNode {
    int x, y, w, h;
    bool occupied;
    struct PackNode *below, *right;
} PackNode;

static PackNode *node_new(int x, int y, int w, int h)
{
    PackNode *n = malloc(sizeof *n);
    if (!n) return NULL;
    n->x = x;
    n->y = y;
    n->w = w;
    n->h = h;
    n->occupied = false;
    n->below = n->right = NULL;
    return n;
}

static void node_free(PackNode *node)
{
    if (!node) return;
    node_free(node->below);
    node_free(node->right);
    free(node);
}

/* The reference's PackRegion.pack: the first free region that holds
   the frame takes it at its top-left and splits; a full region
   tries below, then right. `oom` separates the heap's refusal from
   the region simply not fitting. */
static bool pack_fit(PackNode *node, AtlasFrame *f, bool *oom)
{
    if (!node->occupied) {
        if (f->w > node->w || f->h > node->h) return false;
        node->occupied = true;
        /* The reference sets the packable's x, y here (plus the
           border, which create_atlas leaves at zero). */
        f->x = node->x;
        f->y = node->y;
        /* sub1 is the strip below the item, the ITEM's width —
           the strip right of it covers the rest at full height;
           sub2 is the strip right, the region's full height. */
        node->below = node_new(node->x, node->y + f->h,
                               f->w, node->h - f->h);
        node->right = node_new(node->x + f->w, node->y,
                               node->w - f->w, node->h);
        if (!node->below || !node->right) {
            *oom = true;
            return false;
        }
        return true;
    }
    if (pack_fit(node->below, f, oom)) return true;
    if (*oom) return false;
    return pack_fit(node->right, f, oom);
}

/* The perimeter sort: non-increasing, ties keeping the input
   order — Python's sorted is stable, and qsort is not, so the tie
   break is the index itself. */
static const AtlasFrame *g_sort_frames;

static int cmp_perimeter(const void *a, const void *b)
{
    size_t ia = *(const size_t *)a, ib = *(const size_t *)b;
    if (g_sort_frames[ia].perimeter > g_sort_frames[ib].perimeter)
        return -1;
    if (g_sort_frames[ia].perimeter < g_sort_frames[ib].perimeter)
        return 1;
    return ia < ib ? -1 : ia > ib;
}

/* The page's first size: max(max width, max height, the smallest
   power of two at least ceil(sqrt(total pixels))). The empty set
   answers 2 — the reference's `bit_length` of (ceil(sqrt(0)) - 1),
   which is the bit length of -1. */
static int page_size(const AtlasFrame *frames, size_t n)
{
    long pixels = 0;
    int maxw = 0, maxh = 0;
    for (size_t i = 0; i < n; ++i) {
        pixels += (long)frames[i].w * frames[i].h;
        if (frames[i].w > maxw) maxw = frames[i].w;
        if (frames[i].h > maxh) maxh = frames[i].h;
    }
    if (n == 0) return 2;

    size_t ceil_sqrt = (size_t)ceil(sqrt((double)pixels));
    size_t pow2 = 1;
    while (pow2 < ceil_sqrt) pow2 <<= 1;
    int size = (int)pow2;
    if (maxw > size) size = maxw;
    if (maxh > size) size = maxh;
    return size;
}

bool mcme_atlas_build(const char *const *paths, size_t count,
                      McmeAtlas *out)
{
    if (!out || (count > 0 && !paths)) {
        mcme_set_error("atlas: null argument");
        return false;
    }
    memset(out, 0, sizeof *out);

    /* The missing image rides first — the manager seeds its table
       with it before the pack's files, so the app's page carries
       it too, and the bounds rule below always has a rectangle to
       answer with (D35). One more frame than the caller named. */
    size_t total = count + 1;
    AtlasFrame *frames = calloc(total, sizeof *frames);
    if (!frames) {
        mcme_set_error("atlas: out of memory");
        return false;
    }
    McmeImage missing;
    if (!missing_image(&missing)) {
        mcme_set_error("atlas: out of memory");
        free(frames);
        return false;
    }
    frames[0].rgba = missing.rgba;
    frames[0].w = missing.width;
    frames[0].h = missing.height;
    frames[0].perimeter = 2.0 * (missing.width + missing.height);

    /* Every frame loads whole, the reference's Frame doing the
       same; one that will not load refuses the whole atlas. */
    for (size_t i = 0; i < count; ++i) {
        McmeImage img;
        if (!mcme_image_load(paths[i], &img)) {
            for (size_t j = 0; j < total; ++j)
                stbi_image_free((void *)frames[j].rgba);
            free(frames);
            return false;
        }
        frames[i + 1].rgba = img.rgba;
        frames[i + 1].w = img.width;
        frames[i + 1].h = img.height;
        frames[i + 1].perimeter = 2.0 * (img.width + img.height);
    }

    /* Sort by perimeter, ties by input order. */
    size_t *order = malloc(total * sizeof *order);
    if (!order) {
        mcme_set_error("atlas: out of memory");
        goto fail_frames;
    }
    for (size_t i = 0; i < total; ++i) order[i] = i;
    if (total > 1) {
        g_sort_frames = frames;
        qsort(order, total, sizeof *order, cmp_perimeter);
        g_sort_frames = NULL;
    }

    /* The page starts at the reference's estimate and doubles
       until the packer takes everything. */
    int size = page_size(frames, total);
    PackNode *root = NULL;
    for (;;) {
        root = node_new(0, 0, size, size);
        if (!root) {
            mcme_set_error("atlas: out of memory");
            goto fail_order;
        }
        bool oom = false, all = true;
        for (size_t i = 0; i < total; ++i) {
            if (!pack_fit(root, &frames[order[i]], &oom)) {
                all = false;
                break;
            }
        }
        if (all) break; /* everything placed; x/y live on the frames */
        node_free(root);
        root = NULL;
        if (oom) {
            mcme_set_error("atlas: out of memory");
            goto fail_order;
        }
        size *= 2;
    }
    node_free(root);
    free(order);

    /* The page, and every frame blitted where it landed. */
    out->size = size;
    out->count = total;
    out->page.width = size;
    out->page.height = size;
    out->page.rgba = calloc((size_t)size * (size_t)size, 4);
    /* calloc: a failure part-way through filling leaves the rest
       NULL, which mcme_atlas_free skips rather than freeing. */
    out->names = calloc(total, sizeof *out->names);
    out->bounds = malloc(total * 4 * sizeof *out->bounds);
    if (!out->page.rgba || !out->names || !out->bounds) {
        mcme_set_error("atlas: out of memory");
        mcme_atlas_free(out);
        goto fail_frames;
    }

    for (size_t i = 0; i < total; ++i) {
        const AtlasFrame *f = &frames[i];
        for (int row = 0; row < f->h; ++row) {
            unsigned char *dst = out->page.rgba +
                ((size_t)(f->y + row) * (size_t)size + (size_t)f->x) * 4;
            memcpy(dst, f->rgba + (size_t)row * (size_t)f->w * 4,
                   (size_t)f->w * 4);
        }
        const char *name = i == 0 ? MCME_MISSING_TEXTURE : paths[i - 1];
        out->names[i] = ndup(name, strlen(name));
        if (!out->names[i]) {
            mcme_set_error("atlas: out of memory");
            mcme_atlas_free(out);
            goto fail_frames;
        }
        /* The module's bounds, quirk included: the top edge uses
           the smaller of the frame's sides. */
        out->bounds[i * 4 + 0] = f->x / (double)size;
        out->bounds[i * 4 + 1] = f->y / (double)size;
        out->bounds[i * 4 + 2] = (f->x + f->w) / (double)size;
        out->bounds[i * 4 + 3] =
            (f->y + (f->h < f->w ? f->h : f->w)) / (double)size;
    }

    for (size_t i = 0; i < total; ++i)
        stbi_image_free((void *)frames[i].rgba);
    free(frames);
    return true;

fail_order:
    free(order);
fail_frames:
    for (size_t i = 0; i < total; ++i)
        stbi_image_free((void *)frames[i].rgba);
    free(frames);
    return false;
}

void mcme_atlas_free(McmeAtlas *atlas)
{
    if (!atlas) return;
    for (size_t i = 0; i < atlas->count; ++i) free(atlas->names[i]);
    free(atlas->names);
    free(atlas->bounds);
    free(atlas->page.rgba);
    memset(atlas, 0, sizeof *atlas);
}

/* ========================================================================
 * Texture coordinates in atlas space (D35)
 * ======================================================================== */

bool mcme_atlas_bounds(const McmeAtlas *atlas, const char *name,
                       double out[4])
{
    if (!atlas || !name || !out) {
        mcme_set_error("atlas bounds: null argument");
        return false;
    }
    size_t found = (size_t)-1;
    for (size_t i = 0; i < atlas->count; ++i) {
        if (strcmp(atlas->names[i], name) == 0) {
            found = i;
            break;
        }
    }
    if (found == (size_t)-1) {
        /* The reference's texture_bounds, rule for rule: a texture
           the page does not hold draws from the missing image's
           own rectangle (D35). */
        for (size_t i = 0; i < atlas->count; ++i) {
            if (strcmp(atlas->names[i], MCME_MISSING_TEXTURE) == 0) {
                found = i;
                break;
            }
        }
        if (found == (size_t)-1) {
            mcme_set_error("atlas bounds: '%s' is not on the page and "
                           "this atlas carries no " MCME_MISSING_TEXTURE,
                           name);
            return false;
        }
    }
    memcpy(out, &atlas->bounds[found * 4], 4 * sizeof *out);
    return true;
}

/* The builder's per-direction brightness (chunk_builder_cy.pyx: a
   step of 0.15 per side — down three steps, the east/west pair
   two, the north/south pair one, up and the cullless none bucket
   none) — indexed by McmeCullDir, DOWN first. */
static const double k_brightness[7] = {
    0.55, 1.0, 0.85, 0.70, 0.85, 0.70, 1.0
};

/* The band shade at a vertex's world height: the builder's
   triangular wave — (y / 32) % 2, folded back over 1 where it
   passes the top, scaled to 0.9 + 0.2 of it — so the light runs
   0.9 at the trough to 1.1 at the peak and back every 64 blocks.
   The reference reads the wave with numpy's modulo, whose
   remainder carries the DIVISOR's sign; C's fmod keeps the
   dividend's, so the period is added back for a vertex below zero
   (D36). */
static double band_shade(double y)
{
    double t = fmod(y / 32.0, 2.0);
    if (t < 0.0) t += 2.0;
    if (t > 1.0) t = 2.0 - t;
    return 0.9 + 0.2 * t;
}

bool mcme_atlas_mesh(const McmeAtlas *atlas, const McmeMesh *mesh,
                     long block_y, McmeAtlasMesh *out)
{
    if (!atlas || !mesh || !out) {
        mcme_set_error("atlas mesh: null argument");
        return false;
    }
    memset(out, 0, sizeof *out);

    for (int i = 0; i < 7; ++i) {
        if (!mesh->present[i]) continue;
        const McmeMeshBucket *b = &mesh->buckets[i];
        out->present[i] = true;

        /* One row per vertex of the face stream — six per face,
           the two triangles the builder draws. A row's rectangle
           is its triangle's: the builder writes one rectangle per
           THREE rows, stepping the bucket's texture index (two per
           face, one per triangle) alongside (D35). */
        size_t rows = b->face_count * 6;
        McmeAtlasBucket *m = &out->buckets[i];
        m->row_count = rows;
        if (rows == 0) continue;
        m->bounds = calloc(rows * 4, sizeof *m->bounds);
        m->auvs = calloc(rows * 2, sizeof *m->auvs);
        m->tints = calloc(rows * 3, sizeof *m->tints);
        if (!m->bounds || !m->auvs || !m->tints) {
            mcme_set_error("atlas mesh: out of memory");
            mcme_atlas_mesh_free(out);
            return false;
        }
        for (size_t f = 0; f < b->face_count; ++f) {
            for (size_t k = 0; k < 6; ++k) {
                size_t row = f * 6 + k;
                uint32_t tex = b->texture_indexes[f * 2 + k / 3];
                uint32_t vert = b->faces[row];
                if (tex >= mesh->texture_count ||
                    vert >= b->vert_count) {
                    mcme_set_error("atlas mesh: the mesh's own indexes "
                                   "are out of range");
                    mcme_atlas_mesh_free(out);
                    return false;
                }
                double rect[4];
                if (!mcme_atlas_bounds(atlas, mesh->textures[tex],
                                       rect)) {
                    mcme_atlas_mesh_free(out);
                    return false;
                }
                memcpy(&m->bounds[row * 4], rect, sizeof rect);

                /* The fragment stage's mapping, per axis:
                   mix(b0, b1, mod(t, 1.0)) — GLSL's mod is
                   floor-based, so a coordinate below zero wraps up
                   rather than clamping (D35). */
                const double *t = &b->tverts[vert * 2];
                double *uv = &m->auvs[row * 2];
                for (int axis = 0; axis < 2; ++axis) {
                    double wrapped = t[axis] - floor(t[axis]);
                    uv[axis] = rect[axis] +
                        (rect[axis + 2] - rect[axis]) * wrapped;
                }

                /* The tint column: the mesh's own tint folded the
                   builder's way, in its order — the direction's
                   brightness, then the band shade at the vertex's
                   world height, the block's y under it (D36). The
                   reference rounds each fold through its float32
                   storage; mcme's doubles hold the exact product. */
                double shade = band_shade(
                    (double)block_y + b->verts[vert * 3 + 1]);
                double *tint = &m->tints[row * 3];
                for (int c = 0; c < 3; ++c)
                    tint[c] = b->tint_verts[vert * 3 + c] *
                        k_brightness[i] * shade;
            }
        }
    }
    return true;
}

void mcme_atlas_mesh_free(McmeAtlasMesh *m)
{
    if (!m) return;
    for (int i = 0; i < 7; ++i) {
        free(m->buckets[i].bounds);
        free(m->buckets[i].auvs);
        free(m->buckets[i].tints);
    }
    memset(m, 0, sizeof *m);
}
