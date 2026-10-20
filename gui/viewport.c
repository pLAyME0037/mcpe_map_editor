/*
 * See viewport.h. Two halves, one thread rule: the staging half is
 * pure arithmetic (the selftest runs it headless), the GL half runs
 * only inside GtkGLArea's own callbacks, where the context is current
 * and the loop thread stands (D6 — no gl* call ever touches a worker).
 *
 * The camera is this frontend's own dress of the mesh's numbers (D49
 * extended to geometry): the mesh arrives in block units 0..1, and
 * the viewport centers it, turns it a fixed three-quarter tilt, and
 * projects it — presentation constants, not data. No face culling:
 * the builder's winding is its own business and the depth buffer
 * settles visibility either way.
 */

#include "viewport.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <epoxy/gl.h>
#include <gtk/gtk.h>

#include "mcme/resource.h"
#include "meshtask.h"

#define MCME_VIEWPORT_KEY "mcme.viewport"

/* ----------------------------------------------------------------------
 * Staging — the pure half
 * ---------------------------------------------------------------------- */

bool mcme_viewport_stage(const McmeMesh *mesh, float **verts_out,
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

    verts = calloc(nverts ? nverts * 6 : 1, sizeof *verts);
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
        for (size_t v = 0; v < b->vert_count; ++v) {
            verts[(vbase + v) * 6 + 0] = (float)b->verts[v * 3 + 0];
            verts[(vbase + v) * 6 + 1] = (float)b->verts[v * 3 + 1];
            verts[(vbase + v) * 6 + 2] = (float)b->verts[v * 3 + 2];
            verts[(vbase + v) * 6 + 3] = (float)b->tint_verts[v * 3 + 0];
            verts[(vbase + v) * 6 + 4] = (float)b->tint_verts[v * 3 + 1];
            verts[(vbase + v) * 6 + 5] = (float)b->tint_verts[v * 3 + 2];
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
    GLuint program, vao, vbo, ibo;
    GLint a_pos, a_tint, u_mvp;
    float *verts;              /* staged, waiting for the next frame */
    uint32_t *idx;
    size_t nverts, nidx;
    bool staged, drew, gl_failed;
} ViewportState;

/* GLES 3.0 — GTK's context here is OpenGL ES (the probe: ES 3.2). */
static const char *k_vs =
    "#version 300 es\n"
    "precision highp float;\n"
    "in vec3 a_pos;\n"
    "in vec3 a_tint;\n"
    "uniform mat4 u_mvp;\n"
    "out vec3 v_tint;\n"
    "void main() {\n"
    "    v_tint = a_tint;\n"
    "    gl_Position = u_mvp * vec4(a_pos, 1.0);\n"
    "}\n";

static const char *k_fs =
    "#version 300 es\n"
    "precision mediump float;\n"
    "in vec3 v_tint;\n"
    "out vec4 o_color;\n"
    "void main() {\n"
    "    o_color = vec4(v_tint, 1.0);\n"
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

/* The fixed camera: project * tilt * turn * center — the mesh's
   0..1 block units dressed into view (presentation, not data). */
static void build_mvp(float m[16], float aspect)
{
    float p[16] = {0}, rx[16] = {0}, ry[16] = {0}, t[16] = {0};
    float tmp[16];
    const float fovy = 0.9f;              /* ~52 degrees */
    const float tilt = 0.42f;             /* around x     */
    const float turn = 0.6f;              /* around y     */
    const float near = 0.1f, far = 10.1f;
    const float f = 1.0f / tanf(fovy * 0.5f);

    p[0] = f / aspect;
    p[5] = f;
    p[10] = (far + near) / (near - far);
    p[11] = -1.0f;
    p[14] = (2.0f * far * near) / (near - far);

    rx[0] = 1.0f;
    rx[5] = cosf(tilt); rx[6] = sinf(tilt);
    rx[9] = -sinf(tilt); rx[10] = cosf(tilt);
    rx[15] = 1.0f;

    ry[0] = cosf(turn); ry[2] = -sinf(turn);
    ry[5] = 1.0f;
    ry[8] = sinf(turn); ry[10] = cosf(turn);
    ry[15] = 1.0f;

    t[0] = t[5] = t[10] = t[15] = 1.0f;
    t[12] = -0.5f; t[13] = -0.5f; t[14] = -2.6f;

    m4_mul(tmp, rx, ry);
    m4_mul(m, p, tmp);
    m4_mul(m, m, t);
}

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
    v->a_tint = glGetAttribLocation(v->program, "a_tint");
    v->u_mvp = glGetUniformLocation(v->program, "u_mvp");
    glGenVertexArrays(1, &v->vao);
    glGenBuffers(1, &v->vbo);
    glGenBuffers(1, &v->ibo);
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
                     (GLsizeiptr)(v->nverts * 6 * sizeof(float)),
                     v->verts, GL_STATIC_DRAW);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, v->ibo);
        glBufferData(GL_ELEMENT_ARRAY_BUFFER,
                     (GLsizeiptr)(v->nidx * sizeof(uint32_t)),
                     v->idx, GL_STATIC_DRAW);
        glVertexAttribPointer((GLuint)v->a_pos, 3, GL_FLOAT, GL_FALSE,
                              6 * sizeof(float), (void *)0);
        glEnableVertexAttribArray((GLuint)v->a_pos);
        glVertexAttribPointer((GLuint)v->a_tint, 3, GL_FLOAT, GL_FALSE,
                              6 * sizeof(float),
                              (void *)(3 * sizeof(float)));
        glEnableVertexAttribArray((GLuint)v->a_tint);
        free(v->verts);
        free(v->idx);
        v->verts = NULL;
        v->idx = NULL;
        v->staged = false;
    }

    glClearColor(0.13f, 0.15f, 0.18f, 1.0f);
    glEnable(GL_DEPTH_TEST);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    if (v->nidx > 0 && w > 0 && h > 0) {
        float mvp[16];

        build_mvp(mvp, (float)w / (float)h);
        glUseProgram(v->program);
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
    g_free(v);
}

GtkWidget *mcme_viewport_new(void)
{
    GtkWidget *area = gtk_gl_area_new();
    ViewportState *v = g_new0(ViewportState, 1);

    gtk_gl_area_set_has_depth_buffer(GTK_GL_AREA(area), TRUE);
    g_object_set_data_full(G_OBJECT(area), MCME_VIEWPORT_KEY, v,
                           state_free);
    g_signal_connect(area, "realize", G_CALLBACK(on_realize), v);
    g_signal_connect(area, "render", G_CALLBACK(on_render), v);
    g_signal_connect(area, "unrealize", G_CALLBACK(on_unrealize), v);
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
        mcme_viewport_stage(&result->mesh, &verts, &idx, &nverts, &nidx)) {
        free(v->verts);
        free(v->idx);
        v->verts = verts;
        v->idx = idx;
        v->nverts = nverts;
        v->nidx = nidx;
        v->staged = true;
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
