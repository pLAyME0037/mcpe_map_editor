/* See camera.h. The matrix math is the viewport's own m4_mul and
   projection, moved here with the numbers they dress: the old fixed
   camera's constants are now the default state's fields, and the
   per-frame matrix is the same composition it always was —
   project * tilt(pitch) * turn(yaw) * center-back(dist). */

#include "camera.h"

#include <math.h>
#include <string.h>

#define MCME_DRAG_SLOPE 0.006f /* radians per pixel        */
#define MCME_PITCH_MAX  1.4f   /* ~80° — no inversion      */
#define MCME_ZOOM_BASE  0.88f  /* distance per step in     */
#define MCME_DIST_MIN   1.2f
#define MCME_DIST_MAX   8.0f

void mcme_camera_default(McmeCamera *cam)
{
    if (cam == NULL) return;
    cam->yaw = 0.6f;
    cam->pitch = 0.42f;
    cam->dist = 2.6f;
    cam->last_x = 0.0;
    cam->last_y = 0.0;
}

void mcme_camera_drag(McmeCamera *cam, double dx, double dy)
{
    if (cam == NULL) return;
    cam->yaw += (float)dx * MCME_DRAG_SLOPE;
    cam->pitch += (float)dy * MCME_DRAG_SLOPE;
    if (cam->pitch > MCME_PITCH_MAX) cam->pitch = MCME_PITCH_MAX;
    if (cam->pitch < -MCME_PITCH_MAX) cam->pitch = -MCME_PITCH_MAX;
}

void mcme_camera_zoom(McmeCamera *cam, int steps)
{
    float step = 1.0f;
    int n = steps < 0 ? -steps : steps;

    if (cam == NULL) return;
    /* Past the clamps' reach the walk is done either way; the cap
       also keeps the multiplication from under/overflowing (the
       clamps catch the result, exact). */
    if (n > 1024) n = 1024;
    for (int i = 0; i < n; ++i)
        step *= steps > 0 ? MCME_ZOOM_BASE : (1.0f / MCME_ZOOM_BASE);
    cam->dist *= step;
    if (cam->dist < MCME_DIST_MIN) cam->dist = MCME_DIST_MIN;
    if (cam->dist > MCME_DIST_MAX) cam->dist = MCME_DIST_MAX;
}

/* out = a * b, column-major (GL's), out may alias a or b. */
static void m4_mul(float out[16], const float a[16], const float b[16])
{
    float r[16];

    for (int c = 0; c < 4; ++c)
        for (int rr = 0; rr < 4; ++rr) {
            r[c * 4 + rr] = 0.0f;
            for (int k = 0; k < 4; ++k)
                r[c * 4 + rr] += a[k * 4 + rr] * b[c * 4 + k];
        }
    memcpy(out, r, sizeof r);
}

void mcme_camera_mvp(const McmeCamera *cam, float aspect, float m[16])
{
    McmeCamera here;
    float p[16] = {0}, rx[16] = {0}, ry[16] = {0}, t[16] = {0};
    float tmp[16];
    const float fovy = 0.9f;              /* ~52 degrees */
    const float near = 0.1f, far = 10.1f;
    const float f = 1.0f / tanf(fovy * 0.5f);

    if (cam == NULL) {
        mcme_camera_default(&here);
        cam = &here;
    }

    p[0] = f / aspect;
    p[5] = f;
    p[10] = (far + near) / (near - far);
    p[11] = -1.0f;
    p[14] = (2.0f * far * near) / (near - far);

    rx[0] = 1.0f;
    rx[5] = cosf(cam->pitch); rx[6] = sinf(cam->pitch);
    rx[9] = -sinf(cam->pitch); rx[10] = cosf(cam->pitch);
    rx[15] = 1.0f;

    ry[0] = cosf(cam->yaw); ry[2] = -sinf(cam->yaw);
    ry[5] = 1.0f;
    ry[8] = sinf(cam->yaw); ry[10] = cosf(cam->yaw);
    ry[15] = 1.0f;

    t[0] = t[5] = t[10] = t[15] = 1.0f;
    t[12] = -0.5f; t[13] = -0.5f; t[14] = -cam->dist;

    m4_mul(tmp, rx, ry);
    m4_mul(m, p, tmp);
    m4_mul(m, m, t);
}
