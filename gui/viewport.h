/*
 * The viewport — D6's GL half. A GtkGLArea whose render callback is
 * the only place any gl* call happens: the worker builds the mesh as
 * data (meshtask), the wire delivers it to the loop thread, and the
 * visit stages it — plain CPU floats — for the next frame to upload
 * and draw.
 *
 * The staged geometry is the mesh's own numbers dressed for the GPU:
 * positions and per-vertex tint, interleaved; the seven cull buckets
 * merged in direction order with each face's indices made global.
 * The mesh's texture half rides later (the atlas is the CLI's, D34);
 * tint is what every mesh already carries.
 */
#ifndef MCME_GUI_VIEWPORT_H
#define MCME_GUI_VIEWPORT_H

#include <stdbool.h>
#include <stdint.h>

#include <gtk/gtk.h>

#include "mcme/resource.h" /* McmeMesh */
#include "meshtask.h"      /* McmeMeshResult */

GtkWidget *mcme_viewport_new(void);

/* Pure staging, no GL and no GTK: the mesh flattened into one
 * interleaved vertex stream (position then tint, three floats each)
 * and one index stream rebased onto it — bucket-local face indices
 * made stream-global. Both arrays are malloc'd; the caller frees
 * them. False on a NULL argument, out of memory, or a face pointing
 * past its bucket's vertices — a mesh no GPU should be handed. */
bool mcme_viewport_stage(const McmeMesh *mesh, float **verts_out,
                         uint32_t **idx_out, size_t *nverts_out,
                         size_t *nidx_out);

/* Loop thread only: takes ownership of the result, stages it, and
 * queues a frame. A refused result is freed and leaves the viewport
 * as it was. */
void mcme_viewport_set_mesh(GtkWidget *viewport, McmeMeshResult *result);

/* True once a frame carrying a mesh has been drawn; true when any GL
 * step went wrong (context, shader, upload, or draw). The smoke gate
 * reads both. */
bool mcme_viewport_drew(const GtkWidget *viewport);
bool mcme_viewport_gl_failed(const GtkWidget *viewport);

#endif
