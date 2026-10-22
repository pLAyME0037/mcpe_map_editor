/*
 * The pick (G14): the point on the picture answered as the cell
 * under it. Pure arithmetic over the stage's own stream — the same
 * interleaved layout the window uploads (stride 8: position, uv,
 * tint; only the positions are read) — and the frame's own matrix:
 * mcme_camera_mvp with the widget's width/height, the exact pair
 * viewport.c renders with. No GTK, no GL, no camera of its own:
 * the selftest pins it headless, and the smoke answers a pick
 * without a hand.
 */
#ifndef MCME_GUI_PICK_H
#define MCME_GUI_PICK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* The inverse of a column-major 4×4 (GL's layout, camera.c's
 * writing): out = m⁻¹. False on a singular input — the total-
 * function house rule; a real frustum's matrix never is. */
bool mcme_mat4_invert(const float m[16], float out[16]);

/* The sight-line through one widget pixel: the pixel to ndc (the
 * widget's own width/height — the render's pair), unprojected at
 * both ends of the depth box and reduced to origin (the near
 * point) and a unit direction. False on NULL, a non-positive
 * widget, or a singular matrix. */
bool mcme_pick_ray(const float mvp[16], double px, double py,
                   int width, int height,
                   float orig[3], float dir[3]);

/* The cell under the point: the nearest forward triangle over the
 * staged stream, then the cell that face stands on. The plane axis
 * is the coordinate the three hit vertices share exactly (block
 * geometry is dyadic and exact — mesh.c says so), and the face's
 * own winding — the reference's cube tables face outward, so
 * cross(v1-v0, v2-v0) reads outward — puts the cell on the solid's
 * side of the plane: outward +axis answers ceil(P)-1, outward
 * -axis answers floor(P). The other axes floor where the ray
 * struck, and every axis clamps to [0, edge) — the frame the mesh
 * was built in (16 for a sub-chunk, 1 for the one block). A face
 * with no shared coordinate (a rotated model's) answers the hit
 * point's own cell. False when nothing answers: NULL, a ragged or
 * out-of-range stream, no hit. */
bool mcme_pick_cell(const float *verts, size_t nverts,
                    const uint32_t *idx, size_t nidx,
                    const float orig[3], const float dir[3],
                    int edge, long cell[3]);

#endif /* MCME_GUI_PICK_H */
