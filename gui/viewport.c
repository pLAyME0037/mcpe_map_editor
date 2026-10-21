/*
 * See viewport.h. Two halves, one thread rule: the staging half is
 * pure arithmetic (the selftest runs it headless), the GL half runs
 * only inside GtkGLArea's own callbacks, where the context is current
 * and the loop thread stands (D6 — no gl* call ever touches a worker).
 *
 * The camera is this frontend's own dress of the mesh's numbers (D49
 * extended to geometry): the mesh arrives in block units 0..1, and
 * the viewport centers it, turns it, and projects it — presentation,
 * not data. G8 puts it in the user's hand: the orbit (camera.h, pure
 * math the selftest pins headless) opens as the fixed three-quarter
 * still that stood before, and the gestures take it over — the
 * drag's deltas and the scroll's steps drive the one road the smoke
 * drives too. No face culling: the builder's winding is its own
 * business and the depth buffer settles visibility either way.
 */

#include "viewport.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <epoxy/gl.h>
#include <gtk/gtk.h>

#include "camera.h"
#include "mcme/resource.h"
#include "meshtask.h"

#define MCME_VIEWPORT_KEY "mcme.viewport"
#define MCME_CAMERA_KEY "mcme.camera"

/* ----------------------------------------------------------------------
 * Staging — the pure half
 * ---------------------------------------------------------------------- */

bool mcme_viewport_stage(const McmeMesh *mesh, const McmeAtlas *atlas,
                         const uint32_t *tex_slots, float **verts_out,
                         uint32_t **idx_out, size_t *nverts_out,
                         size_t *nidx_out)
{
    size_t nverts = 0, nidx = 0, vbase = 0, ibase = 0;
    float *verts;
    uint32_t *idx;

    if (mesh == NULL || verts_out == NULL || idx_out == NULL ||
        nverts_out == NULL || nidx_out == NULL)
        return false;

    for (int d = 0; d < 7; ++d) {
        if (!mesh->present[d]) continue;
        nverts += mesh->buckets[d].vert_count;
        nidx += mesh->buckets[d].face_count * 6;
    }

    verts = calloc(nverts ? nverts * 8 : 1, sizeof *verts);
    idx = calloc(nidx ? nidx : 1, sizeof *idx);
    if (verts == NULL || idx == NULL) {
        free(verts);
        free(idx);
        return false;
    }

    for (int d = 0; d < 7; ++d) {
        const McmeMeshBucket *b = &mesh->buckets[d];

        if (!mesh->present[d]) continue;

        for (size_t f = 0; f < b->face_count * 6; ++f) {
            /* A face index past the bucket's vertices would read the
               vertex stream out of bounds on the GPU; refuse the mesh
               at the door instead. */
            if (b->faces[f] >= b->vert_count) {
                free(verts);
                free(idx);
                return false;
            }
            idx[ibase + f] = (uint32_t)(vbase + b->faces[f]);
        }
        for (size_t f = 0; f < b->face_count; ++f) {
            /* The face's texture slot, then its four corners' uvs
               lerped into the page's bounds. The builder's vertices
               are face-owned (four per face), so the last write per
               vertex is the only write; a NULL page dresses every
               uv 0 — the tint-only fallback. */
            double u0 = 0, v0 = 0, u1 = 0, v1 = 0;

            if (atlas != NULL && tex_slots != NULL &&
                b->texture_indexes[f * 2] < mesh->texture_count) {
                uint32_t slot = tex_slots[b->texture_indexes[f * 2]];

                if ((size_t)slot * 4 + 3 < atlas->count * 4) {
                    u0 = atlas->bounds[slot * 4 + 0];
                    v0 = atlas->bounds[slot * 4 + 1];
                    u1 = atlas->bounds[slot * 4 + 2];
                    v1 = atlas->bounds[slot * 4 + 3];
                }
            }
            for (size_t k = 0; k < 6; ++k) {
                size_t v = b->faces[f * 6 + k];
                float *out = &verts[(vbase + v) * 8];
                double su = b->tverts[v * 2 + 0];
                double sv = b->tverts[v * 2 + 1];

                out[3] = (float)(u0 + su * (u1 - u0));
                out[4] = (float)(v0 + sv * (v1 - v0));
            }
        }
        for (size_t v = 0; v < b->vert_count; ++v) {
            float *out = &verts[(vbase + v) * 8];

            out[0] = (float)b->verts[v * 3 + 0];
            out[1] = (float)b->verts[v * 3 + 1];
            out[2] = (float)b->verts[v * 3 + 2];
            out[5] = (float)b->tint_verts[v * 3 + 0];
            out[6] = (float)b->tint_verts[v * 3 + 1];
            out[7] = (float)b->tint_verts[v * 3 + 2];
        }
        vbase += b->vert_count;
        ibase += b->face_count * 6;
    }

    *verts_out = verts;
    *idx_out = idx;
    *nverts_out = nverts;
    *nidx_out = nidx;
    return true;
}

/* ----------------------------------------------------------------------
 * The GL half
 * ---------------------------------------------------------------------- */

typedef struct {
    GLuint program, vao, vbo, ibo, tex;
    GLint a_pos, a_uv, a_tint, u_mvp, u_use_tex;
    float *verts;              /* staged, waiting for the next frame */
    uint32_t *idx;
    size_t nverts, nidx;
    uint8_t *page;             /* the page's pixels, awaiting upload  */
    int page_w, page_h;
    bool staged, page_staged, drew, textured, gl_failed;
} ViewportState;

/* GLES 3.0 — GTK's context here is OpenGL ES (the probe: ES 3.2).
   The texture samples the page; the tint multiplies it — the
   reference's per-face tint over the block's own image. */
static const char *k_vs =
    "#version 300 es\n"
    "precision highp float;\n"
    "in vec3 a_pos;\n"
    "in vec2 a_uv;\n"
    "in vec3 a_tint;\n"
    "uniform mat4 u_mvp;\n"
    "out vec2 v_uv;\n"
    "out vec3 v_tint;\n"
    "void main() {\n"
    "    v_uv = a_uv;\n"
    "    v_tint = a_tint;\n"
    "    gl_Position = u_mvp * vec4(a_pos, 1.0);\n"
    "}\n";

static const char *k_fs =
    "#version 300 es\n"
    "precision mediump float;\n"
    "in vec2 v_uv;\n"
    "in vec3 v_tint;\n"
    "uniform sampler2D u_tex;\n"
    "uniform bool u_use_tex;\n"
    "out vec4 o_color;\n"
    "void main() {\n"
    "    vec4 c = u_use_tex ? texture(u_tex, v_uv)\n"
    "                       : vec4(1.0);\n"
    "    o_color = vec4(c.rgb * v_tint, c.a);\n"
    "}\n";

static ViewportState *viewport_state(const GtkWidget *viewport)
{
    return g_object_get_data(G_OBJECT(viewport), MCME_VIEWPORT_KEY);
}

static GLuint compile(GLenum type, const char *src)
{
    GLuint s = glCreateShader(type);
    GLint ok = 0;

    glShaderSource(s, 1, &src, NULL);
    glCompileShader(s);
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[512];

        glGetShaderInfoLog(s, sizeof log, NULL, log);
        fprintf(stderr, "mcgui: shader: %s\n", log);
        glDeleteShader(s);
        return 0;
    }
    return s;
}

/* The camera's math — m4_mul, the projection, the orbit's
   composition — lives in camera.h's unit now: G8 gave the fixed
   camera's constants to the default state and put the hand on it. */

static void on_realize(GtkGLArea *area, gpointer user)
{
    ViewportState *v = user;
    GLuint vs, fs;
    GLint linked = 0;

    gtk_gl_area_make_current(area);
    if (gtk_gl_area_get_error(area) != NULL) {
        fprintf(stderr, "mcgui: gl context: %s\n",
                gtk_gl_area_get_error(area)->message);
        v->gl_failed = true;
        return;
    }
    vs = compile(GL_VERTEX_SHADER, k_vs);
    fs = compile(GL_FRAGMENT_SHADER, k_fs);
    if (vs == 0 || fs == 0) {
        if (vs != 0) glDeleteShader(vs);
        if (fs != 0) glDeleteShader(fs);
        v->gl_failed = true;
        return;
    }
    v->program = glCreateProgram();
    glAttachShader(v->program, vs);
    glAttachShader(v->program, fs);
    glLinkProgram(v->program);
    glDeleteShader(vs);
    glDeleteShader(fs);
    glGetProgramiv(v->program, GL_LINK_STATUS, &linked);
    if (!linked) {
        char log[512];

        glGetProgramInfoLog(v->program, sizeof log, NULL, log);
        fprintf(stderr, "mcgui: shader link: %s\n", log);
        v->gl_failed = true;
        return;
    }
    v->a_pos = glGetAttribLocation(v->program, "a_pos");
    v->a_uv = glGetAttribLocation(v->program, "a_uv");
    v->a_tint = glGetAttribLocation(v->program, "a_tint");
    v->u_mvp = glGetUniformLocation(v->program, "u_mvp");
    v->u_use_tex = glGetUniformLocation(v->program, "u_use_tex");
    glGenVertexArrays(1, &v->vao);
    glGenBuffers(1, &v->vbo);
    glGenBuffers(1, &v->ibo);
    glGenTextures(1, &v->tex);
}

static gboolean on_render(GtkGLArea *area, GdkGLContext *ctx, gpointer user)
{
    ViewportState *v = user;
    int w = gtk_widget_get_width(GTK_WIDGET(area));
    int h = gtk_widget_get_height(GTK_WIDGET(area));

    (void)ctx;
    if (v->gl_failed) return TRUE;

    if (v->staged) {
        glBindVertexArray(v->vao);
        glBindBuffer(GL_ARRAY_BUFFER, v->vbo);
        glBufferData(GL_ARRAY_BUFFER,
                     (GLsizeiptr)(v->nverts * 8 * sizeof(float)),
                     v->verts, GL_STATIC_DRAW);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, v->ibo);
        glBufferData(GL_ELEMENT_ARRAY_BUFFER,
                     (GLsizeiptr)(v->nidx * sizeof(uint32_t)),
                     v->idx, GL_STATIC_DRAW);
        glVertexAttribPointer((GLuint)v->a_pos, 3, GL_FLOAT, GL_FALSE,
                              8 * sizeof(float), (void *)0);
        glEnableVertexAttribArray((GLuint)v->a_pos);
        glVertexAttribPointer((GLuint)v->a_uv, 2, GL_FLOAT, GL_FALSE,
                              8 * sizeof(float),
                              (void *)(3 * sizeof(float)));
        glEnableVertexAttribArray((GLuint)v->a_uv);
        glVertexAttribPointer((GLuint)v->a_tint, 3, GL_FLOAT, GL_FALSE,
                              8 * sizeof(float),
                              (void *)(5 * sizeof(float)));
        glEnableVertexAttribArray((GLuint)v->a_tint);
        free(v->verts);
        free(v->idx);
        v->verts = NULL;
        v->idx = NULL;
        v->staged = false;
    }
    if (v->page_staged) {
        /* The page rides the same road: staged as pixels, uploaded
           here where the context is current. Block art wants nearest
           and no bleed across the page's cells. */
        glBindTexture(GL_TEXTURE_2D, v->tex);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, v->page_w, v->page_h, 0,
                     GL_RGBA, GL_UNSIGNED_BYTE, v->page);
        free(v->page);
        v->page = NULL;
        v->page_staged = false;
        v->textured = true;
    }

    glClearColor(0.13f, 0.15f, 0.18f, 1.0f);
    glEnable(GL_DEPTH_TEST);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    if (v->nidx > 0 && w > 0 && h > 0) {
        float mvp[16];
        McmeCamera *cam =
            g_object_get_data(G_OBJECT(area), MCME_CAMERA_KEY);

        /* The frame's matrix is the camera's own answer — the
           default state draws the still the window opened with. */
        mcme_camera_mvp(cam, (float)w / (float)h, mvp);
        glUseProgram(v->program);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, v->tex);
        glUniform1i(v->u_use_tex, v->textured ? 1 : 0);
        glUniformMatrix4fv(v->u_mvp, 1, GL_FALSE, mvp);
        glBindVertexArray(v->vao);
        glDrawElements(GL_TRIANGLES, (GLsizei)v->nidx, GL_UNSIGNED_INT,
                       (void *)0);
        v->drew = true;
    }
    {
        GLenum err = glGetError();

        if (err != GL_NO_ERROR) {
            fprintf(stderr, "mcgui: gl error 0x%x\n", err);
            v->gl_failed = true;
        }
    }
    return TRUE;
}

static void on_unrealize(GtkGLArea *area, gpointer user)
{
    ViewportState *v = user;

    gtk_gl_area_make_current(area);
    if (gtk_gl_area_get_error(area) == NULL && v->program != 0) {
        glDeleteTextures(1, &v->tex);
        glDeleteBuffers(1, &v->vbo);
        glDeleteBuffers(1, &v->ibo);
        glDeleteVertexArrays(1, &v->vao);
        glDeleteProgram(v->program);
    }
    v->program = 0;
}

static void state_free(gpointer data)
{
    ViewportState *v = data;

    free(v->verts);
    free(v->idx);
    free(v->page);
    g_free(v);
}

/* The gestures adapt input to the hand's one road: Gtk's drag
   offsets are cumulative from the gesture's start, so the deltas
   are taken here (the camera keeps the last seen offset) and the
   public drag applies them. */
static void on_drag_begin(GtkGestureDrag *g, gdouble x, gdouble y,
                          gpointer user)
{
    McmeCamera *cam = g_object_get_data(G_OBJECT(user),
                                        MCME_CAMERA_KEY);

    (void)g; (void)x; (void)y;
    if (cam == NULL) return;
    cam->last_x = 0.0;
    cam->last_y = 0.0;
}

static void on_drag_update(GtkGestureDrag *g, gdouble off_x,
                           gdouble off_y, gpointer user)
{
    McmeCamera *cam = g_object_get_data(G_OBJECT(user),
                                        MCME_CAMERA_KEY);
    double dx, dy;

    (void)g;
    if (cam == NULL) return;
    dx = off_x - cam->last_x;
    dy = off_y - cam->last_y;
    cam->last_x = off_x;
    cam->last_y = off_y;
    mcme_viewport_drag(GTK_WIDGET(user), dx, dy);
}

static gboolean on_scroll(GtkEventControllerScroll *c, gdouble dx,
                          gdouble dy, gpointer user)
{
    (void)c;

    /* One notch a step; a horizontal scroll rides the same wheel
       (trackpads push both). */
    mcme_viewport_zoom(GTK_WIDGET(user),
                       (int)lround(dy != 0.0 ? dy : dx));
    return TRUE;
}

GtkWidget *mcme_viewport_new(void)
{
    GtkWidget *area = gtk_gl_area_new();
    ViewportState *v = g_new0(ViewportState, 1);
    McmeCamera *cam = g_new(McmeCamera, 1);
    GtkGesture *drag;
    GtkEventController *scroll;

    gtk_gl_area_set_has_depth_buffer(GTK_GL_AREA(area), TRUE);
    g_object_set_data_full(G_OBJECT(area), MCME_VIEWPORT_KEY, v, state_free);
    mcme_camera_default(cam);
    g_object_set_data_full(G_OBJECT(area), MCME_CAMERA_KEY, cam, g_free);
    g_signal_connect(area, "realize", G_CALLBACK(on_realize), v);
    g_signal_connect(area, "render", G_CALLBACK(on_render), v);
    g_signal_connect(area, "unrealize", G_CALLBACK(on_unrealize), v);
    /* The hand: the drag turns, the scroll walks — both through the
       public road below, which queues the frame the new view
       needs. The camera rides beside the GL state, its own datum. */
    drag = gtk_gesture_drag_new();
    g_signal_connect(drag, "drag-begin", G_CALLBACK(on_drag_begin), area);
    g_signal_connect(drag, "drag-update", G_CALLBACK(on_drag_update), area);
    gtk_widget_add_controller(area, GTK_EVENT_CONTROLLER(drag));
    scroll = gtk_event_controller_scroll_new(
        GTK_EVENT_CONTROLLER_SCROLL_VERTICAL);
    g_signal_connect(scroll, "scroll", G_CALLBACK(on_scroll), area);
    gtk_widget_add_controller(area, GTK_EVENT_CONTROLLER(scroll));
    return area;
}

void mcme_viewport_set_mesh(GtkWidget *viewport, McmeMeshResult *result)
{
    ViewportState *v = viewport_state(viewport);
    float *verts = NULL;
    uint32_t *idx = NULL;
    size_t nverts = 0, nidx = 0;

    if (result == NULL) return;

    if (result->ok &&
        mcme_viewport_stage(&result->mesh,
                            result->has_atlas ? &result->atlas : NULL,
                            result->tex_slots,
                            &verts, &idx, &nverts, &nidx)) {
        free(v->verts);
        free(v->idx);
        v->verts = verts;
        v->idx = idx;
        v->nverts = nverts;
        v->nidx = nidx;
        v->staged = true;
        if (result->has_atlas) {
            /* The page's pixels ride on: the result frees its atlas,
               so the buffer becomes ours before the free. */
            free(v->page);
            v->page = result->atlas.page.rgba;
            result->atlas.page.rgba = NULL;
            v->page_w = result->atlas.page.width;
            v->page_h = result->atlas.page.height;
            v->page_staged = v->page != NULL;
        }
    }
    mcme_meshresult_free(result); /* staged as our own floats now */
    gtk_gl_area_queue_render(GTK_GL_AREA(viewport));
}

bool mcme_viewport_drew(const GtkWidget *viewport)
{
    const ViewportState *v = viewport_state(viewport);

    return v != NULL && v->drew;
}

bool mcme_viewport_gl_failed(const GtkWidget *viewport)
{
    const ViewportState *v = viewport_state(viewport);

    return v == NULL || v->gl_failed;
}

bool mcme_viewport_textured(const GtkWidget *viewport)
{
    const ViewportState *v = viewport_state(viewport);

    return v != NULL && v->textured;
}

/* The hand — one road for the gestures and the smoke alike: mutate
   the orbit, ask for the frame the new view needs. */
void mcme_viewport_drag(GtkWidget *viewport, double dx, double dy)
{
    McmeCamera *cam;

    if (viewport == NULL) return;
    cam = g_object_get_data(G_OBJECT(viewport), MCME_CAMERA_KEY);
    if (cam == NULL) return;
    mcme_camera_drag(cam, dx, dy);
    gtk_gl_area_queue_render(GTK_GL_AREA(viewport));
}

void mcme_viewport_zoom(GtkWidget *viewport, int steps)
{
    McmeCamera *cam;

    if (viewport == NULL) return;
    cam = g_object_get_data(G_OBJECT(viewport), MCME_CAMERA_KEY);
    if (cam == NULL) return;
    mcme_camera_zoom(cam, steps);
    gtk_gl_area_queue_render(GTK_GL_AREA(viewport));
}

bool mcme_viewport_camera(const GtkWidget *viewport, float *yaw_out,
                          float *pitch_out, float *dist_out)
{
    const McmeCamera *cam =
        viewport == NULL ? NULL
                         : g_object_get_data(G_OBJECT(viewport),
                                             MCME_CAMERA_KEY);

    if (cam == NULL) return false;
    if (yaw_out != NULL) *yaw_out = cam->yaw;
    if (pitch_out != NULL) *pitch_out = cam->pitch;
    if (dist_out != NULL) *dist_out = cam->dist;
    return true;
}
