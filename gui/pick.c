/* See pick.h. The matrix inverse is the adjugate in GL's own
   layout; the ray is Möller–Trumbore's test over the stage's
   stride; the cell is the geometry's own — a shared coordinate
   for the plane, the winding for its side, the floor for the
   rest. Nothing here allocates, so the selftest's leak check
   stands over it too. */
#include "pick.h"

#include <math.h>

/* The adjugate: out = inv(m), column-major both. det == 0 answers
   false — a singular input has no inverse to give. */
bool mcme_mat4_invert(const float m[16], float out[16])
{
    float inv[16];
    float det;

    if (m == NULL || out == NULL) return false;
    inv[0] = m[5] * m[10] * m[15] - m[5] * m[11] * m[14] -
             m[9] * m[6] * m[15] + m[9] * m[7] * m[14] +
             m[13] * m[6] * m[11] - m[13] * m[7] * m[10];
    inv[4] = -m[4] * m[10] * m[15] + m[4] * m[11] * m[14] +
             m[8] * m[6] * m[15] - m[8] * m[7] * m[14] -
             m[12] * m[6] * m[11] + m[12] * m[7] * m[10];
    inv[8] = m[4] * m[9] * m[15] - m[4] * m[11] * m[13] -
             m[8] * m[5] * m[15] + m[8] * m[7] * m[13] +
             m[12] * m[5] * m[11] - m[12] * m[7] * m[9];
    inv[12] = -m[4] * m[9] * m[14] + m[4] * m[10] * m[13] +
              m[8] * m[5] * m[14] - m[8] * m[6] * m[13] -
              m[12] * m[5] * m[10] + m[12] * m[6] * m[9];
    inv[1] = -m[1] * m[10] * m[15] + m[1] * m[11] * m[14] +
             m[9] * m[2] * m[15] - m[9] * m[3] * m[14] -
             m[13] * m[2] * m[11] + m[13] * m[3] * m[10];
    inv[5] = m[0] * m[10] * m[15] - m[0] * m[11] * m[14] -
             m[8] * m[2] * m[15] + m[8] * m[3] * m[14] +
             m[12] * m[2] * m[11] - m[12] * m[3] * m[10];
    inv[9] = -m[0] * m[9] * m[15] + m[0] * m[11] * m[13] +
             m[8] * m[1] * m[15] - m[8] * m[3] * m[13] -
             m[12] * m[1] * m[11] + m[12] * m[3] * m[9];
    inv[13] = m[0] * m[9] * m[14] - m[0] * m[10] * m[13] -
              m[8] * m[1] * m[14] + m[8] * m[2] * m[13] +
              m[12] * m[1] * m[10] - m[12] * m[2] * m[9];
    inv[2] = m[1] * m[6] * m[15] - m[1] * m[7] * m[14] -
             m[5] * m[2] * m[15] + m[5] * m[3] * m[14] +
             m[13] * m[2] * m[7] - m[13] * m[3] * m[6];
    inv[6] = -m[0] * m[6] * m[15] + m[0] * m[7] * m[14] +
             m[4] * m[2] * m[15] - m[4] * m[3] * m[14] -
             m[12] * m[2] * m[7] + m[12] * m[3] * m[6];
    inv[10] = m[0] * m[5] * m[15] - m[0] * m[7] * m[13] -
              m[4] * m[1] * m[15] + m[4] * m[3] * m[13] +
              m[12] * m[1] * m[7] - m[12] * m[3] * m[5];
    inv[14] = -m[0] * m[5] * m[14] + m[0] * m[6] * m[13] +
              m[4] * m[1] * m[14] - m[4] * m[2] * m[13] -
              m[12] * m[1] * m[6] + m[12] * m[2] * m[5];
    inv[3] = -m[1] * m[6] * m[11] + m[1] * m[7] * m[10] +
             m[5] * m[2] * m[11] - m[5] * m[3] * m[10] -
             m[9] * m[2] * m[7] + m[9] * m[3] * m[6];
    inv[7] = m[0] * m[6] * m[11] - m[0] * m[7] * m[10] -
             m[4] * m[2] * m[11] + m[4] * m[3] * m[10] +
             m[8] * m[2] * m[7] - m[8] * m[3] * m[6];
    inv[11] = -m[0] * m[5] * m[11] + m[0] * m[7] * m[9] +
              m[4] * m[1] * m[11] - m[4] * m[3] * m[9] -
              m[8] * m[1] * m[7] + m[8] * m[3] * m[5];
    inv[15] = m[0] * m[5] * m[10] - m[0] * m[6] * m[9] -
              m[4] * m[1] * m[10] + m[4] * m[2] * m[9] +
              m[8] * m[1] * m[6] - m[8] * m[2] * m[5];

    det = m[0] * inv[0] + m[1] * inv[4] + m[2] * inv[8] +
          m[3] * inv[12];
    if (det == 0.0f) return false;
    det = 1.0f / det;
    for (int i = 0; i < 16; ++i) out[i] = inv[i] * det;
    return true;
}

/* world = (m · (ndc_x, ndc_y, ndc_z, 1)) / w — one unprojection. */
static bool unproject(const float inv[16], double nx, double ny,
                      double nz, float out[3])
{
    float x, y, z, w;

    x = (float)nx * inv[0] + (float)ny * inv[4] + (float)nz * inv[8] +
        inv[12];
    y = (float)nx * inv[1] + (float)ny * inv[5] + (float)nz * inv[9] +
        inv[13];
    z = (float)nx * inv[2] + (float)ny * inv[6] + (float)nz * inv[10] +
        inv[14];
    w = (float)nx * inv[3] + (float)ny * inv[7] + (float)nz * inv[11] +
        inv[15];
    if (w == 0.0f) return false;
    out[0] = x / w;
    out[1] = y / w;
    out[2] = z / w;
    return true;
}

bool mcme_pick_ray(const float mvp[16], double px, double py,
                   int width, int height,
                   float orig[3], float dir[3])
{
    float inv[16];
    float nearp[3], farp[3];
    double nx, ny, dx, dy, dz, len;

    if (mvp == NULL || orig == NULL || dir == NULL) return false;
    if (width <= 0 || height <= 0) return false;
    if (!mcme_mat4_invert(mvp, inv)) return false;

    /* The pixel's own ndc — the render's width/height, upside the
       way the screen's y runs. The depth ends are two points on
       one sight-line: any pair spans it, -1 and +1 are the box
       the frame draws in. */
    nx = 2.0 * px / (double)width - 1.0;
    ny = 1.0 - 2.0 * py / (double)height;
    if (!unproject(inv, nx, ny, -1.0, nearp) ||
        !unproject(inv, nx, ny, 1.0, farp))
        return false;

    dx = (double)farp[0] - nearp[0];
    dy = (double)farp[1] - nearp[1];
    dz = (double)farp[2] - nearp[2];
    len = sqrt(dx * dx + dy * dy + dz * dz);
    if (len == 0.0) return false;
    orig[0] = nearp[0];
    orig[1] = nearp[1];
    orig[2] = nearp[2];
    dir[0] = (float)(dx / len);
    dir[1] = (float)(dy / len);
    dir[2] = (float)(dz / len);
    return true;
}

/* One triangle of the stage's stream, Möller–Trumbore: false when
   the ray misses this one; else the forward parameter t. */
static bool tri_hit(const float orig[3], const float dir[3],
                    const float *v0, const float *v1, const float *v2,
                    float *t_out)
{
    float e1[3], e2[3], pv[3], tv[3], qv[3];
    float det, inv_det, u, v, t;
    const float det_eps = 1e-9f;
    const float t_eps = 1e-7f;

    e1[0] = v1[0] - v0[0];
    e1[1] = v1[1] - v0[1];
    e1[2] = v1[2] - v0[2];
    e2[0] = v2[0] - v0[0];
    e2[1] = v2[1] - v0[1];
    e2[2] = v2[2] - v0[2];
    pv[0] = dir[1] * e2[2] - dir[2] * e2[1];
    pv[1] = dir[2] * e2[0] - dir[0] * e2[2];
    pv[2] = dir[0] * e2[1] - dir[1] * e2[0];
    det = e1[0] * pv[0] + e1[1] * pv[1] + e1[2] * pv[2];
    if (fabsf(det) < det_eps) return false;
    inv_det = 1.0f / det;
    tv[0] = orig[0] - v0[0];
    tv[1] = orig[1] - v0[1];
    tv[2] = orig[2] - v0[2];
    u = (tv[0] * pv[0] + tv[1] * pv[1] + tv[2] * pv[2]) * inv_det;
    if (u < 0.0f || u > 1.0f) return false;
    qv[0] = tv[1] * e1[2] - tv[2] * e1[1];
    qv[1] = tv[2] * e1[0] - tv[0] * e1[2];
    qv[2] = tv[0] * e1[1] - tv[1] * e1[0];
    v = (dir[0] * qv[0] + dir[1] * qv[1] + dir[2] * qv[2]) * inv_det;
    if (v < 0.0f || u + v > 1.0f) return false;
    t = (e2[0] * qv[0] + e2[1] * qv[1] + e2[2] * qv[2]) * inv_det;
    if (t <= t_eps) return false;
    *t_out = t;
    return true;
}

/* The cell a struck face stands on (pick.h's rule), clamped to the
   frame the mesh was built in. */
static void face_cell(const float *v0, const float *v1, const float *v2,
                      const float hit[3], int edge, long cell[3])
{
    float span[3];
    int axis = 0;

    for (int a = 0; a < 3; ++a) {
        float lo = v0[a], hi = v0[a];

        if (v1[a] < lo) lo = v1[a];
        if (v2[a] < lo) lo = v2[a];
        if (v1[a] > hi) hi = v1[a];
        if (v2[a] > hi) hi = v2[a];
        span[a] = hi - lo;
        if (span[a] < span[axis]) axis = a;
    }

    if (span[axis] == 0.0f) {
        /* A shared coordinate: the face's plane. The winding — the
           reference's cube tables face outward, mesh.c — puts the
           solid on one side of it: outward + answers ceil(P)-1,
           outward - answers floor(P). Dyadic positions, so both
           roundings are exact. */
        float e1[3], e2[3], nx, ny, nz, p;
        int outward;

        e1[0] = v1[0] - v0[0];
        e1[1] = v1[1] - v0[1];
        e1[2] = v1[2] - v0[2];
        e2[0] = v2[0] - v0[0];
        e2[1] = v2[1] - v0[1];
        e2[2] = v2[2] - v0[2];
        nx = e1[1] * e2[2] - e1[2] * e2[1];
        ny = e1[2] * e2[0] - e1[0] * e2[2];
        nz = e1[0] * e2[1] - e1[1] * e2[0];
        {
            float n = (axis == 0) ? nx : (axis == 1) ? ny : nz;

            outward = n >= 0.0 ? 1 : -1;
        }
        p = (double)v0[axis];
        cell[axis] = outward > 0 ? (long)ceil(p) - 1 : (long)floor(p);
        for (int a = 0; a < 3; ++a)
            if (a != axis) cell[a] = (long)floor((double)hit[a]);
    } else {
        /* No shared coordinate — a rotated model's face: the cell
           containing the point itself (block rotations keep a box's
           corners inside its cell). */
        for (int a = 0; a < 3; ++a)
            cell[a] = (long)floor((double)hit[a]);
    }
    for (int a = 0; a < 3; ++a) {
        if (cell[a] < 0) cell[a] = 0;
        if (cell[a] >= edge) cell[a] = edge - 1;
    }
}

bool mcme_pick_cell(const float *verts, size_t nverts,
                    const uint32_t *idx, size_t nidx,
                    const float orig[3], const float dir[3],
                    int edge, long cell[3])
{
    float best_t = 0.0f;
    const float *bv0 = NULL, *bv1 = NULL, *bv2 = NULL;

    if (verts == NULL || idx == NULL || orig == NULL ||
        dir == NULL || cell == NULL)
        return false;
    if (edge <= 0) return false;
    if (nidx % 6 != 0) return false; /* the stage writes faces whole */
    for (size_t i = 0; i < nidx; ++i)
        if (idx[i] >= nverts) return false;

    for (size_t i = 0; i < nidx; i += 6) {
        const float *v0 = verts + (size_t)idx[i] * 8;
        const float *v1 = verts + (size_t)idx[i + 1] * 8;
        const float *v2 = verts + (size_t)idx[i + 2] * 8;
        float t;

        if (tri_hit(orig, dir, v0, v1, v2, &t) &&
            (bv0 == NULL || t < best_t)) {
            best_t = t;
            bv0 = v0;
            bv1 = v1;
            bv2 = v2;
        }
        /* The quad's second triangle: the next three indices the
           stage wrote (tri_face's 0,2,3 — mesh.c) read as a triple
           of their own. A shape like (i, i+2, i+3) would read the
           face's 4th index, which tri_face repeats from its 1st —
           a degenerate corner, silently skipped. */
        v0 = verts + (size_t)idx[i + 3] * 8;
        v1 = verts + (size_t)idx[i + 4] * 8;
        v2 = verts + (size_t)idx[i + 5] * 8;
        if (tri_hit(orig, dir, v0, v1, v2, &t) &&
            (bv0 == NULL || t < best_t)) {
            best_t = t;
            bv0 = v0;
            bv1 = v1;
            bv2 = v2;
        }
    }
    if (bv0 == NULL) return false;
    {
        float hit[3];

        hit[0] = orig[0] + dir[0] * best_t;
        hit[1] = orig[1] + dir[1] * best_t;
        hit[2] = orig[2] + dir[2] * best_t;
        face_cell(bv0, bv1, bv2, hit, edge, cell);
    }
    return true;
}
