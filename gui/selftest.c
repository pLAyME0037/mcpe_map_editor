/*
 * The wire's own tests, run headless by `./mcgui --selftest`:
 *
 *   fifo    — three posts, one drain, the order posted in; a NULL
 *             callback is refused; the empty drain answers zero.
 *   worker  — a thousand posts from a real worker thread, drained in
 *             visits, every payload delivered once, FIFO.
 *   repost  — a handler that posts again waits one visit (the core
 *             queue's snapshot): the loop can not grow inside itself.
 *   wake    — the wire's g_idle_add path through the real main
 *             context: two posts coalesce into one wake, the visit
 *             drains both, and the next post wakes again.
 *   pack    — the first client call (D3): a crafted pack.mcmeta read
 *             through mcme_pack_open on a worker, delivered through
 *             the wire to the loop thread, plus the named refusal for
 *             a directory that is not a pack.
 *   mesh    — G3's worker: a crafted pack's model meshed through
 *             mcme_mesh_build on a worker, delivered through the wire,
 *             plus the named refusal for the full block whose texture
 *             the pack does not hold (D32).
 *   stage   — the viewport's pure half: a mesh flattened to one
 *             interleaved vertex stream (position, tint) and one
 *             rebased index stream — and the refusal of a face that
 *             points past its bucket's vertices.
 *
 * The core queue (src/core/event_queue.c) has no glib in it and its own
 * tests can not see a main context; these checks stand where the two
 * halves meet — and where the GUI meets the core's readers — and cover
 * the queue along the way.
 */

#include "selftest.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <glib.h>
#include <sanitizer/lsan_interface.h>

#include "mcme/core.h"
#include "mcme/resource.h"
#include "meshtask.h"
#include "packtask.h"
#include "viewport.h"
#include "wire.h"

static int g_checks;
static int g_failed;

#define CHECK(cond)                                                     \
    do {                                                                \
        g_checks++;                                                     \
        if (!(cond)) {                                                  \
            g_failed++;                                                 \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__,     \
                    #cond);                                             \
        }                                                               \
    } while (0)

typedef struct {
    unsigned out[1200];
    unsigned n;
} Log;

typedef struct {
    Log *log;
    unsigned value;
} Step;

static void log_step(void *userdata)
{
    Step *s = userdata;

    if (s->log->n < sizeof(s->log->out) / sizeof(s->log->out[0]))
        s->log->out[s->log->n] = s->value;
    s->log->n++;
    free(s); /* the handler owns its userdata */
}

static bool post_step_raw(McmeEventQueue *q, Log *log, unsigned value)
{
    Step *s = malloc(sizeof(Step));

    if (s == NULL) return false;
    s->log = log;
    s->value = value;
    return mcme_eq_post(q, log_step, s);
}

static void test_fifo(void)
{
    McmeEventQueue *q = mcme_eq_new();
    Log log = {0};

    CHECK(q != NULL);
    CHECK(post_step_raw(q, &log, 1));
    CHECK(post_step_raw(q, &log, 2));
    CHECK(post_step_raw(q, &log, 3));
    CHECK(mcme_eq_drain(q) == 3);
    CHECK(log.n == 3);
    CHECK(log.out[0] == 1 && log.out[1] == 2 && log.out[2] == 3);
    CHECK(mcme_eq_drain(q) == 0);
    CHECK(mcme_eq_pending(q) == 0);
    CHECK(!mcme_eq_post(q, NULL, NULL));
    mcme_eq_free(q);
}

#define WORK_N 1000

typedef struct {
    McmeEventQueue *q;
    Log *log;
} Worker;

static gpointer worker_post(gpointer data)
{
    Worker *w = data;
    unsigned i;

    for (i = 0; i < WORK_N; i++)
        CHECK(post_step_raw(w->q, w->log, i + 1));
    return NULL;
}

static void test_worker(void)
{
    McmeEventQueue *q = mcme_eq_new();
    Log log = {0};
    Worker w = {q, &log};
    GThread *t = g_thread_new("poster", worker_post, &w);
    unsigned ran = 0, i;
    bool ordered = true;

    g_thread_join(t);
    for (;;) {
        size_t step = mcme_eq_drain(q);

        if (step == 0) break;
        ran += (unsigned)step;
    }
    CHECK(ran == WORK_N);
    CHECK(log.n == WORK_N);
    for (i = 0; i < log.n; i++)
        if (log.out[i] != i + 1) ordered = false;
    CHECK(ordered);
    mcme_eq_free(q);
}

typedef struct {
    McmeEventQueue *q;
    Log *log;
    unsigned value;
    int left; /* reposts remaining */
} RStep;

static void repost_step(void *userdata)
{
    RStep *s = userdata;

    if (s->log->n < sizeof(s->log->out) / sizeof(s->log->out[0]))
        s->log->out[s->log->n] = s->value;
    s->log->n++;
    if (s->left > 0) {
        s->left--;
        mcme_eq_post(s->q, repost_step, s); /* ownership passes back */
    } else {
        free(s);
    }
}

static void test_repost(void)
{
    McmeEventQueue *q = mcme_eq_new();
    Log log = {0};
    RStep *s = malloc(sizeof(RStep));

    CHECK(s != NULL);
    s->q = q;
    s->log = &log;
    s->value = 9;
    s->left = 1;
    CHECK(mcme_eq_post(q, repost_step, s));
    CHECK(mcme_eq_drain(q) == 1); /* the repost waits one visit */
    CHECK(log.n == 1);
    CHECK(mcme_eq_drain(q) == 1);
    CHECK(log.n == 2);
    CHECK(log.out[0] == 9 && log.out[1] == 9);
    mcme_eq_free(q);
}

static bool post_step_wire(McmeWire *wire, Log *log, unsigned value)
{
    Step *s = malloc(sizeof(Step));

    if (s == NULL) return false;
    s->log = log;
    s->value = value;
    return mcme_wire_post(wire, log_step, s);
}

static void test_wake(void)
{
    McmeWire *wire = mcme_wire_new();
    Log log = {0};

    CHECK(wire != NULL);
    CHECK(post_step_wire(wire, &log, 7));
    CHECK(post_step_wire(wire, &log, 8)); /* coalesced: one wake */
    g_main_context_iteration(NULL, TRUE);  /* waits out the source */
    CHECK(log.n == 2);                     /* both ran in the visit */
    CHECK(log.out[0] == 7 && log.out[1] == 8);
    CHECK(post_step_wire(wire, &log, 9)); /* the visit cleared the flag */
    g_main_context_iteration(NULL, TRUE);
    CHECK(log.n == 3);
    CHECK(log.out[2] == 9);
    mcme_wire_free(wire);
}

static int g_pack_done_calls;
static bool g_pack_done_ok;

/* Loop thread, via the wire's visit — the shape on_pack_result has. */
static void on_pack_done(void *userdata, McmePackResult *result)
{
    (void)userdata;
    g_pack_done_calls++;
    g_pack_done_ok = result->ok;
    mcme_packresult_free(result);
}

static void test_packtask(void)
{
    char dir[] = "/tmp/mcme_gui_pack_XXXXXX";
    char path[512];
    McmePackResult *r;
    McmeWire *wire;
    FILE *f;

    CHECK(mkdtemp(dir) != NULL);
    snprintf(path, sizeof(path), "%s/pack.mcmeta", dir);
    f = fopen(path, "w");
    CHECK(f != NULL);
    if (f != NULL) {
        fprintf(f, "{\"pack\":{\"pack_format\":34,"
                   "\"description\":\"gui selftest\"}}\n");
        fclose(f);
    }

    /* The summarize: the core's read, the CLI's four lines. */
    r = mcme_pack_summarize(dir);
    CHECK(r != NULL);
    if (r != NULL) {
        CHECK(r->ok);
        CHECK(r->text != NULL && strstr(r->text, "pack_format  34") != NULL);
        CHECK(strstr(r->text, "gui selftest") != NULL);
        mcme_packresult_free(r);
    }

    /* The refusal, named. */
    r = mcme_pack_summarize("/tmp/mcme-no-such-pack-dir");
    CHECK(r != NULL);
    if (r != NULL) {
        CHECK(!r->ok);
        CHECK(r->text != NULL && r->text[0] != '\0');
        mcme_packresult_free(r);
    }

    /* The whole client path headless: the worker summarizes, posts
     * through the wire, the loop's visit delivers. */
    wire = mcme_wire_new();
    g_pack_done_calls = 0;
    g_pack_done_ok = false;
    CHECK(mcme_packtask_start(wire, dir, on_pack_done, NULL));
    while (g_pack_done_calls == 0)
        g_main_context_iteration(NULL, TRUE);
    CHECK(g_pack_done_calls == 1);
    CHECK(g_pack_done_ok);
    mcme_wire_free(wire);

    unlink(path);
    rmdir(dir);
}

static int g_mesh_done_calls;
static bool g_mesh_done_ok;
static size_t g_mesh_done_verts;
static bool g_mesh_done_atlas;
static bool g_mesh_done_missing_first;
static bool g_mesh_done_page;
static size_t g_mesh_done_atlas_count;
static uint32_t g_mesh_done_slot0;
static bool g_mesh_done_bounds_ok;

/* Loop thread, via the wire's visit — the shape on_mesh_result has. */
static void on_mesh_done(void *userdata, McmeMeshResult *result)
{
    (void)userdata;
    g_mesh_done_calls++;
    g_mesh_done_ok = result->ok;
    if (result->ok) {
        for (int d = 0; d < 7; ++d) {
            const McmeMeshBucket *b = &result->mesh.buckets[d];

            if (!result->mesh.present[d]) continue;
            g_mesh_done_verts += b->vert_count;
            CHECK(b->face_count >= 1);
            for (size_t f = 0; f < b->face_count * 6; ++f)
                CHECK(b->faces[f] < b->vert_count);
        }
        /* The page's half (G4): the seed first, the pixels there,
           the mesh's first texture mapped into bounds that stay
           inside the unit square. */
        g_mesh_done_atlas = result->has_atlas;
        if (result->has_atlas) {
            g_mesh_done_missing_first =
                result->atlas.count > 0 &&
                strcmp(result->atlas.names[0], "missing_no") == 0;
            g_mesh_done_page = result->atlas.page.rgba != NULL &&
                               result->atlas.page.width > 0 &&
                               result->atlas.page.height > 0;
            g_mesh_done_atlas_count = result->atlas.count;
            if (result->mesh.texture_count > 0 &&
                result->tex_slots != NULL) {
                uint32_t s = result->tex_slots[0];

                g_mesh_done_slot0 = s;
                g_mesh_done_bounds_ok =
                    (size_t)s * 4 + 3 < result->atlas.count * 4 &&
                    result->atlas.bounds[s * 4 + 0] >= 0.0 &&
                    result->atlas.bounds[s * 4 + 1] >= 0.0 &&
                    result->atlas.bounds[s * 4 + 2] <= 1.0 &&
                    result->atlas.bounds[s * 4 + 3] <= 1.0;
            }
        }
    } else {
        CHECK(result->text != NULL && result->text[0] != '\0');
    }
    mcme_meshresult_free(result);
}

static void write_file(const char *path, const char *text)
{
    FILE *f = fopen(path, "w");

    CHECK(f != NULL);
    if (f != NULL) {
        fputs(text, f);
        fclose(f);
    }
}

/* The mesh task headless: a crafted pack's model meshed on a worker,
   delivered through the wire, and the named refusal. */
static void test_meshtask(void)
{
    char dir[] = "/tmp/mcme_gui_mesh_XXXXXX";
    char path[512];
    McmeWire *wire;

    CHECK(mkdtemp(dir) != NULL);
    snprintf(path, sizeof(path), "%s/pack.mcmeta", dir);
    write_file(path, "{\"pack\":{\"pack_format\":34,"
               "\"description\":\"gui mesh selftest\"}}\n");
    snprintf(path, sizeof(path), "%s/assets/mc/models/block", dir);
    CHECK(mcme_fs_mkdir_p(path));
    snprintf(path, sizeof(path), "%s/assets/minecraft/textures/block",
             dir);
    CHECK(mcme_fs_mkdir_p(path));

    /* A full cube with all six cull faces, textured block/opaque —
       the shape the CLI's own blockmesh fixture meshes (cull6). */
    snprintf(path, sizeof(path), "%s/assets/mc/models/block/cube.json",
             dir);
    write_file(path,
        "{\"textures\":{\"a\":\"block/opaque\"},\"elements\":"
        "[{\"from\":[0,0,0],\"to\":[16,16,16],\"faces\":"
        "{\"down\":{\"texture\":\"#a\",\"cullface\":\"down\"},"
        "\"up\":{\"texture\":\"#a\",\"cullface\":\"up\"},"
        "\"north\":{\"texture\":\"#a\",\"cullface\":\"north\"},"
        "\"east\":{\"texture\":\"#a\",\"cullface\":\"east\"},"
        "\"south\":{\"texture\":\"#a\",\"cullface\":\"south\"},"
        "\"west\":{\"texture\":\"#a\",\"cullface\":\"west\"}}}]}\n");

    /* The refusal, named: a full block whose texture the pack does
       not hold does not mesh (D32). The texture lands below — the
       same fixture, read again after. */
    wire = mcme_wire_new();
    g_mesh_done_calls = 0;
    g_mesh_done_ok = true;
    CHECK(mcme_meshtask_start(wire, dir, "mc", "block/cube",
                              on_mesh_done, NULL));
    while (g_mesh_done_calls == 0)
        g_main_context_iteration(NULL, TRUE);
    CHECK(g_mesh_done_calls == 1);
    CHECK(!g_mesh_done_ok);

    /* The texture the model names, through the core's own writer. */
    {
        McmeImage img = {2, 2, NULL};
        unsigned char px[2 * 2 * 4];

        for (int i = 0; i < 4; ++i) {
            px[i * 4 + 0] = 200; px[i * 4 + 1] = 170;
            px[i * 4 + 2] = 90;  px[i * 4 + 3] = 255;
        }
        img.rgba = px;
        snprintf(path, sizeof(path),
                 "%s/assets/minecraft/textures/block/opaque.png", dir);
        CHECK(mcme_image_write_png(path, &img));
    }

    /* The whole client path headless: the worker meshes, posts
     * through the wire, the loop's visit delivers. */
    g_mesh_done_calls = 0;
    g_mesh_done_ok = false;
    g_mesh_done_verts = 0;
    g_mesh_done_atlas = false;
    g_mesh_done_missing_first = false;
    g_mesh_done_page = false;
    g_mesh_done_atlas_count = 0;
    g_mesh_done_slot0 = 99;
    g_mesh_done_bounds_ok = false;
    CHECK(mcme_meshtask_start(wire, dir, "mc", "block/cube",
                              on_mesh_done, NULL));
    while (g_mesh_done_calls == 0)
        g_main_context_iteration(NULL, TRUE);
    CHECK(g_mesh_done_calls == 1);
    CHECK(g_mesh_done_ok);
    CHECK(g_mesh_done_verts >= 4); /* four vertices a face (D32) */
    /* The page rode with the mesh: the missing image seeded first,
       the pack's own file behind it, the mesh's texture mapped into
       bounds that stay inside the unit square. */
    CHECK(g_mesh_done_atlas);
    CHECK(g_mesh_done_missing_first);
    CHECK(g_mesh_done_page);
    CHECK(g_mesh_done_atlas_count >= 2);
    CHECK(g_mesh_done_slot0 > 0);
    CHECK(g_mesh_done_bounds_ok);

    /* A face whose texture the pack does not hold is not a full
       block: the mesh answers, and the missing image is its texture
       — the page's seeded first slot. */
    snprintf(path, sizeof(path), "%s/assets/mc/models/block/partial.json",
             dir);
    write_file(path,
        "{\"textures\":{\"a\":\"block/absent\"},\"elements\":"
        "[{\"from\":[0,0,0],\"to\":[16,16,16],\"faces\":"
        "{\"up\":{\"texture\":\"#a\"}}}]}\n");
    g_mesh_done_calls = 0;
    g_mesh_done_ok = false;
    g_mesh_done_slot0 = 99;
    CHECK(mcme_meshtask_start(wire, dir, "mc", "block/partial",
                              on_mesh_done, NULL));
    while (g_mesh_done_calls == 0)
        g_main_context_iteration(NULL, TRUE);
    CHECK(g_mesh_done_calls == 1);
    CHECK(g_mesh_done_ok);
    CHECK(g_mesh_done_atlas);
    CHECK(g_mesh_done_slot0 == 0); /* missing_no, the seed */

    /* A texture file stb will not read: the index still holds it by
       name (the mesh answers), but the page refuses whole (D34) —
       the degradation is the tint-only viewport, not a lost mesh. */
    snprintf(path, sizeof(path),
             "%s/assets/minecraft/textures/block/opaque.png", dir);
    write_file(path, "not a png\n");
    g_mesh_done_calls = 0;
    g_mesh_done_ok = false;
    g_mesh_done_atlas = true;
    CHECK(mcme_meshtask_start(wire, dir, "mc", "block/cube",
                              on_mesh_done, NULL));
    while (g_mesh_done_calls == 0)
        g_main_context_iteration(NULL, TRUE);
    CHECK(g_mesh_done_calls == 1);
    CHECK(g_mesh_done_ok);
    CHECK(!g_mesh_done_atlas);
    mcme_wire_free(wire);

    snprintf(path, sizeof(path), "%s/pack.mcmeta", dir);
    unlink(path);
    snprintf(path, sizeof(path), "%s/assets/mc/models/block/cube.json",
             dir);
    unlink(path);
    snprintf(path, sizeof(path), "%s/assets/mc/models/block/partial.json",
             dir);
    unlink(path);
    snprintf(path, sizeof(path),
             "%s/assets/minecraft/textures/block/opaque.png", dir);
    unlink(path);
    snprintf(path, sizeof(path), "%s/assets/mc/models/block", dir);
    rmdir(path);
    snprintf(path, sizeof(path), "%s/assets/mc/models", dir);
    rmdir(path);
    snprintf(path, sizeof(path), "%s/assets/mc", dir);
    rmdir(path);
    snprintf(path, sizeof(path), "%s/assets/minecraft/textures/block",
             dir);
    rmdir(path);
    snprintf(path, sizeof(path), "%s/assets/minecraft/textures", dir);
    rmdir(path);
    snprintf(path, sizeof(path), "%s/assets/minecraft", dir);
    rmdir(path);
    snprintf(path, sizeof(path), "%s/assets", dir);
    rmdir(path);
    rmdir(dir);
}

static double *darray(size_t n)
{
    double *p = calloc(n, sizeof *p);

    CHECK(p != NULL);
    return p;
}

static uint32_t *uarray(size_t n)
{
    uint32_t *p = calloc(n, sizeof *p);

    CHECK(p != NULL);
    return p;
}

/* The viewport's staging half, pure and headless: two buckets in,
   one interleaved stream and one rebased index stream out — each
   face's uvs lerped into its slot's page bounds, the tint-only
   fallback, and the refusal of a face that points past its bucket. */
static void test_stage(void)
{
    McmeMesh m = {0};
    McmeMeshBucket *up, *none;
    McmeAtlas atlas = {0};
    double bounds[8] = {0.0, 0.0, 0.25, 0.25,   /* slot 0, missing  */
                        0.25, 0.5, 0.75, 0.75}; /* slot 1           */
    uint32_t slots[1] = {1};                    /* the mesh's one   */
    char **tex_names;                           /* texture -> 1     */
    float *verts = NULL;
    uint32_t *idx = NULL;
    size_t nverts = 0, nidx = 0;

    tex_names = calloc(1, sizeof *tex_names);
    CHECK(tex_names != NULL); /* mcme_mesh_free frees per entry */
    m.textures = tex_names;
    m.texture_count = 1;

    up = &m.buckets[MCME_CULL_UP];
    none = &m.buckets[MCME_CULL_NONE];
    m.present[MCME_CULL_UP] = true;
    m.present[MCME_CULL_NONE] = true;
    m.texture_count = 1;

    up->vert_count = 4;
    up->face_count = 1;
    up->verts = darray(12);
    up->tverts = darray(8);
    up->tint_verts = darray(12);
    up->faces = uarray(6);
    up->texture_indexes = uarray(2);
    for (int v = 0; v < 4; ++v) {
        up->verts[v * 3 + 0] = v;          /* x = 0,1,2,3      */
        up->verts[v * 3 + 1] = 10 + v;     /* y                */
        up->verts[v * 3 + 2] = 20 + v;     /* z                */
        up->tint_verts[v * 3 + 0] = 0.25;  /* flat tint, red   */
    }
    up->tverts[0] = 0; up->tverts[1] = 1;
    up->tverts[2] = 1; up->tverts[3] = 1;
    up->tverts[4] = 1; up->tverts[5] = 0;
    up->tverts[6] = 0; up->tverts[7] = 0;
    up->texture_indexes[0] = 0;
    up->texture_indexes[1] = 0;
    up->faces[0] = 0; up->faces[1] = 1; up->faces[2] = 2;
    up->faces[3] = 2; up->faces[4] = 3; up->faces[5] = 0;

    none->vert_count = 3;
    none->face_count = 1;
    none->verts = darray(9);
    none->tverts = darray(6);
    none->tint_verts = darray(9);
    none->faces = uarray(6);
    none->texture_indexes = uarray(2);
    none->texture_indexes[0] = 0;
    none->texture_indexes[1] = 0;
    none->faces[0] = 0; none->faces[1] = 1; none->faces[2] = 2;
    none->faces[3] = 2; none->faces[4] = 1; none->faces[5] = 0;

    atlas.count = 2;
    atlas.bounds = bounds;

    CHECK(mcme_viewport_stage(&m, &atlas, slots, &verts, &idx,
                              &nverts, &nidx));
    CHECK(nverts == 7);
    CHECK(nidx == 12);
    if (verts != NULL && idx != NULL) {
        /* The interleave: position, uv, tint — per vertex. The
           first vertex's (u,v) = (0,1) lands on the slot's far
           corner: u0 + 0*(u1-u0) = 0.25, v0 + 1*(v1-v0) = 0.75. */
        CHECK(verts[0] == 0.0f && verts[1] == 10.0f &&
              verts[2] == 20.0f);
        CHECK(verts[3] == 0.25f && verts[4] == 0.75f);
        CHECK(verts[5] == 0.25f && verts[6] == 0.0f &&
              verts[7] == 0.0f);
        CHECK(verts[1 * 8 + 3] == 0.75f &&
              verts[1 * 8 + 4] == 0.75f);
        CHECK(verts[2 * 8 + 3] == 0.75f &&
              verts[2 * 8 + 4] == 0.5f);
        /* The second bucket's first vertex sits at base 4; its
           zero uvs land on the slot's origin corner. */
        CHECK(verts[4 * 8 + 0] == 0.0f);
        CHECK(verts[4 * 8 + 3] == 0.25f &&
              verts[4 * 8 + 4] == 0.5f);
        CHECK(verts[4 * 8 + 5] == 0.0f);
        /* The rebase: none's local 0,1,2 became 4,5,6. */
        CHECK(idx[0] == 0 && idx[4] == 3);
        CHECK(idx[6] == 4 && idx[7] == 5 && idx[8] == 6);
    }
    free(verts);
    free(idx);

    /* No page: every uv 0 — the tint-only fallback. */
    verts = NULL;
    idx = NULL;
    CHECK(mcme_viewport_stage(&m, NULL, slots, &verts, &idx,
                              &nverts, &nidx));
    if (verts != NULL) {
        CHECK(verts[3] == 0.0f && verts[4] == 0.0f);
        CHECK(verts[4 * 8 + 3] == 0.0f);
    }
    free(verts);
    free(idx);

    /* A slot past the page: the missing image's corner answers —
       uv stays inside the page either way. */
    slots[0] = 9;
    verts = NULL;
    idx = NULL;
    CHECK(mcme_viewport_stage(&m, &atlas, slots, &verts, &idx,
                              &nverts, &nidx));
    if (verts != NULL) {
        CHECK(verts[3] == 0.0f && verts[4] == 0.0f);
    }
    free(verts);
    free(idx);
    slots[0] = 1;

    /* A face past its bucket's vertices: refused, nothing handed. */
    up->faces[4] = 99;
    verts = NULL;
    idx = NULL;
    CHECK(!mcme_viewport_stage(&m, &atlas, slots, &verts, &idx,
                               &nverts, &nidx));
    CHECK(verts == NULL && idx == NULL);

    CHECK(!mcme_viewport_stage(NULL, &atlas, slots, &verts, &idx,
                               &nverts, &nidx));

    mcme_mesh_free(&m); /* hand-built arrays free like the core's */
}

int mcme_gui_selftest(void)
{
    test_fifo();
    test_worker();
    test_repost();
    test_wake();
    test_packtask();
    test_meshtask();
    test_stage();

    /* The binary's default turns LeakSanitizer off for GTK's caches;
     * the wire's own allocations are asserted here, where nothing has
     * initialized GTK and every Step and queue must be gone. */
    CHECK(__lsan_do_recoverable_leak_check() == 0);

    if (g_failed == 0) {
        printf("PASS — the event wire, %d checks\n", g_checks);
        return 0;
    }
    printf("FAIL — the event wire, %d of %d checks\n",
           g_failed, g_checks);
    return 1;
}
