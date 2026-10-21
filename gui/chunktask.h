/*
 * G9's worker: the world's own block through the wire. The worker
 * probes the world, reads the block spec the store spells at the
 * coordinates (mcme_world_block_spec — the CLI's chunk get road,
 * moved to the core whole), parses it the way the verbs parse it
 * (mcme_block_parse_spec), and meshes it through the blockmesh
 * layer's total answer with the pack's indices — then the meshtask's
 * own page dresses it (shared atlas, G9). The spec text rides the
 * result: the smoke's evidence and the loop thread's label, the
 * world's own answer in its own words.
 *
 * Since G10 the block arrives CULLED to its world: the worker also
 * reads the six neighbours standing on the builder's CULL_MAP
 * cells (mcme_cull_offset — a side outside the stored world is
 * air, the verbs' rule) and meshes each for the one transparency
 * the clauses read, then drops the sides mcme_cull_drops rejects
 * (mcme_mesh_cull_dirs — the same predicate `mcme_atlas_mesh_dirs`
 * walks, D37's two clauses over transparency alone). The mesh
 * crossing carries exactly the faces the world would show: a
 * block buried in opaque neighbours crosses empty, and `faces`
 * counts both halves of the answer for the settle line to print.
 * A lone block in a picker is not in situ and stays uncalled — it
 * rides meshtask, never this road.
 *
 * Data crosses; no core call ever happens off the worker (the G2
 * discipline, kept).
 *
 * Since G11 the same road carries the WHOLE SUB-CHUNK the
 * coordinates stand in (--section): the worker composes the grid
 * the builder walks (mcme_world_subgrid — `chunk submesh`'s own
 * seven layers, moved to the core whole), meshes every DISTINCT
 * state the grid holds once (the verbs' prelude, D40), and walks
 * the sub-chunk's 4096 cells in the builder's own order — each
 * cell's mesh copied from its state, culled to its six real
 * neighbours, moved to the cell (mcme_mesh_translate) — merging
 * the survivors into one mesh at the sub-chunk's own origin.
 * `faces` and `faces_total` sum the cells' answers and `cells`
 * counts the cells whose faces stand. The spec rides the target
 * CELL's answer: the store's own words for the coordinates asked.
 */
#ifndef MCME_GUI_CHUNKTASK_H
#define MCME_GUI_CHUNKTASK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "meshtask.h" /* McmeMeshResult, McmeTextureIndex */

typedef struct {
    bool ok;
    char *text;  /* the refusal when !ok — the store's own words */
    char *spec;  /* the world's answer: "ns:name{states}" when ok */
    size_t faces;       /* the faces the world's sides keep      */
    size_t faces_total; /* the mesh's faces before the cull       */
    size_t cells;       /* the cells whose faces stand (the
                           section ask; the block ask's one)      */
    McmeMeshResult *mesh; /* owned; handed to the viewport, NULLed */
} McmeChunkResult;

typedef void (*McmeChunkDoneFn)(void *userdata, McmeChunkResult *result);

/* Worker: probe the world, read the spec at (x, y, z) in the
 * dimension (0 overworld, 1 nether, 2 end — the numbers the CLI
 * parses), mesh it with the pack, deliver on the loop thread. With
 * `section` the whole sub-chunk stands in instead of the one
 * block (G11). */
bool mcme_chunktask_start(McmeWire *wire, const char *world_dir,
                          const char *pack_dir, int32_t dimension,
                          long x, long y, long z, bool section,
                          McmeChunkDoneFn done, void *userdata);

void mcme_chunkresult_free(McmeChunkResult *result);

#endif
