/*
 * The camera in the user's hand (G8). Three numbers over the
 * centered block — yaw turns it, pitch tilts it, distance walks it
 * back — the presentation-not-data dress D49 always named (the
 * reference has no camera; the window does). The defaults reproduce
 * the fixed three-quarter still that stood before: the picture a
 * pack opens to is unchanged, and the drag takes it over from
 * there.
 *
 * Pure arithmetic, no GTK and no GL: the selftest pins the orbit
 * beside the staging half, headless. `last_x`/`last_y` are the
 * gesture adapter's bookkeeping alone (Gtk's drag offsets are
 * cumulative from the gesture's start; the deltas are taken here).
 */
#ifndef MCME_GUI_CAMERA_H
#define MCME_GUI_CAMERA_H

typedef struct {
    float  yaw;          /* radians around y — the turn               */
    float  pitch;        /* radians around x, clamped — the tilt      */
    float  dist;         /* how far back one stands — the translate   */
    double last_x;       /* the gesture's last seen offset (x)        */
    double last_y;       /* ... and (y)                               */
} McmeCamera;

/* The still that stood before it: yaw 0.6, pitch 0.42, dist 2.6 —
   the old fixed camera's own constants, now the default state. */
void mcme_camera_default(McmeCamera *cam);

/* One drag's deltas, in pixels: yaw grows with dx, pitch with dy —
   the tilt clamped to ±1.4 rad (~80°), past which the view
   inverts, which is confusion, not capability. */
void mcme_camera_drag(McmeCamera *cam, double dx, double dy);

/* One scroll's steps: steps > 0 walks in (dist *= 0.88^steps),
   clamped to [1.2, 8.0] — the block never swallowed by the eye,
   never walked off to a star. */
void mcme_camera_zoom(McmeCamera *cam, int steps);

/* project * tilt * turn * center-back — the frame's matrix, aspect
   true. NULL answers the default camera's matrix (the total-
   function house rule). */
void mcme_camera_mvp(const McmeCamera *cam, float aspect, float m[16]);

#endif
