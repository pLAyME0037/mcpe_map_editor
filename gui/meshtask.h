/*
 * G3's worker: the viewport's first mesh. The worker opens the pack's
 * model and texture indices and calls mcme_mesh_build — the same
 * builder the CLI's `mesh`, `blockmesh`, and `chunk mesh` verbs all
 * end at (D32) — and hands the finished mesh through the wire. Data
 * crosses; no GL call ever happens off the main thread (D6).
 *
 * G4 dresses it: the same worker packs the editor's page
 * (mcme_atlas_build over the index's whole table, D34 — the missing
 * image seeded first) and answers where each of the mesh's own
 * textures landed, so the loop thread can bake texture coordinates
 * without touching the index again.
 */
#ifndef MCME_GUI_MESHTASK_H
#define MCME_GUI_MESHTASK_H

#include <stdbool.h>
#include <stdint.h>

#include "mcme/resource.h" /* McmeMesh, McmeAtlas */
#include "wire.h"

typedef struct {
    bool ok;
    char *text;    /* the refusal when !ok */
    McmeMesh mesh; /* valid when ok; free with mcme_meshresult_free */
    /* The page: valid when has_atlas. A page that will not build is
       a tint-only viewport, not a refused mesh — the degradation is
       the answer, and the mesh still crosses. */
    bool      has_atlas;
    McmeAtlas atlas;
    uint32_t *tex_slots; /* mesh texture i -> atlas slot, has_atlas  */
} McmeMeshResult;

typedef void (*McmeMeshDoneFn)(void *userdata, McmeMeshResult *result);

/* Worker: opens the pack's indices, meshes namespace:path, packs the
 * texture page, closes the indices (the mesh and the page own their
 * data), delivers on the loop thread. */
bool mcme_meshtask_start(McmeWire *wire, const char *pack_dir,
                         const char *ns, const char *path,
                         McmeMeshDoneFn done, void *userdata);

void mcme_meshresult_free(McmeMeshResult *result);

#endif
