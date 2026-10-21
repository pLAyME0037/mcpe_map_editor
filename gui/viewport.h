/*
 * The viewport — D6's GL half. A GtkGLArea whose render callback is
 * the only place any gl* call happens: the worker builds the mesh as
 * data (meshtask), the wire delivers it to the loop thread, and the
 * visit stages it — plain CPU floats — for the next frame to upload
 * and draw.
 *
 * The staged geometry is the mesh's own numbers dressed for the GPU:
 * positions, texture coordinates and per-vertex tint, interleaved;
 * the seven cull buckets merged in direction order with each face's
 * indices made global. The texture coordinates come from G4's seam:
 * the mesh's own uvs lerped into the page's bounds for the face's
 * slot (D34's atlas, built worker-side). The page uploads on the
 * frame after it arrives — GL stays inside the callbacks, D6.
 */
#ifndef MCME_GUI_VIEWPORT_H
#define MCME_GUI_VIEWPORT_H

#include <stdbool.h>
#include <stdint.h>

#include <gtk/gtk.h>

#include "mcme/resource.h" /* McmeMesh, McmeAtlas */
#include "meshtask.h"      /* McmeMeshResult */

GtkWidget *mcme_viewport_new(void);

/* Pure staging, no GL and no GTK: the mesh flattened into one
 * interleaved vertex stream (position three floats, uv two, tint
 * three) and one index stream rebased onto it — bucket-local face
 * indices made stream-global. Each face's uvs lerp the mesh's own
 * into the page's bounds for its texture slot; a NULL page (or
 * slot table) dresses every vertex with uv 0 — the tint-only
 * fallback. Both arrays are malloc'd; the caller frees them. False
 * on a NULL mesh argument, out of memory, or a face pointing past
 * its bucket's vertices — a mesh no GPU should be handed. */
bool mcme_viewport_stage(const McmeMesh *mesh, const McmeAtlas *atlas,
                         const uint32_t *tex_slots, float **verts_out,
                         uint32_t **idx_out, size_t *nverts_out,
                         size_t *nidx_out);

/* Loop thread only: takes ownership of the result, stages it (page
 * and all), and queues a frame. A refused result is freed and leaves
 * the viewport as it was. */
void mcme_viewport_set_mesh(GtkWidget *viewport, McmeMeshResult *result);

/* G8: the camera in the user's hand. Drag (deltas, pixels) and zoom
 * (steps, > 0 walks in) mutate the orbit and queue the frame the
 * new view needs — the gesture handlers' road and the smoke's, one
 * road. The current orbit reads back for the smoke's evidence line;
 * false when the widget carries no camera. */
void mcme_viewport_drag(GtkWidget *viewport, double dx, double dy);
void mcme_viewport_zoom(GtkWidget *viewport, int steps);
bool mcme_viewport_camera(const GtkWidget *viewport, float *yaw_out,
                          float *pitch_out, float *dist_out);

/* True once a frame carrying a mesh has been drawn; true when any GL
 * step went wrong (context, shader, upload, or draw); true once the
 * page's texture has landed on the GPU. The smoke gates read all
 * three. */
bool mcme_viewport_drew(const GtkWidget *viewport);
bool mcme_viewport_gl_failed(const GtkWidget *viewport);
bool mcme_viewport_textured(const GtkWidget *viewport);

#endif
