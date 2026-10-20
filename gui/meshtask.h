/*
 * G3's worker: the viewport's first mesh. The worker opens the pack's
 * model and texture indices and calls mcme_mesh_build — the same
 * builder the CLI's `mesh`, `blockmesh`, and `chunk mesh` verbs all
 * end at (D32) — and hands the finished mesh through the wire. Data
 * crosses; no GL call ever happens off the main thread (D6).
 */
#ifndef MCME_GUI_MESHTASK_H
#define MCME_GUI_MESHTASK_H

#include <stdbool.h>

#include "mcme/resource.h" /* McmeMesh */
#include "wire.h"

typedef struct {
    bool ok;
    char *text;    /* the refusal when !ok */
    McmeMesh mesh; /* valid when ok; free with mcme_meshresult_free */
} McmeMeshResult;

typedef void (*McmeMeshDoneFn)(void *userdata, McmeMeshResult *result);

/* Worker: opens the pack's indices, meshes namespace:path, closes the
 * indices (the mesh owns its data), delivers on the loop thread. */
bool mcme_meshtask_start(McmeWire *wire, const char *pack_dir,
                         const char *ns, const char *path,
                         McmeMeshDoneFn done, void *userdata);

void mcme_meshresult_free(McmeMeshResult *result);

#endif
