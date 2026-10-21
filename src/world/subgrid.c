/*
 * The sub-chunk grid the builder walks — the CLI's sub_bedrock_layers,
 * sub_java_layers and the padded compose moved into the core whole
 * (G11): the same reads, the same edge rule, the same answers, asked
 * now by `chunk submesh` and by the GUI's section view alike. The
 * statuses are the block reader's own, so the verbs keep their exit
 * codes by mapping them, and the "chunk: ..." messages carry the
 * reader's context the verbs have always printed. See world.h.
 */

#include "mcme/world.h"

#include <stdio.h>
#include <string.h>

#include "mcme/bedrock_chunk.h"
#include "mcme/bedrock_write.h" /* the sub-chunk key, the spec text */
#include "mcme/java_chunk.h"
#include "mcme/leveldb.h"
#include "mcme/region.h"
#include "mcme/resource.h"      /* the CULL_MAP's one table (G10) */

static const char kSubAir[] = "minecraft:air";

/* The spec standing at a layer's local position, air where the
   layer answers none. The layer's order is Bedrock's own sub-chunk
   order — y fastest, then z, then x — which both loaders answer:
   the store's layers arrive in it, and the Java section's grid is
   transcribed into it as it is adopted. */
static const char *sub_layer_spec(const McmeSubchunkLayer *l,
                                  int lx, int ly, int lz)
{
    if (l->indices == NULL) return kSubAir;
    size_t idx = l->indices[(size_t)lx * 256 + (size_t)lz * 16
                            + (size_t)ly];
    return idx < l->spec_count ? l->specs[idx] : kSubAir;
}

/* The composed grid: the sub-chunk's cells at [1..16], the
   neighbours' edge layers as the padding every cull read off an
   edge lands on. The corner padding is never read — only the six
   faces' cells are — and stays air. */
static void sub_grid_compose(McmeSubgrid *grid)
{
    const McmeSubchunkLayer *layers = grid->layers;
    for (int i = 0; i < MCME_SUB_CELLS; ++i) grid->cells[i] = kSubAir;
    for (int lx = 0; lx < MCME_SUB_EDGE; ++lx) {
        for (int ly = 0; ly < MCME_SUB_EDGE; ++ly) {
            for (int lz = 0; lz < MCME_SUB_EDGE; ++lz) {
                grid->cells[(lx + 1) * MCME_SUB_WIDTH * MCME_SUB_WIDTH
                            + (ly + 1) * MCME_SUB_WIDTH + (lz + 1)]
                    = sub_layer_spec(
                        &layers[MCME_SUB_TARGET], lx, ly, lz);
                grid->cells[(lx + 1) * MCME_SUB_WIDTH * MCME_SUB_WIDTH
                            + lz + 1]
                    = sub_layer_spec(&layers[1], lx,
                                     MCME_SUB_EDGE - 1, lz);
                grid->cells[(lx + 1) * MCME_SUB_WIDTH * MCME_SUB_WIDTH
                            + (MCME_SUB_EDGE + 1) * MCME_SUB_WIDTH
                            + lz + 1]
                    = sub_layer_spec(&layers[2], lx, 0, lz);
                grid->cells[(lx + 1) * MCME_SUB_WIDTH * MCME_SUB_WIDTH
                            + (ly + 1) * MCME_SUB_WIDTH]
                    = sub_layer_spec(&layers[3], lx, ly,
                                     MCME_SUB_EDGE - 1);
                grid->cells[(lx + 1) * MCME_SUB_WIDTH * MCME_SUB_WIDTH
                            + (ly + 1) * MCME_SUB_WIDTH
                            + MCME_SUB_EDGE + 1]
                    = sub_layer_spec(&layers[5], lx, ly, 0);
                grid->cells[(MCME_SUB_EDGE + 1) * MCME_SUB_WIDTH
                            * MCME_SUB_WIDTH
                            + (ly + 1) * MCME_SUB_WIDTH + lz + 1]
                    = sub_layer_spec(&layers[4], MCME_SUB_EDGE - 1,
                                     ly, lz);
                grid->cells[(ly + 1) * MCME_SUB_WIDTH + lz + 1]
                    = sub_layer_spec(&layers[6], 0, ly, lz);
            }
        }
    }
}

/* The seven layers from the Bedrock store: one open, one key and
   one decode each, the decodes in the grid's arena. A key the
   store does not hold is the air layer — the world's edge, except
   under the target, which is what `chunk get` refuses with. */
static McmeWorldBlockStatus sub_bedrock_layers(
    const McmeWorldInfo *info, int32_t dimension,
    int sx, int sy, int sz, McmeSubchunkLayer out[MCME_SUB_LAYERS],
    McmeArena *arena)
{
    McmeLevelDb *db = NULL;
    if (!mcme_leveldb_open(&db, info->db_path)) {
        return MCME_WORLDBLOCK_IO;  /* the store's own error stands */
    }
    McmeWorldBlockStatus st = MCME_WORLDBLOCK_OK;
    for (int s = 0; s < MCME_SUB_LAYERS
                    && st == MCME_WORLDBLOCK_OK; ++s) {
        /* The seven cells: the CULL_MAP's one table (G10) — s 0 is
           the sub-chunk itself. */
        int dx = 0, dy = 0, dz = 0;
        if (s != 0) mcme_cull_offset(s - 1, &dx, &dy, &dz);
        int cx = sx + dx;
        int cy = sy + dy;
        int cz = sz + dz;
        out[s].specs = NULL;
        out[s].spec_count = 0;
        out[s].indices = NULL;

        uint8_t key[14];
        size_t key_len = 0;
        if (!mcme_bedrock_subchunk_key(cx * 16, cy * 16, cz * 16,
                                       dimension, key, sizeof key,
                                       &key_len)) {
            mcme_set_error("chunk: the sub-chunk (%d, %d, "
                           "%d) is past the Y keys can name",
                           cx, cy, cz);
            st = MCME_WORLDBLOCK_USAGE;
            break;
        }
        if (!mcme_leveldb_has(db, key, key_len)) {
            if (s == MCME_SUB_TARGET) {
                mcme_leveldb_close(db);
                mcme_set_error("chunk: the sub-chunk of "
                               "block (%ld, %ld, %ld) is not stored "
                               "in this world", sx * 16L, sy * 16L,
                               sz * 16L);
                return MCME_WORLDBLOCK_NOT_FOUND;
            }
            continue;               /* the world's edge: air */
        }
        McmeBuffer payload;
        mcme_buffer_init(&payload);
        bool deleted = false;
        bool got = mcme_leveldb_get(db, key, key_len, &payload,
                                    &deleted);
        if (!got) {
            mcme_buffer_free(&payload);
            st = MCME_WORLDBLOCK_IO; /* the store's own error stands */
            break;
        }
        if (deleted) {
            mcme_buffer_free(&payload);
            if (s == MCME_SUB_TARGET) {
                mcme_leveldb_close(db);
                mcme_set_error("chunk: the sub-chunk of "
                               "block (%ld, %ld, %ld) is deleted in "
                               "this world", sx * 16L, sy * 16L,
                               sz * 16L);
                return MCME_WORLDBLOCK_NOT_FOUND;
            }
            continue;               /* the world's edge: air */
        }
        McmeBedrockSubchunk sc;
        if (!mcme_bedrock_subchunk_decode(&sc, arena, payload.data,
                                          payload.len)) {
            char why[256];
            snprintf(why, sizeof why, "%s", mcme_last_error());
            mcme_set_error("chunk: the sub-chunk (%d, %d, "
                           "%d) does not decode: %s", cx, cy, cz,
                           why);
            st = MCME_WORLDBLOCK_BAD_FORMAT;
        } else if (sc.layer_count > 0) {
            const McmeBedrockLayer *layer = &sc.layers[0];
            const char **specs = mcme_arena_alloc(
                arena, layer->palette_len * sizeof *specs);
            uint16_t *indices = mcme_arena_alloc(
                arena, MCME_JAVA_SECTION_BLOCKS * sizeof *indices);
            if (specs == NULL || indices == NULL) {
                mcme_set_error("chunk: out of memory");
                st = MCME_WORLDBLOCK_INTERNAL;
            } else {
                for (size_t i = 0; i < layer->palette_len
                         && st == MCME_WORLDBLOCK_OK; ++i) {
                    McmeBuffer buf;
                    mcme_buffer_init(&buf);
                    if (!mcme_bedrock_block_spec(
                            layer->palette[i].root, &buf)) {
                        st = MCME_WORLDBLOCK_BAD_FORMAT;
                    } else {
                        char *copy =
                            mcme_arena_alloc(arena, buf.len + 1);
                        if (copy == NULL) {
                            mcme_set_error("chunk: out of memory");
                            st = MCME_WORLDBLOCK_INTERNAL;
                        } else {
                            memcpy(copy, buf.data, buf.len);
                            copy[buf.len] = '\0';
                            specs[i] = copy;
                        }
                    }
                    mcme_buffer_free(&buf);
                }
                if (st == MCME_WORLDBLOCK_OK) {
                    memcpy(indices, layer->indices,
                           MCME_JAVA_SECTION_BLOCKS
                           * sizeof *indices);
                    out[s].specs = specs;
                    out[s].spec_count = layer->palette_len;
                    out[s].indices = indices;
                }
            }
        }
        mcme_buffer_free(&payload);
    }
    mcme_leveldb_close(db);
    return st;
}

/* The seven layers from the region files: each chunk's tag read
   once — the target's chunk carries three of the grid's sections,
   the ones beside it one each — and each section through the
   batched read. A chunk the region file does not have is the air
   layer, the world's edge, except under the target; a chunk that
   does not DECODE is the read's own refusal. The arenas the docs
   parse into are the grid's to keep. */
static McmeWorldBlockStatus sub_java_layers(
    const McmeWorldInfo *info, int32_t dimension,
    int sx, int sy, int sz, McmeSubchunkLayer out[MCME_SUB_LAYERS],
    McmeSubgrid *grid)
{
    /* The chunks loaded so far: the sides share them three ways
       down the middle column, and each is parsed once. */
    McmeNbtDoc docs[MCME_SUB_LAYERS];
    McmeArena *doc_arenas[MCME_SUB_LAYERS];
    int doc_x[MCME_SUB_LAYERS], doc_z[MCME_SUB_LAYERS];
    int loaded = 0;

    McmeWorldBlockStatus st = MCME_WORLDBLOCK_OK;
    for (int s = 0; s < MCME_SUB_LAYERS
                    && st == MCME_WORLDBLOCK_OK; ++s) {
        /* The seven cells: the CULL_MAP's one table (G10) — s 0 is
           the sub-chunk itself. */
        int dx = 0, dy = 0, dz = 0;
        if (s != 0) mcme_cull_offset(s - 1, &dx, &dy, &dz);
        int cx = sx + dx;
        int cy = sy + dy;
        int cz = sz + dz;
        out[s].specs = NULL;
        out[s].spec_count = 0;
        out[s].indices = NULL;

        int found = -1;
        for (int i = 0; i < loaded; ++i)
            if (doc_x[i] == cx && doc_z[i] == cz) { found = i; break; }
        if (found < 0) {
            McmeArena *arena = NULL;
            McmeNbtDoc doc;
            char file[MCME_REGION_PATH_MAX];
            McmeWorldBlockStatus cst = mcme_java_chunk_doc(
                info->path, dimension, cx * 16L, cy * 16L,
                cz * 16L, &arena, &doc, file, sizeof file);
            if (cst == MCME_WORLDBLOCK_NOT_FOUND) {
                if (s == MCME_SUB_TARGET) return cst;
                continue;           /* the world's edge: air */
            }
            if (cst != MCME_WORLDBLOCK_OK) return cst;
            found = loaded;
            docs[found] = doc;
            doc_arenas[found] = arena;
            doc_x[found] = cx;
            doc_z[found] = cz;
            grid->arenas[grid->arena_count++] = arena;
            loaded++;
        }

        McmeJavaSectionGrid section;
        if (!mcme_java_section_grid(&section, doc_arenas[found],
                                    docs[found].root, cy)) {
            char why[256];
            snprintf(why, sizeof why, "%s", mcme_last_error());
            mcme_set_error("chunk: the section (%d, %d, %d) does "
                           "not decode: %s", cx, cy, cz, why);
            return MCME_WORLDBLOCK_BAD_FORMAT;
        }
        if (section.stored) {
            /* The grid is the section's own order — x fastest,
               then z, then y — and the layer is Bedrock's sub-chunk
               order, y fastest, then z, then x. The transcription
               happens once, here, so every meshing read keeps one
               formula whichever format the layer came from. */
            uint16_t *xzy = mcme_arena_alloc(
                doc_arenas[found],
                MCME_JAVA_SECTION_BLOCKS * sizeof *xzy);
            if (!xzy) {
                mcme_set_error("chunk: out of memory");
                return MCME_WORLDBLOCK_INTERNAL;
            }
            for (int ly = 0; ly < 16; ++ly)
                for (int lz = 0; lz < 16; ++lz)
                    for (int lx = 0; lx < 16; ++lx)
                        xzy[(size_t)lx * 256 + (size_t)lz * 16
                            + (size_t)ly]
                            = section.indices[(size_t)lx
                                  + (size_t)lz * 16
                                    + (size_t)ly * 256];
            out[s].specs = section.specs;
            out[s].spec_count = section.spec_count;
            out[s].indices = xzy;
        }
    }
    return st;
}

McmeWorldBlockStatus mcme_world_subgrid(const McmeWorldInfo *info,
                                        int32_t dimension,
                                        long x, long y, long z,
                                        McmeSubgrid *grid)
{
    if (info == NULL || grid == NULL) {
        mcme_set_error("chunk: null argument");
        return MCME_WORLDBLOCK_INTERNAL;
    }
    memset(grid, 0, sizeof *grid);

    int sx = (int)(x >> 4), sy = (int)(y >> 4), sz = (int)(z >> 4);

    McmeWorldBlockStatus st;
    if (info->kind == MCME_WORLD_JAVA) {
        st = sub_java_layers(info, dimension, sx, sy, sz,
                             grid->layers, grid);
    } else if (info->kind == MCME_WORLD_BEDROCK) {
        McmeArena *arena = mcme_arena_new();
        if (arena == NULL) {
            mcme_set_error("chunk: out of memory");
            return MCME_WORLDBLOCK_INTERNAL;
        }
        grid->arenas[grid->arena_count++] = arena;
        st = sub_bedrock_layers(info, dimension, sx, sy, sz,
                                grid->layers, arena);
    } else {
        mcme_set_error("chunk: '%s' is not a Bedrock or Java world",
                       info->path);
        st = MCME_WORLDBLOCK_USAGE;
    }

    if (st != MCME_WORLDBLOCK_OK) {
        mcme_subgrid_free(grid);
        return st;
    }
    sub_grid_compose(grid);
    return MCME_WORLDBLOCK_OK;
}

void mcme_subgrid_free(McmeSubgrid *grid)
{
    if (grid == NULL) return;
    for (int i = 0; i < grid->arena_count; ++i)
        mcme_arena_free(grid->arenas[i]);
    memset(grid, 0, sizeof *grid);
}

const char *mcme_subgrid_at(const McmeSubgrid *grid, int lx, int ly,
                            int lz)
{
    if (grid == NULL) return kSubAir;
    if (lx < -1 || lx > MCME_SUB_EDGE || ly < -1 || ly > MCME_SUB_EDGE
            || lz < -1 || lz > MCME_SUB_EDGE)
        return kSubAir;
    return grid->cells[(size_t)(lx + 1) * MCME_SUB_WIDTH
                            * MCME_SUB_WIDTH
                       + (size_t)(ly + 1) * MCME_SUB_WIDTH
                       + (size_t)(lz + 1)];
}
