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
 *   camera  — G8's hand, headless: the default orbit's matrix IS
 *             the old fixed still's, element for element (the
 *             picture a pack opens to never moved); the drag's
 *             slope and tilt clamps; the zoom's clamps; a finite
 *             frame at every extreme the clamps allow.
 *   worldbar — G13's bar, headless: the parser the flags and the
 *             entries share (the whole token consumed, int32's
 *             range) and the validation of the bar's whole
 *             question — a world directory and three whole
 *             coordinates — every refusal a static sentence:
 *             nothing allocates, so the leak check at the end
 *             stands over it too.
 *   pick    — G14's point, headless: the matrix inverse the ray
 *             unprojects through (m·inv = I), the sight-line back
 *             through a known world point's own pixel, the center
 *             ray through the orbit's fixed point (0.5, 0.5,
 *             dist), and the cell under the struck face — the
 *             reference's outward winding over hand-laid quads
 *             (both sides), nearest hit over far, behind and
 *             miss, the frame's clamp, and all six faces of the
 *             reference's own missing-block cube answering the
 *             cell it stands on.
 *
 * The core queue (src/core/event_queue.c) has no glib in it and its own
 * tests can not see a main context; these checks stand where the two
 * halves meet — and where the GUI meets the core's readers — and cover
 * the queue along the way.
 */

#include "selftest.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <glib.h>
#include <sanitizer/lsan_interface.h>

#include "mcme/convert.h"
#include "mcme/core.h"
#include "mcme/resource.h"
#include "mcme/world.h"
#include "camera.h"
#include "chunktask.h"
#include "converttask.h"
#include "filltask.h"
#include "meshtask.h"
#include "packtask.h"
#include "pick.h"
#include "prunetask.h"
#include "statstask.h"
#include "viewport.h"
#include "wire.h"
#include "worldbar.h"

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

#define CHECK_MSG(cond, ...)                                            \
    do {                                                                \
        g_checks++;                                                     \
        if (!(cond)) {                                                  \
            g_failed++;                                                 \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__,     \
                    #cond);                                             \
            fprintf(stderr, "    ");                                    \
            fprintf(stderr, __VA_ARGS__);                               \
            fprintf(stderr, "\n");                                      \
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
static size_t g_pack_done_models;
static bool g_pack_done_has_pick;
static size_t g_pack_done_blocks;
static bool g_pack_done_has_pickblock;

/* Loop thread, via the wire's visit — the shape on_pack_result has. */
static void on_pack_done(void *userdata, McmePackResult *result)
{
    (void)userdata;
    g_pack_done_calls++;
    g_pack_done_ok = result->ok;
    g_pack_done_models = result->model_count;
    g_pack_done_has_pick = false;
    for (size_t i = 0; i < result->model_count; ++i)
        if (result->models[i] != NULL &&
            strcmp(result->models[i], "mc:block/pick") == 0)
            g_pack_done_has_pick = true;
    g_pack_done_blocks = result->block_count;
    g_pack_done_has_pickblock = false;
    for (size_t i = 0; i < result->block_count; ++i)
        if (result->blocks[i] != NULL &&
            strcmp(result->blocks[i], "mc:pickblock") == 0)
            g_pack_done_has_pickblock = true;
    mcme_packresult_free(result);
}

static void write_file(const char *path, const char *text);

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
    /* One model, so the result's list has something to carry. */
    snprintf(path, sizeof(path), "%s/assets/mc/models/block", dir);
    CHECK(mcme_fs_mkdir_p(path));
    snprintf(path, sizeof(path), "%s/assets/mc/models/block/pick.json",
             dir);
    write_file(path, "{\"textures\":{\"a\":\"t/a\"}}\n");
    /* One blockstate, so the twin list has something too (G7). */
    snprintf(path, sizeof(path), "%s/assets/mc/blockstates", dir);
    CHECK(mcme_fs_mkdir_p(path));
    snprintf(path, sizeof(path),
             "%s/assets/mc/blockstates/pickblock.json", dir);
    write_file(path, "{\"variants\":{\"\":{\"model\":\"mc:block/pick\"}}}\n");

    /* The summarize: the core's read, the CLI's four lines, and both
       lists — the pack's one model as "ns:path", its one blockstate
       as "ns:name" (D53/G7, each index's own walk order). */
    r = mcme_pack_summarize(dir);
    CHECK(r != NULL);
    if (r != NULL) {
        CHECK(r->ok);
        CHECK(r->text != NULL && strstr(r->text, "pack_format  34") != NULL);
        CHECK(strstr(r->text, "gui selftest") != NULL);
        CHECK(r->model_count == 1);
        CHECK(r->models != NULL && r->models[0] != NULL &&
              strcmp(r->models[0], "mc:block/pick") == 0);
        CHECK(r->models != NULL && r->models[1] == NULL);
        CHECK(r->block_count == 1);
        CHECK(r->blocks != NULL && r->blocks[0] != NULL &&
              strcmp(r->blocks[0], "mc:pickblock") == 0);
        CHECK(r->blocks != NULL && r->blocks[1] == NULL);
        mcme_packresult_free(r);
    }

    /* The refusal, named, with no lists at all. */
    r = mcme_pack_summarize("/tmp/mcme-no-such-pack-dir");
    CHECK(r != NULL);
    if (r != NULL) {
        CHECK(!r->ok);
        CHECK(r->text != NULL && r->text[0] != '\0');
        CHECK(r->model_count == 0);
        CHECK(r->models == NULL);
        CHECK(r->block_count == 0);
        CHECK(r->blocks == NULL);
        mcme_packresult_free(r);
    }

    /* The whole client path headless: the worker summarizes, posts
     * through the wire, the loop's visit delivers. */
    wire = mcme_wire_new();
    g_pack_done_calls = 0;
    g_pack_done_ok = false;
    g_pack_done_models = 99;
    g_pack_done_has_pick = false;
    g_pack_done_blocks = 99;
    g_pack_done_has_pickblock = false;
    CHECK(mcme_packtask_start(wire, dir, on_pack_done, NULL));
    while (g_pack_done_calls == 0)
        g_main_context_iteration(NULL, TRUE);
    CHECK(g_pack_done_calls == 1);
    CHECK(g_pack_done_ok);
    CHECK(g_pack_done_models == 1);
    CHECK(g_pack_done_has_pick);
    CHECK(g_pack_done_blocks == 1);
    CHECK(g_pack_done_has_pickblock);
    mcme_wire_free(wire);

    unlink(path);
    snprintf(path, sizeof(path), "%s/assets/mc/blockstates", dir);
    rmdir(path);
    snprintf(path, sizeof(path), "%s/assets/mc/models/block", dir);
    rmdir(path);
    snprintf(path, sizeof(path), "%s/assets/mc/models", dir);
    rmdir(path);
    snprintf(path, sizeof(path), "%s/assets/mc", dir);
    rmdir(path);
    snprintf(path, sizeof(path), "%s/assets", dir);
    rmdir(path);
    snprintf(path, sizeof(path), "%s/pack.mcmeta", dir);
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
    /* The blockmesh layer's row (G7): one blockstate naming the cube
       the model runs mesh — a bare name, no states. */
    snprintf(path, sizeof(path), "%s/assets/mc/blockstates", dir);
    CHECK(mcme_fs_mkdir_p(path));
    snprintf(path, sizeof(path), "%s/assets/mc/blockstates/test.json",
             dir);
    write_file(path, "{\"variants\":{\"\":{\"model\":\"mc:block/cube\"}}}\n");

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

    /* G7's twin: the blockmesh layer's walk (D33) — the block's ""
       variant picks the cube the model run meshed, and the page
       rides with it the same road. */
    g_mesh_done_calls = 0;
    g_mesh_done_ok = false;
    g_mesh_done_verts = 0;
    g_mesh_done_atlas = false;
    CHECK(mcme_meshtask_start_block(wire, dir, "mc", "test",
                                    on_mesh_done, NULL));
    while (g_mesh_done_calls == 0)
        g_main_context_iteration(NULL, TRUE);
    CHECK(g_mesh_done_calls == 1);
    CHECK(g_mesh_done_ok);
    CHECK(g_mesh_done_verts >= 4);
    CHECK(g_mesh_done_atlas);

    /* The total answer (D33): an unknown block is the missing mesh,
       not a refusal — the block-level answer never fails the pack. */
    g_mesh_done_calls = 0;
    g_mesh_done_ok = false;
    g_mesh_done_verts = 0;
    CHECK(mcme_meshtask_start_block(wire, dir, "mc", "nosuchblock",
                                    on_mesh_done, NULL));
    while (g_mesh_done_calls == 0)
        g_main_context_iteration(NULL, TRUE);
    CHECK(g_mesh_done_calls == 1);
    CHECK(g_mesh_done_ok);
    CHECK(g_mesh_done_verts >= 4);

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
    snprintf(path, sizeof(path), "%s/assets/mc/blockstates/test.json",
             dir);
    unlink(path);
    snprintf(path, sizeof(path),
             "%s/assets/minecraft/textures/block/opaque.png", dir);
    unlink(path);
    snprintf(path, sizeof(path), "%s/assets/mc/blockstates", dir);
    rmdir(path);
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

static int g_chunk_done_calls;
static bool g_chunk_done_ok;
static char g_chunk_done_spec[512];
static char g_chunk_done_text[512];
static size_t g_chunk_done_verts;
static size_t g_chunk_done_faces;
static size_t g_chunk_done_faces_total;
static size_t g_chunk_done_cells;
static bool g_chunk_done_atlas;
/* the tint fold's wiring pin (G12): when armed, every staged
   vertex answers the direction's brightness and the band at the
   ask's own height — the pack's raw tints are white, so anything
   else is the fold misreading where it stands */
static bool g_chunk_check_tints;
static double g_chunk_tint_base;
static size_t g_chunk_done_tint_bad;

/* Loop thread, via the wire's visit — the shape on_mesh_result has,
   with the world's own words riding both answers. */
static void on_chunk_done(void *userdata, McmeChunkResult *result)
{
    (void)userdata;
    g_chunk_done_calls++;
    g_chunk_done_ok = result->ok;
    g_chunk_done_spec[0] = '\0';
    g_chunk_done_text[0] = '\0';
    g_chunk_done_verts = 0;
    g_chunk_done_faces = 0;
    g_chunk_done_faces_total = 0;
    g_chunk_done_cells = 0;
    g_chunk_done_atlas = false;
    g_chunk_done_tint_bad = 0;
    if (result->ok) {
        snprintf(g_chunk_done_spec, sizeof g_chunk_done_spec, "%s",
                 result->spec != NULL ? result->spec : "");
        g_chunk_done_faces = result->faces;
        g_chunk_done_faces_total = result->faces_total;
        g_chunk_done_cells = result->cells;
        if (result->mesh != NULL) {
            for (int d = 0; d < 7; ++d)
                if (result->mesh->mesh.present[d])
                    g_chunk_done_verts +=
                        result->mesh->mesh.buckets[d].vert_count;
            g_chunk_done_atlas = result->mesh->has_atlas;
            if (g_chunk_check_tints && result->mesh->ok) {
                const McmeMesh *sm = &result->mesh->mesh;
                for (int d = 0; d <= MCME_CULL_NONE; ++d) {
                    if (!sm->present[d]) continue;
                    const McmeMeshBucket *b = &sm->buckets[d];
                    double bright = mcme_tint_brightness(d);
                    for (size_t v = 0; v < b->vert_count; ++v) {
                        double want = bright
                            * mcme_tint_band(g_chunk_tint_base
                                             + b->verts[v * 3 + 1]);
                        for (int c = 0; c < 3; ++c)
                            if (b->tint_verts[v * 3 + c] != want)
                                g_chunk_done_tint_bad++;
                    }
                }
            }
        }
    } else {
        snprintf(g_chunk_done_text, sizeof g_chunk_done_text, "%s",
                 result->text != NULL ? result->text : "");
    }
    mcme_chunkresult_free(result);
}

/* The world's block through the wire, headless (G9): the battery's
   own probe — the golden world answers "minecraft:air" at (5,5,5)
   (ref_java_blocks) — meshed by a crafted pack under the block's
   own namespace, the spec riding the result, and the named
   refusals: a directory that is not a world, and the nether this
   world keeps no DIM-1 for (ref_chunkmesh's own refusal). */
static void test_chunktask(void)
{
    char dir[] = "/tmp/mcme_gui_chunk_XXXXXX";
    char path[512];
    McmeWire *wire;
    bool golden = mcme_fs_exists("tests/golden/worlds/java");

    printf("  chunktask (the world's own block and sub-chunk through "
           "the wire)\n");

    CHECK(mkdtemp(dir) != NULL);
    snprintf(path, sizeof(path), "%s/pack.mcmeta", dir);
    write_file(path, "{\"pack\":{\"pack_format\":34,"
               "\"description\":\"gui chunk selftest\"}}\n");
    snprintf(path, sizeof(path), "%s/assets/mc/models/block", dir);
    CHECK(mcme_fs_mkdir_p(path));
    snprintf(path, sizeof(path), "%s/assets/minecraft/textures/block",
             dir);
    CHECK(mcme_fs_mkdir_p(path));
    /* The pack's selection for the world's answer — under the
       block's OWN namespace (the chunk worker's split, G9), the
       cube test_meshtask meshes already. */
    snprintf(path, sizeof(path),
             "%s/assets/minecraft/blockstates", dir);
    CHECK(mcme_fs_mkdir_p(path));
    snprintf(path, sizeof(path),
             "%s/assets/minecraft/blockstates/air.json", dir);
    write_file(path, "{\"variants\":{\"\":{\"model\":"
                     "\"mc:block/cube\"}}}\n");
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

    wire = mcme_wire_new();

    /* A directory that is not a world: refused, its own words. */
    g_chunk_done_calls = 0;
    g_chunk_done_ok = true;
    CHECK(mcme_chunktask_start(wire, "/tmp/mcme_gui_no_world_dir",
                               dir, 0, 5, 5, 5, false, on_chunk_done,
                               NULL));
    while (g_chunk_done_calls == 0)
        g_main_context_iteration(NULL, TRUE);
    CHECK(g_chunk_done_calls == 1);
    CHECK(!g_chunk_done_ok);
    CHECK(g_chunk_done_text[0] != '\0');
    mcme_clear_error();

    /* the same refusal asked over the whole sub-chunk (G11) */
    g_chunk_done_calls = 0;
    g_chunk_done_ok = true;
    CHECK(mcme_chunktask_start(wire, "/tmp/mcme_gui_no_world_dir",
                               dir, 0, 5, 5, 5, true, on_chunk_done,
                               NULL));
    while (g_chunk_done_calls == 0)
        g_main_context_iteration(NULL, TRUE);
    CHECK(g_chunk_done_calls == 1);
    CHECK(!g_chunk_done_ok);
    CHECK(g_chunk_done_text[0] != '\0');
    mcme_clear_error();

    if (golden) {
        /* The battery's probe: the golden's sections read as the
           format's air — the spec rides the answer, and the pack's
           cube dresses it. */
        g_chunk_done_calls = 0;
        g_chunk_done_ok = false;
        CHECK(mcme_chunktask_start(wire, "tests/golden/worlds/java",
                                   dir, 0, 5, 5, 5, false,
                                   on_chunk_done, NULL));
        while (g_chunk_done_calls == 0)
            g_main_context_iteration(NULL, TRUE);
        CHECK(g_chunk_done_calls == 1);
        CHECK(g_chunk_done_ok);
        CHECK(strcmp(g_chunk_done_spec, "minecraft:air") == 0);
        /* The pack's air is an opaque cube, and every neighbour's
           is: the cull keeps nothing — the wire's own 0/6 (G10,
           the clauses firing on the neighbour's flag alone). */
        CHECK(g_chunk_done_faces == 0);
        CHECK(g_chunk_done_faces_total == 6);
        CHECK(g_chunk_done_cells == 0);
        CHECK(g_chunk_done_verts == 0);
        CHECK(g_chunk_done_atlas);

        /* The nether: no DIM-1 in this world, so the chunk is not
           stored — the refusal, through the wire. */
        g_chunk_done_calls = 0;
        g_chunk_done_ok = true;
        CHECK(mcme_chunktask_start(wire, "tests/golden/worlds/java",
                                   dir, 1, 5, 5, 5, false,
                                   on_chunk_done, NULL));
        while (g_chunk_done_calls == 0)
            g_main_context_iteration(NULL, TRUE);
        CHECK(g_chunk_done_calls == 1);
        CHECK(!g_chunk_done_ok);
        CHECK(g_chunk_done_text[0] != '\0');
        mcme_clear_error();

        /* the section ask's own: the compose refuses the nether
           the same way (G11 — the core's grid, the same status) */
        g_chunk_done_calls = 0;
        g_chunk_done_ok = true;
        CHECK(mcme_chunktask_start(wire, "tests/golden/worlds/java",
                                   dir, 1, 5, 5, 5, true,
                                   on_chunk_done, NULL));
        while (g_chunk_done_calls == 0)
            g_main_context_iteration(NULL, TRUE);
        CHECK(g_chunk_done_calls == 1);
        CHECK(!g_chunk_done_ok);
        CHECK(g_chunk_done_text[0] != '\0');
        mcme_clear_error();

        /* The clauses' other answer (G10): the pack's air as a
           slab — partial on both counts, so neither clause fires
           and every face stands, the reference's own air record
           (partial). Same world, same probe; only the pack's own
           selection changed. */
        snprintf(path, sizeof(path),
                 "%s/assets/minecraft/blockstates/air.json", dir);
        write_file(path, "{\"variants\":{\"\":{\"model\":"
                         "\"mc:block/slab\"}}}\n");
        snprintf(path, sizeof(path),
                 "%s/assets/mc/models/block/slab.json", dir);
        write_file(path,
            "{\"textures\":{\"a\":\"block/opaque\"},\"elements\":"
            "[{\"from\":[0,0,0],\"to\":[16,6,16],\"faces\":"
            "{\"down\":{\"texture\":\"#a\",\"cullface\":\"down\"},"
            "\"up\":{\"texture\":\"#a\",\"cullface\":\"up\"},"
            "\"north\":{\"texture\":\"#a\",\"cullface\":\"north\"},"
            "\"east\":{\"texture\":\"#a\",\"cullface\":\"east\"},"
            "\"south\":{\"texture\":\"#a\",\"cullface\":\"south\"},"
            "\"west\":{\"texture\":\"#a\",\"cullface\":\"west\"}}}]}\n");

        /* the fold's wiring over the block ask (G12): the pack's
           tints are white, so every staged vertex answers the
           direction's brightness and the band at the block's own
           height — 5, under the model's local 0 and 1 */
        g_chunk_check_tints = true;
        g_chunk_tint_base = 5.0;
        g_chunk_done_calls = 0;
        g_chunk_done_ok = false;
        CHECK(mcme_chunktask_start(wire, "tests/golden/worlds/java",
                                   dir, 0, 5, 5, 5, false,
                                   on_chunk_done, NULL));
        while (g_chunk_done_calls == 0)
            g_main_context_iteration(NULL, TRUE);
        CHECK(g_chunk_done_calls == 1);
        CHECK(g_chunk_done_ok);
        CHECK(strcmp(g_chunk_done_spec, "minecraft:air") == 0);
        CHECK(g_chunk_done_faces == 6);
        CHECK(g_chunk_done_faces_total == 6);
        CHECK(g_chunk_done_cells == 1);
        CHECK(g_chunk_done_verts == 24);
        CHECK(g_chunk_done_atlas);
        CHECK_MSG(g_chunk_done_tint_bad == 0,
                  "block ask: %zu vertices folded wrong",
                  g_chunk_done_tint_bad);
        g_chunk_check_tints = false;

        /* The WHOLE SUB-CHUNK over the same slab pack (G11): every
           cell is the pack's partial air, so nothing culls and the
           4096 cells all stand — the section ask's drawn extreme,
           merged to one mesh at the sub-chunk's own origin.
           Its own wiring pin (G12): the fold reads the
           sub-chunk's floor — y 5 stands in section y 0 — under
           every merged vertex at cell + local, all 98304 of
           them through the direction's brightness and the band. */
        g_chunk_check_tints = true;
        g_chunk_tint_base = 0.0;
        g_chunk_done_calls = 0;
        g_chunk_done_ok = false;
        CHECK(mcme_chunktask_start(wire, "tests/golden/worlds/java",
                                   dir, 0, 5, 5, 5, true,
                                   on_chunk_done, NULL));
        while (g_chunk_done_calls == 0)
            g_main_context_iteration(NULL, TRUE);
        CHECK(g_chunk_done_calls == 1);
        CHECK(g_chunk_done_ok);
        CHECK(strcmp(g_chunk_done_spec, "minecraft:air") == 0);
        CHECK(g_chunk_done_faces == 4096u * 6u);
        CHECK(g_chunk_done_faces_total == 4096u * 6u);
        CHECK(g_chunk_done_cells == 4096);
        CHECK(g_chunk_done_verts == 4096u * 24u);
        CHECK(g_chunk_done_atlas);
        CHECK_MSG(g_chunk_done_tint_bad == 0,
                  "section ask: %zu vertices folded wrong",
                  g_chunk_done_tint_bad);
        g_chunk_check_tints = false;

        /* The pack's air back to its opaque cube: the section's
           other extreme — every cell buried in opaque neighbours,
           so the whole sub-chunk crosses empty, its 24576 faces
           all counted before the cull (the settle's 0/N). */
        snprintf(path, sizeof(path),
                 "%s/assets/minecraft/blockstates/air.json", dir);
        write_file(path, "{\"variants\":{\"\":{\"model\":"
                         "\"mc:block/cube\"}}}\n");

        g_chunk_done_calls = 0;
        g_chunk_done_ok = false;
        CHECK(mcme_chunktask_start(wire, "tests/golden/worlds/java",
                                   dir, 0, 5, 5, 5, true,
                                   on_chunk_done, NULL));
        while (g_chunk_done_calls == 0)
            g_main_context_iteration(NULL, TRUE);
        CHECK(g_chunk_done_calls == 1);
        CHECK(g_chunk_done_ok);
        CHECK(strcmp(g_chunk_done_spec, "minecraft:air") == 0);
        CHECK(g_chunk_done_faces == 0);
        CHECK(g_chunk_done_faces_total == 4096u * 6u);
        CHECK(g_chunk_done_cells == 0);
        CHECK(g_chunk_done_verts == 0);
        CHECK(g_chunk_done_atlas);
    } else {
        printf("  SKIP golden world probes (tests/golden absent)\n");
    }

    mcme_wire_free(wire);

    snprintf(path, sizeof(path), "%s/pack.mcmeta", dir);
    unlink(path);
    snprintf(path, sizeof(path), "%s/assets/mc/models/block/cube.json",
             dir);
    unlink(path);
    snprintf(path, sizeof(path), "%s/assets/mc/models/block/slab.json",
             dir);
    unlink(path);
    snprintf(path, sizeof(path),
             "%s/assets/minecraft/blockstates/air.json", dir);
    unlink(path);
    snprintf(path, sizeof(path),
             "%s/assets/minecraft/textures/block/opaque.png", dir);
    unlink(path);
    snprintf(path, sizeof(path), "%s/assets/minecraft/blockstates", dir);
    rmdir(path);
    snprintf(path, sizeof(path), "%s/assets/minecraft/textures/block",
             dir);
    rmdir(path);
    snprintf(path, sizeof(path), "%s/assets/mc/models/block", dir);
    rmdir(path);
    snprintf(path, sizeof(path), "%s/assets/mc/models", dir);
    rmdir(path);
    snprintf(path, sizeof(path), "%s/assets/mc", dir);
    rmdir(path);
    snprintf(path, sizeof(path), "%s/assets/minecraft/textures", dir);
    rmdir(path);
    snprintf(path, sizeof(path), "%s/assets/minecraft", dir);
    rmdir(path);
    snprintf(path, sizeof(path), "%s/assets", dir);
    rmdir(path);
    rmdir(dir);
}

/* the fill worker's answers through the wire (G15) */
static int g_fill_done_calls;
static bool g_fill_done_ok;
static char g_fill_done_text[512];

static void on_fill_done(void *userdata, McmeFillResult *result)
{
    (void)userdata;
    g_fill_done_calls++;
    g_fill_done_ok = result->ok;
    snprintf(g_fill_done_text, sizeof g_fill_done_text, "%s",
             result->text ? result->text : "");
    mcme_fillresult_free(result);
}

/* A temp world's store, gone — GDir's own walk (glib is here); a
   plain file falls to unlink. */
static void rm_rf(const char *path)
{
    GDir *d = g_dir_open(path, 0, NULL);

    if (d != NULL) {
        const char *name;

        while ((name = g_dir_read_name(d)) != NULL) {
            char sub[512];

            snprintf(sub, sizeof sub, "%s/%s", path, name);
            rm_rf(sub);
        }
        g_dir_close(d);
        rmdir(path);
    } else {
        unlink(path);
    }
}

/* A file tree, copied whole — GDir's walk both ways (glib is
   here); the bytes through g_file_get/set_contents, so a store's
   binary files ride as they stand. */
static void cp_rf(const char *src, const char *dst)
{
    GDir *d = g_dir_open(src, 0, NULL);

    if (d == NULL) return;
    for (const char *name; (name = g_dir_read_name(d)) != NULL; ) {
        char from[512], to[512];
        GDir *sub;

        snprintf(from, sizeof from, "%s/%s", src, name);
        snprintf(to, sizeof to, "%s/%s", dst, name);
        sub = g_dir_open(from, 0, NULL);
        if (sub != NULL) {
            g_dir_close(sub);
            g_mkdir_with_parents(to, 0755);
            cp_rf(from, to);
        } else {
            gchar *data = NULL;
            gsize len = 0;

            if (g_file_get_contents(from, &data, &len, NULL)) {
                g_file_set_contents(to, data, len, NULL);
                g_free(data);
            }
        }
    }
    g_dir_close(d);
}

/* The lane's first writer (G15, G17): one cell into a store,
   through the wire — the probe's own words when the directory is
   not a world, the verbs' parser's refusal for a spec it will not
   take, the key's own range, and the write itself read back
   through the CLI's own spec road (mcme_world_block_spec): the
   store's answer, not the writer's claim. Both kinds write (G17):
   Bedrock under the store's LOCK, Java through the CLI's own fill
   road now in core — a chunk the region file does not have
   refusing the whole fill in the file's own words. */
static void test_filltask(void)
{
    char dir[] = "/tmp/mcme_gui_fill_XXXXXX";
    McmeWire *wire;
    McmeWorldInfo info;
    McmeBuffer spec;
    bool bedrock = mcme_fs_exists("tests/golden/worlds/bedrock");

    printf("  filltask (one cell into a store, read back through "
           "the wire)\n");

    CHECK(mkdtemp(dir) != NULL);

    wire = mcme_wire_new();

    /* a directory that is not a world: refused in the store's own
       words, and the LOCK never runs — no store stands in its
       place afterwards */
    g_fill_done_calls = 0;
    g_fill_done_ok = true;
    CHECK(mcme_filltask_start(wire, "/tmp/mcme_gui_no_world_dir",
                              0, 0, 80, 0, "minecraft:stone",
                              on_fill_done, NULL));
    while (g_fill_done_calls == 0)
        g_main_context_iteration(NULL, TRUE);
    CHECK(g_fill_done_calls == 1);
    CHECK(!g_fill_done_ok);
    CHECK(g_fill_done_text[0] != '\0');
    CHECK(!mcme_fs_exists("/tmp/mcme_gui_no_world_dir/db"));
    mcme_clear_error();

    /* the guards: nothing starts, nothing posts */
    CHECK(!mcme_filltask_start(NULL, dir, 0, 0, 80, 0,
                               "minecraft:stone", on_fill_done,
                               NULL));
    CHECK(!mcme_filltask_start(wire, NULL, 0, 0, 80, 0,
                               "minecraft:stone", on_fill_done,
                               NULL));
    CHECK(!mcme_filltask_start(wire, dir, 0, 0, 80, 0, NULL,
                               on_fill_done, NULL));
    CHECK(!mcme_filltask_start(wire, dir, 0, 0, 80, 0,
                               "minecraft:stone", NULL, NULL));
    mcme_fillresult_free(NULL);
    CHECK(g_fill_done_calls == 1); /* still: no stray posts */

    if (!bedrock) {
        printf("  SKIP golden world probes (tests/golden absent)\n");
        mcme_wire_free(wire);
        rm_rf(dir);
        return;
    }

    /* The store comes from the golden, copied whole — the writes
       land on the copy; the fixture itself is never opened for
       writing (the battery's own rule, kept). */
    cp_rf("tests/golden/worlds/bedrock", dir);

    /* a Java world (G17): the CLI's own fill road behind the same
       button — a chunk the region file does not have refuses the
       whole fill in the file's own words, and the write at a cell
       the world has lands and reads back the same way Bedrock's
       does. The golden is copied whole: the fixture is never
       opened for writing (the battery's rule, kept). */
    if (mcme_fs_exists("tests/golden/worlds/java")) {
        char jdir[] = "/tmp/mcme_gui_jfill_XXXXXX";

        CHECK(mkdtemp(jdir) != NULL);
        cp_rf("tests/golden/worlds/java", jdir);

        /* (40, 5, 5) is a chunk the region file does not hold:
           refused with nothing written anywhere */
        g_fill_done_calls = 0;
        g_fill_done_ok = true;
        CHECK(mcme_filltask_start(wire, jdir, 0, 40, 5, 5,
                                  "minecraft:stone", on_fill_done,
                                  NULL));
        while (g_fill_done_calls == 0)
            g_main_context_iteration(NULL, TRUE);
        CHECK(g_fill_done_calls == 1);
        CHECK(!g_fill_done_ok);
        CHECK(strstr(g_fill_done_text, "is not stored") != NULL);
        mcme_clear_error();

        /* the write itself: the cell lands in the chunk the world
           has — and the store's own read says the block's name
           back (the proof is the re-read, the GUI label's rule
           whole) */
        g_fill_done_calls = 0;
        g_fill_done_ok = false;
        CHECK(mcme_filltask_start(wire, jdir, 0, 5, 5, 5,
                                  "minecraft:stone", on_fill_done,
                                  NULL));
        while (g_fill_done_calls == 0)
            g_main_context_iteration(NULL, TRUE);
        CHECK(g_fill_done_calls == 1);
        CHECK(g_fill_done_ok);
        CHECK(mcme_world_probe(&info, jdir));
        CHECK(info.kind == MCME_WORLD_JAVA);
        mcme_buffer_init(&spec);
        CHECK(mcme_world_block_spec(&info, 0, 5, 5, 5, &spec)
              == MCME_WORLDBLOCK_OK);
        CHECK(spec.len == strlen("minecraft:stone"));
        CHECK(spec.data != NULL &&
              memcmp(spec.data, "minecraft:stone", spec.len) == 0);
        mcme_buffer_free(&spec);
        mcme_world_info_free(&info);
        rm_rf(jdir);
    }

    /* a block spec the verbs' parser refuses — past the probe, so
       the parser's own words and not the directory's; nothing is
       written */
    g_fill_done_calls = 0;
    g_fill_done_ok = true;
    CHECK(mcme_filltask_start(wire, dir, 0, 0, 80, 0, "",
                              on_fill_done, NULL));
    while (g_fill_done_calls == 0)
        g_main_context_iteration(NULL, TRUE);
    CHECK(g_fill_done_calls == 1);
    CHECK(!g_fill_done_ok);
    CHECK(g_fill_done_text[0] != '\0');
    CHECK(strcmp(g_fill_done_text,
                 "that directory is not a world mcme can "
                 "write") != 0);
    mcme_clear_error();

    /* a sub-chunk this world deleted: refused at the write door,
       the door's own words — the golden's origin column carries
       the deletion markers, so (0, 80, 0) stands for every
       region a prune left behind */
    g_fill_done_calls = 0;
    g_fill_done_ok = true;
    CHECK(mcme_filltask_start(wire, dir, 0, 0, 80, 0,
                              "minecraft:stone", on_fill_done, NULL));
    while (g_fill_done_calls == 0)
        g_main_context_iteration(NULL, TRUE);
    CHECK(g_fill_done_calls == 1);
    CHECK(!g_fill_done_ok);
    CHECK(strstr(g_fill_done_text,
                 "is deleted in this world") != NULL);
    mcme_clear_error();

    /* the write itself: the cell lands in the store — and the
       store's own read says the block's name back (the proof is
       the re-read, the GUI label's rule whole). The coordinate is
       a column the fixture has never stored: the subchunk comes
       into being the reference's way and is then edited. */
    g_fill_done_calls = 0;
    g_fill_done_ok = false;
    CHECK(mcme_filltask_start(wire, dir, 0, 5000, 20, 5000,
                              "minecraft:stone", on_fill_done, NULL));
    while (g_fill_done_calls == 0)
        g_main_context_iteration(NULL, TRUE);
    CHECK(g_fill_done_calls == 1);
    CHECK(g_fill_done_ok);
    CHECK(mcme_world_probe(&info, dir));
    CHECK(info.kind == MCME_WORLD_BEDROCK);
    mcme_buffer_init(&spec);
    CHECK(mcme_world_block_spec(&info, 0, 5000, 20, 5000, &spec)
          == MCME_WORLDBLOCK_OK);
    CHECK(spec.len == strlen("minecraft:stone"));
    CHECK(spec.data != NULL &&
          memcmp(spec.data, "minecraft:stone", spec.len) == 0);
    mcme_buffer_free(&spec);
    mcme_world_info_free(&info);

    /* coordinates the key cannot name: refused before the store
       is ever opened */
    g_fill_done_calls = 0;
    g_fill_done_ok = true;
    CHECK(mcme_filltask_start(wire, dir, 0, 1L << 40, 80, 0,
                              "minecraft:stone", on_fill_done, NULL));
    while (g_fill_done_calls == 0)
        g_main_context_iteration(NULL, TRUE);
    CHECK(g_fill_done_calls == 1);
    CHECK(!g_fill_done_ok);
    CHECK(strcmp(g_fill_done_text,
                 "the fill's coordinates are outside the range a "
                 "key can name") == 0);
    mcme_clear_error();

    mcme_wire_free(wire);
    rm_rf(dir);
}

/* the prune worker's answers through the wire (G18) */
static int g_prune_done_calls;
static bool g_prune_done_ok;
static int64_t g_prune_done_chunks, g_prune_done_keys,
    g_prune_done_files;
static char g_prune_done_text[512];

static void on_prune_done(void *userdata, McmePruneResult *result)
{
    (void)userdata;
    g_prune_done_calls++;
    g_prune_done_ok = result->ok;
    g_prune_done_chunks = result->chunks;
    g_prune_done_keys = result->keys;
    g_prune_done_files = result->region_files;
    snprintf(g_prune_done_text, sizeof g_prune_done_text, "%s",
             result->text ? result->text : "");
    mcme_pruneresult_free(result);
}

/* The lane's second writer (G18), and the destructive one: the
   tapped cell's whole chunk column out of the store, through the
   wire — the probe's own words when the directory is not a world,
   the column's own range, the empty selection's silent success
   (prune.h's contract: ok, all fields zero, nothing written), and
   the delete itself proven by the read that fails after it — the
   re-read rule inverted: the world's own answer says the chunk is
   gone. Both halves prune (G18): Bedrock tombstones the column's
   keys, Java zeroes the slot and counts in the region files it
   punched. */
static void test_prunetask(void)
{
    char dir[] = "/tmp/mcme_gui_prune_XXXXXX";
    McmeWire *wire;
    McmeWorldInfo info;
    McmeBuffer spec;
    bool bedrock = mcme_fs_exists("tests/golden/worlds/bedrock");

    printf("  prunetask (one chunk column out of a store, gone by "
           "the read)\n");

    CHECK(mkdtemp(dir) != NULL);

    wire = mcme_wire_new();

    /* the guards: nothing starts, nothing posts */
    CHECK(!mcme_prunetask_start(NULL, dir, 0, 0, 0, on_prune_done,
                                NULL));
    CHECK(!mcme_prunetask_start(wire, NULL, 0, 0, 0, on_prune_done,
                                NULL));
    CHECK(!mcme_prunetask_start(wire, dir, 0, 0, 0, NULL, NULL));
    mcme_pruneresult_free(NULL);
    CHECK(g_prune_done_calls == 0);

    /* a directory that is not a world: refused in the probe's own
       words, and nothing is created in its place */
    g_prune_done_calls = 0;
    g_prune_done_ok = true;
    CHECK(mcme_prunetask_start(wire, "/tmp/mcme_gui_no_world_dir",
                               0, 0, 0, on_prune_done, NULL));
    while (g_prune_done_calls == 0)
        g_main_context_iteration(NULL, TRUE);
    CHECK(g_prune_done_calls == 1);
    CHECK(!g_prune_done_ok);
    CHECK(g_prune_done_text[0] != '\0');
    CHECK(!mcme_fs_exists("/tmp/mcme_gui_no_world_dir/db"));
    mcme_clear_error();

    /* coordinates a column cannot name: refused before any store
       opens */
    g_prune_done_calls = 0;
    g_prune_done_ok = true;
    CHECK(mcme_prunetask_start(wire, dir, 0, 1L << 40, 0,
                               on_prune_done, NULL));
    while (g_prune_done_calls == 0)
        g_main_context_iteration(NULL, TRUE);
    CHECK(g_prune_done_calls == 1);
    CHECK(!g_prune_done_ok);
    CHECK(strstr(g_prune_done_text, "outside the range") != NULL);
    mcme_clear_error();

    if (!bedrock) {
        printf("  SKIP golden world probes (tests/golden absent)\n");
        mcme_wire_free(wire);
        rm_rf(dir);
        return;
    }

    /* The store comes from the golden, copied whole — the delete
       lands on the copy; the fixture itself is never opened for
       writing (the battery's own rule, kept). */
    cp_rf("tests/golden/worlds/bedrock", dir);

    /* the write-then-prune-then-read story: the cell's chunk
       written (the fill road's own proof, G15) and read back,
       then the column pruned — and the store's own read now says
       the cell is not there. The delete proves itself the way the
       write did: by being read back, the world's own answer. */
    g_fill_done_calls = 0;
    g_fill_done_ok = false;
    CHECK(mcme_filltask_start(wire, dir, 0, 5000, 20, 5000,
                              "minecraft:stone", on_fill_done,
                              NULL));
    while (g_fill_done_calls == 0)
        g_main_context_iteration(NULL, TRUE);
    CHECK(g_fill_done_calls == 1);
    CHECK(g_fill_done_ok);
    CHECK(mcme_world_probe(&info, dir));
    CHECK(info.kind == MCME_WORLD_BEDROCK);
    mcme_buffer_init(&spec);
    CHECK(mcme_world_block_spec(&info, 0, 5000, 20, 5000, &spec)
          == MCME_WORLDBLOCK_OK);
    CHECK(spec.len == strlen("minecraft:stone"));
    mcme_buffer_free(&spec);
    mcme_world_info_free(&info);

    g_prune_done_calls = 0;
    g_prune_done_ok = false;
    CHECK(mcme_prunetask_start(wire, dir, 0, 5000, 5000,
                               on_prune_done, NULL));
    while (g_prune_done_calls == 0)
        g_main_context_iteration(NULL, TRUE);
    CHECK(g_prune_done_calls == 1);
    CHECK(g_prune_done_ok);
    CHECK(g_prune_done_chunks == 1);
    CHECK(g_prune_done_keys > 0);   /* Bedrock tombstones keys */
    CHECK(g_prune_done_files == 0); /* a field it does not have */

    CHECK(mcme_world_probe(&info, dir));
    mcme_buffer_init(&spec);
    CHECK(mcme_world_block_spec(&info, 0, 5000, 20, 5000, &spec)
          == MCME_WORLDBLOCK_NOT_FOUND);
    mcme_buffer_free(&spec);
    mcme_world_info_free(&info);
    mcme_clear_error();

    /* the empty selection: the same box run twice — the second
       prune finds nothing left to delete and writes nothing, ok
       with every field zero (prune.h's own contract, said
       plain) */
    g_prune_done_calls = 0;
    g_prune_done_ok = false;
    g_prune_done_chunks = -1;
    CHECK(mcme_prunetask_start(wire, dir, 0, 5000, 5000,
                               on_prune_done, NULL));
    while (g_prune_done_calls == 0)
        g_main_context_iteration(NULL, TRUE);
    CHECK(g_prune_done_calls == 1);
    CHECK(g_prune_done_ok);
    CHECK(g_prune_done_chunks == 0);
    CHECK(g_prune_done_keys == 0);
    CHECK(g_prune_done_files == 0);

    /* the other half (G18): Java's chunk column, counted in the
       region files it punched — and the cell's read says the
       chunk is gone the same way. */
    if (mcme_fs_exists("tests/golden/worlds/java")) {
        char jdir[] = "/tmp/mcme_gui_jprune_XXXXXX";

        CHECK(mkdtemp(jdir) != NULL);
        cp_rf("tests/golden/worlds/java", jdir);

        CHECK(mcme_world_probe(&info, jdir));
        CHECK(info.kind == MCME_WORLD_JAVA);
        mcme_buffer_init(&spec);
        CHECK(mcme_world_block_spec(&info, 0, 5, 5, 5, &spec)
              != MCME_WORLDBLOCK_NOT_FOUND); /* the chunk is there */
        mcme_buffer_free(&spec);
        mcme_world_info_free(&info);
        mcme_clear_error();

        g_prune_done_calls = 0;
        g_prune_done_ok = false;
        CHECK(mcme_prunetask_start(wire, jdir, 0, 5, 5,
                                   on_prune_done, NULL));
        while (g_prune_done_calls == 0)
            g_main_context_iteration(NULL, TRUE);
        CHECK(g_prune_done_calls == 1);
        CHECK(g_prune_done_ok);
        CHECK(g_prune_done_chunks == 1);
        CHECK(g_prune_done_keys == 0);  /* a field it does not
                                           have */
        CHECK(g_prune_done_files == 1);

        CHECK(mcme_world_probe(&info, jdir));
        mcme_buffer_init(&spec);
        CHECK(mcme_world_block_spec(&info, 0, 5, 5, 5, &spec)
              == MCME_WORLDBLOCK_NOT_FOUND);
        mcme_buffer_free(&spec);
        mcme_world_info_free(&info);
        mcme_clear_error();
        rm_rf(jdir);
    }

    mcme_wire_free(wire);
    rm_rf(dir);
}

/* the convert worker's answers through the wire (G19) */
static int g_convert_done_calls;
static bool g_convert_done_ok;
static char g_convert_done_text[512];
static char g_convert_done_out[512];
static McmeConvertReport g_convert_done_report;

static void on_convert_done(void *userdata, McmeConvertResult *result)
{
    (void)userdata;
    g_convert_done_calls++;
    g_convert_done_ok = result->ok;
    snprintf(g_convert_done_text, sizeof g_convert_done_text, "%s",
             result->text ? result->text : "");
    snprintf(g_convert_done_out, sizeof g_convert_done_out, "%s",
             result->out_dir ? result->out_dir : "");
    g_convert_done_report = result->report; /* the whole report, copied */
    mcme_convertresult_free(result);
}

/* The creating writer (G19): a whole world beside itself, through
   the wire — the probe's words for a directory that is not a world,
   cmd_convert's own verdict sentence for Java input (nothing
   written beside it), the derived output name, and the conversion
   proven the fill's way: the cell written before the run reads
   back as exactly its block from the OUTPUT world through
   mcme_world_block_spec — the write carried through the translate
   map, D62 across the border — the drops sentence matching the
   report's own fields, and the second run refusing whole with the
   first output standing (an output world is finished or absent,
   convert.h, D12). */
static void test_converttask(void)
{
    char dir[] = "/tmp/mcme_gui_convert_XXXXXX";
    char out[600];
    McmeWire *wire;
    McmeWorldInfo info;
    McmeBuffer spec;
    char drops[600];
    bool any_drop;
    bool bedrock = mcme_fs_exists("tests/golden/worlds/bedrock");

    printf("  converttask (a world beside itself, read back from "
           "the output)\n");

    CHECK(mkdtemp(dir) != NULL);

    wire = mcme_wire_new();

    /* the guards: nothing starts, nothing posts */
    CHECK(!mcme_converttask_start(NULL, dir, on_convert_done, NULL));
    CHECK(!mcme_converttask_start(wire, NULL, on_convert_done, NULL));
    CHECK(!mcme_converttask_start(wire, dir, NULL, NULL));
    mcme_convertresult_free(NULL);
    CHECK(g_convert_done_calls == 0);

    /* a directory that is not a world: the probe's own words, and
       no <dir>_java appears beside it */
    g_convert_done_calls = 0;
    g_convert_done_ok = true;
    CHECK(mcme_converttask_start(wire, "/tmp/mcme_gui_no_world_dir",
                                 on_convert_done, NULL));
    while (g_convert_done_calls == 0)
        g_main_context_iteration(NULL, TRUE);
    CHECK(g_convert_done_calls == 1);
    CHECK(!g_convert_done_ok);
    CHECK(g_convert_done_text[0] != '\0');
    CHECK(!mcme_fs_exists("/tmp/mcme_gui_no_world_dir_java"));
    mcme_clear_error();

    /* the drops sentence's truth rule: an all-zero report says
       nothing (the CLI prints no line either — one function now,
       one behavior) */
    {
        McmeConvertReport zero;

        memset(&zero, 0, sizeof zero);
        drops[0] = 'x';
        CHECK(mcme_convert_drops_text(&zero, drops, sizeof drops)
              == 0);
        CHECK(drops[0] == '\0');
        CHECK(mcme_convert_drops_text(&zero, NULL, 0) == 0);
    }

    if (!bedrock) {
        printf("  SKIP golden world probes (tests/golden absent)\n");
        mcme_wire_free(wire);
        rm_rf(dir);
        return;
    }

    /* a Java input: cmd_convert's verdict in cmd_convert's words,
       and nothing written beside it */
    if (mcme_fs_exists("tests/golden/worlds/java")) {
        char jdir[] = "/tmp/mcme_gui_jconvert_XXXXXX";
        char jout[600];

        CHECK(mkdtemp(jdir) != NULL);
        cp_rf("tests/golden/worlds/java", jdir);
        snprintf(jout, sizeof jout, "%s_java", jdir);

        g_convert_done_calls = 0;
        g_convert_done_ok = true;
        CHECK(mcme_converttask_start(wire, jdir, on_convert_done,
                                     NULL));
        while (g_convert_done_calls == 0)
            g_main_context_iteration(NULL, TRUE);
        CHECK(g_convert_done_calls == 1);
        CHECK(!g_convert_done_ok);
        CHECK(strstr(g_convert_done_text, "is a java world") != NULL);
        CHECK(strstr(g_convert_done_text,
                     "only Bedrock input converts today") != NULL);
        CHECK(!mcme_fs_exists(jout));
        mcme_clear_error();
        rm_rf(jdir);
    }

    /* The store comes from the golden, copied whole — the
       conversion lands on the copy; the fixture itself is never
       opened for writing (the battery's own rule, kept). */
    cp_rf("tests/golden/worlds/bedrock", dir);
    snprintf(out, sizeof out, "%s_java", dir);

    /* the write before the border: the cell lands the G15 road's
       way, so the conversion has something to carry */
    g_fill_done_calls = 0;
    g_fill_done_ok = false;
    CHECK(mcme_filltask_start(wire, dir, 0, 5000, 20, 5000,
                              "minecraft:stone", on_fill_done, NULL));
    while (g_fill_done_calls == 0)
        g_main_context_iteration(NULL, TRUE);
    CHECK(g_fill_done_calls == 1);
    CHECK(g_fill_done_ok);

    g_convert_done_calls = 0;
    g_convert_done_ok = false;
    CHECK(mcme_converttask_start(wire, dir, on_convert_done, NULL));
    while (g_convert_done_calls == 0)
        g_main_context_iteration(NULL, TRUE);
    CHECK(g_convert_done_calls == 1);
    CHECK(g_convert_done_ok);
    CHECK(strcmp(g_convert_done_out, out) == 0); /* the derived name */
    CHECK(g_convert_done_report.chunks > 0);
    CHECK(g_convert_done_report.blocks > 0);

    /* the drops sentence matches the report's own fields: some
       count nonzero exactly when the sentence speaks */
    any_drop = g_convert_done_report.legacy_subchunks > 0 ||
               g_convert_done_report.water_positions > 0 ||
               g_convert_done_report.biomes > 0 ||
               g_convert_done_report.heightmaps > 0 ||
               g_convert_done_report.block_entities > 0 ||
               g_convert_done_report.items > 0 ||
               g_convert_done_report.ticks > 0 ||
               g_convert_done_report.entities > 0 ||
               g_convert_done_report.actors > 0 ||
               g_convert_done_report.chunk_data > 0 ||
               g_convert_done_report.other > 0;
    drops[0] = 'x';
    CHECK((mcme_convert_drops_text(&g_convert_done_report, drops,
                                   sizeof drops) > 0) == any_drop);
    if (any_drop)
        CHECK(strncmp(drops, "dropped: ", 9) == 0);

    /* the output is a world of the other kind, and it holds the
       cell — read through the store's own spec road, not the
       converter's claim */
    CHECK(mcme_world_probe(&info, out));
    CHECK(info.kind == MCME_WORLD_JAVA);
    mcme_buffer_init(&spec);
    CHECK(mcme_world_block_spec(&info, 0, 5000, 20, 5000, &spec)
          == MCME_WORLDBLOCK_OK);
    CHECK(spec.len == strlen("minecraft:stone"));
    CHECK(spec.data != NULL &&
          memcmp(spec.data, "minecraft:stone", spec.len) == 0);
    mcme_buffer_free(&spec);
    mcme_world_info_free(&info);

    /* the second run: refused whole — the output exists — and the
       first output stands, untouched */
    g_convert_done_calls = 0;
    g_convert_done_ok = true;
    CHECK(mcme_converttask_start(wire, dir, on_convert_done, NULL));
    while (g_convert_done_calls == 0)
        g_main_context_iteration(NULL, TRUE);
    CHECK(g_convert_done_calls == 1);
    CHECK(!g_convert_done_ok);
    CHECK(g_convert_done_text[0] != '\0');
    CHECK(mcme_world_probe(&info, out));
    CHECK(info.kind == MCME_WORLD_JAVA);
    mcme_world_info_free(&info);
    mcme_clear_error();

    mcme_wire_free(wire);
    rm_rf(out);
    rm_rf(dir);
}

/* ===== statstask results ===== */
static int g_stats_done_calls;
static bool g_stats_done_ok;
static char g_stats_done_text[512];
static char g_stats_done_name[256];
static McmeWorldKind g_stats_done_kind;
static size_t g_stats_done_keys;
static size_t g_stats_done_chunks;
static size_t g_stats_done_regions;
static size_t g_stats_done_dims;
static McmeDimensionCount g_stats_done_rows[8];

static void on_stats_done(void *userdata, McmeStatsResult *result)
{
    (void)userdata;
    g_stats_done_calls++;
    g_stats_done_ok = result->ok;
    snprintf(g_stats_done_text, sizeof g_stats_done_text, "%s",
             result->text ? result->text : "");
    snprintf(g_stats_done_name, sizeof g_stats_done_name, "%s",
             result->name ? result->name : "");
    g_stats_done_kind = result->kind;
    g_stats_done_keys = result->keys;
    g_stats_done_chunks = result->chunks;
    g_stats_done_regions = result->region_files;
    g_stats_done_dims = result->dims;
    for (size_t i = 0; i < result->dims && i < 8; ++i)
        g_stats_done_rows[i] = result->by_dim[i];
    mcme_statsresult_free(result);
}

/* The reader (G21): what the door counts, through the wire — the
   probe's words for a directory that is not a world, cmd_info's own
   line for one that probes but holds none, both goldens' fixed
   inventory walked through mcme_bedrock_inventory (8 keys/3 chunks
   overworld+nether for the store) and mcme_java_inventory (2
   regions/5 chunks for java — and, on G22's built fixture, the
   DIM-1/ tree counted as a second dimension), and the arc that ties
   the doors together: a fill
   makes the input's own count grow (5→6 — the door sees what the
   writers did), the convert carries the new cell (report 1 chunk),
   and the count of its OUTPUT equals the report — the writer's
   claim and the reader's count of the product, one number two
   roads. */
static void test_statstask(void)
{
    char dir[] = "/tmp/mcme_gui_stats_XXXXXX";
    char out[600];
    McmeWire *wire;
    McmeConvertReport report;
    bool bedrock = mcme_fs_exists("tests/golden/worlds/bedrock");
    bool chunks_world =
        mcme_fs_exists("tests/golden/worlds/bedrock-chunks");

    printf("  statstask (the world counted, and two counts agreeing "
           "across the border)\n");

    CHECK(mkdtemp(dir) != NULL);

    wire = mcme_wire_new();

    /* the guards: nothing starts, nothing posts */
    CHECK(!mcme_statstask_start(NULL, dir, on_stats_done, NULL));
    CHECK(!mcme_statstask_start(wire, NULL, on_stats_done, NULL));
    CHECK(!mcme_statstask_start(wire, dir, NULL, NULL));
    mcme_statsresult_free(NULL);
    CHECK(g_stats_done_calls == 0);

    /* a directory that is not a world: the probe's own words, and
       no counts counted */
    g_stats_done_calls = 0;
    g_stats_done_ok = true;
    CHECK(mcme_statstask_start(wire, "/tmp/mcme_gui_no_world_dir",
                               on_stats_done, NULL));
    while (g_stats_done_calls == 0)
        g_main_context_iteration(NULL, TRUE);
    CHECK(g_stats_done_calls == 1);
    CHECK(!g_stats_done_ok);
    CHECK(g_stats_done_text[0] != '\0');
    mcme_clear_error();

    /* an empty directory probes but holds no world: cmd_info's own
       line — the CLI's info is where this door's truth lives, so
       the door speaks the verb's sentence */
    {
        char empty[] = "/tmp/mcme_gui_stats_empty_XXXXXX";

        CHECK(mkdtemp(empty) != NULL);
        g_stats_done_calls = 0;
        g_stats_done_ok = true;
        CHECK(mcme_statstask_start(wire, empty, on_stats_done, NULL));
        while (g_stats_done_calls == 0)
            g_main_context_iteration(NULL, TRUE);
        CHECK(g_stats_done_calls == 1);
        CHECK(!g_stats_done_ok);
        CHECK(strstr(g_stats_done_text, "Not a Minecraft world")
              != NULL);
        CHECK(strstr(g_stats_done_text, "session.lock") != NULL);
        rm_rf(empty);
    }

    if (!bedrock) {
        printf("  SKIP golden world counts (tests/golden absent)\n");
        mcme_wire_free(wire);
        rm_rf(dir);
        return;
    }

    /* the store's fixed inventory: 8 keys, 3 chunks, the two
       dimensions the walk found — the numbers the CLI's `chunks`
       prints from this same engine, walked here through the door */
    if (chunks_world) {
        char cdir[] = "/tmp/mcme_gui_stats_chunks_XXXXXX";

        CHECK(mkdtemp(cdir) != NULL);
        cp_rf("tests/golden/worlds/bedrock-chunks", cdir);
        g_stats_done_calls = 0;
        g_stats_done_ok = false;
        CHECK(mcme_statstask_start(wire, cdir, on_stats_done, NULL));
        while (g_stats_done_calls == 0)
            g_main_context_iteration(NULL, TRUE);
        CHECK(g_stats_done_calls == 1);
        CHECK(g_stats_done_ok);
        CHECK(g_stats_done_kind == MCME_WORLD_BEDROCK);
        CHECK(g_stats_done_name[0] == '\0'); /* no level.dat, no name */
        CHECK(g_stats_done_keys == 8);
        CHECK(g_stats_done_chunks == 3);
        CHECK(g_stats_done_dims == 2);
        CHECK(g_stats_done_rows[0].dimension == 0);
        CHECK(g_stats_done_rows[0].chunks == 2);
        CHECK(g_stats_done_rows[1].dimension == 1);
        CHECK(g_stats_done_rows[1].chunks == 1);
        rm_rf(cdir);
    }

    /* java: the inventory's own totals (G22 — every dimension tree,
       through the same scanner the probe's leg rode; for this
       overworld-only golden, what `info` has always printed) and the
       name the level.dat holds — one row now, the overworld's */
    if (mcme_fs_exists("tests/golden/worlds/java")) {
        char jdir[] = "/tmp/mcme_gui_stats_java_XXXXXX";

        CHECK(mkdtemp(jdir) != NULL);
        cp_rf("tests/golden/worlds/java", jdir);
        g_stats_done_calls = 0;
        g_stats_done_ok = false;
        CHECK(mcme_statstask_start(wire, jdir, on_stats_done, NULL));
        while (g_stats_done_calls == 0)
            g_main_context_iteration(NULL, TRUE);
        CHECK(g_stats_done_calls == 1);
        CHECK(g_stats_done_ok);
        CHECK(g_stats_done_kind == MCME_WORLD_JAVA);
        CHECK(strcmp(g_stats_done_name, "World Created By Amulet")
              == 0);
        CHECK(g_stats_done_regions == 2);
        CHECK(g_stats_done_chunks == 5);
        CHECK(g_stats_done_dims == 1);
        CHECK(g_stats_done_rows[0].dimension == 0);
        CHECK(g_stats_done_rows[0].chunks == 5);
        rm_rf(jdir);
    }

    /* java WITH a nether (G22's own fixture — the goldens ship no
       java dimension beyond the overworld, so this one is built: the
       golden with r.0.0.mca copied into DIM-1/region/). Measured
       through the CLI before these pins were written: 3 regions,
       9 chunks, rows overworld 5 + nether 4 — the second tree
       counted as a dimension, its name spelled by the vocabulary. */
    if (mcme_fs_exists("tests/golden/worlds/java")) {
        char ndir[] = "/tmp/mcme_gui_stats_nether_XXXXXX";
        char dim1[512], to[600];
        gchar *data = NULL;
        gsize len = 0;

        CHECK(mkdtemp(ndir) != NULL);
        cp_rf("tests/golden/worlds/java", ndir);
        snprintf(dim1, sizeof dim1, "%s/DIM-1/region", ndir);
        snprintf(to, sizeof to, "%s/r.0.0.mca", dim1);
        CHECK(g_mkdir_with_parents(dim1, 0755) == 0);
        CHECK(g_file_get_contents("tests/golden/worlds/java/region/r.0.0.mca",
                                  &data, &len, NULL));
        CHECK(g_file_set_contents(to, data, len, NULL));
        g_free(data);

        g_stats_done_calls = 0;
        g_stats_done_ok = false;
        CHECK(mcme_statstask_start(wire, ndir, on_stats_done, NULL));
        while (g_stats_done_calls == 0)
            g_main_context_iteration(NULL, TRUE);
        CHECK(g_stats_done_calls == 1);
        CHECK(g_stats_done_ok);
        CHECK(g_stats_done_kind == MCME_WORLD_JAVA);
        CHECK(g_stats_done_regions == 3);
        CHECK(g_stats_done_chunks == 9);
        CHECK(g_stats_done_dims == 2);
        CHECK(g_stats_done_rows[0].dimension == 0);
        CHECK(g_stats_done_rows[0].chunks == 5);
        CHECK(g_stats_done_rows[1].dimension == 1);
        CHECK(g_stats_done_rows[1].chunks == 4);
        rm_rf(ndir);
    }

    /* The arc (a copy whole, the golden never opened for writing —
       the battery's own rule, kept): the door counts the pristine
       input, sees the fill grow it, and counts the convert's
       product against the convert's own report. */
    cp_rf("tests/golden/worlds/bedrock", dir);
    snprintf(out, sizeof out, "%s_java", dir);

    g_stats_done_calls = 0;
    g_stats_done_ok = false;
    CHECK(mcme_statstask_start(wire, dir, on_stats_done, NULL));
    while (g_stats_done_calls == 0)
        g_main_context_iteration(NULL, TRUE);
    CHECK(g_stats_done_calls == 1);
    CHECK(g_stats_done_ok);
    CHECK(g_stats_done_kind == MCME_WORLD_BEDROCK);
    CHECK(g_stats_done_keys == 15);
    CHECK(g_stats_done_chunks == 5);
    CHECK(g_stats_done_dims == 1);
    CHECK(g_stats_done_rows[0].dimension == 0);
    CHECK(g_stats_done_rows[0].chunks == 5);

    /* the fill through the wire (G15's own road): one cell, one
       chunk the door did not see before — 6 now, and one more key */
    g_fill_done_calls = 0;
    g_fill_done_ok = false;
    CHECK(mcme_filltask_start(wire, dir, 0, 5000, 20, 5000,
                              "minecraft:stone", on_fill_done, NULL));
    while (g_fill_done_calls == 0)
        g_main_context_iteration(NULL, TRUE);
    CHECK(g_fill_done_calls == 1);
    CHECK(g_fill_done_ok);

    g_stats_done_calls = 0;
    g_stats_done_ok = false;
    CHECK(mcme_statstask_start(wire, dir, on_stats_done, NULL));
    while (g_stats_done_calls == 0)
        g_main_context_iteration(NULL, TRUE);
    CHECK(g_stats_done_calls == 1);
    CHECK(g_stats_done_ok);
    CHECK(g_stats_done_keys == 16);
    CHECK(g_stats_done_chunks == 6);
    CHECK(g_stats_done_rows[0].chunks == 6);

    /* the convert (the core's own call — its task road is
       converttask's own test): the filled cell is the terrain this
       input has, so the report counts it and the output holds it */
    memset(&report, 0, sizeof report);
    CHECK(mcme_convert_bedrock_java(dir, out, &report, NULL));
    CHECK(report.chunks == 1);
    CHECK(report.blocks == 4096);

    /* THE cross-check (G21): the converter's claim and this door's
       count of its product, one number — 1 and 1, counted by two
       engines that never met */
    g_stats_done_calls = 0;
    g_stats_done_ok = false;
    CHECK(mcme_statstask_start(wire, out, on_stats_done, NULL));
    while (g_stats_done_calls == 0)
        g_main_context_iteration(NULL, TRUE);
    CHECK(g_stats_done_calls == 1);
    CHECK(g_stats_done_ok);
    CHECK(g_stats_done_kind == MCME_WORLD_JAVA);
    CHECK(g_stats_done_regions == 1);
    CHECK(g_stats_done_dims == 1); /* G22: the product's own row —
                                       one region, its one chunk */
    CHECK(g_stats_done_rows[0].dimension == 0);
    CHECK(g_stats_done_rows[0].chunks == 1);
    CHECK(g_stats_done_chunks == report.chunks);
    CHECK(g_stats_done_chunks == 1);

    mcme_wire_free(wire);
    rm_rf(out);
    rm_rf(dir);
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

/* out = a * b, column-major — the reference composition's own
   multiply, written here so the continuity pin below stands on the
   old fixed camera's arithmetic, not on the unit under test. */
static void m4_mul_t(float out[16], const float a[16], const float b[16])
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

/* G8: the hand's math, pinned headless beside the staging half.
   The continuity pin is the one that matters — the default camera's
   matrix is the fixed still's, element for element: the picture a
   pack opens to never moved, the drag just took it over. */
static void test_camera(void)
{
    McmeCamera cam;
    float m[16], ref[16];
    const float aspect = 16.0f / 9.0f;

    /* The still, composed the way the old fixed camera did it:
       project * tilt(0.42) * turn(0.6) * center-back(2.6). */
    {
        float p[16] = {0}, rx[16] = {0}, ry[16] = {0}, t[16] = {0};
        float tmp[16];
        const float f = 1.0f / tanf(0.45f);
        const float tilt = 0.42f, turn = 0.6f;
        const float near = 0.1f, far = 10.1f;

        p[0] = f / aspect;
        p[5] = f;
        p[10] = (far + near) / (near - far);
        p[11] = -1.0f;
        p[14] = (2.0f * far * near) / (near - far);
        rx[0] = 1.0f;
        rx[5] = cosf(tilt); rx[6] = sinf(tilt);
        rx[9] = -sinf(tilt); rx[10] = cosf(tilt); rx[15] = 1.0f;
        ry[0] = cosf(turn); ry[2] = -sinf(turn); ry[5] = 1.0f;
        ry[8] = sinf(turn); ry[10] = cosf(turn); ry[15] = 1.0f;
        t[0] = t[5] = t[10] = t[15] = 1.0f;
        t[12] = t[13] = -0.5f; t[14] = -2.6f;
        m4_mul_t(tmp, rx, ry);
        m4_mul_t(ref, p, tmp);
        m4_mul_t(ref, ref, t);
    }

    mcme_camera_default(&cam);
    CHECK(cam.yaw == 0.6f && cam.pitch == 0.42f && cam.dist == 2.6f);
    mcme_camera_mvp(&cam, aspect, m);
    for (int i = 0; i < 16; ++i)
        CHECK(fabsf(m[i] - ref[i]) < 1e-5f);

    /* NULL is the default camera — the total-function rule. */
    {
        float mn[16];

        mcme_camera_mvp(NULL, aspect, mn);
        for (int i = 0; i < 16; ++i)
            CHECK(mn[i] == m[i]);
    }

    /* The aspect rides: a wider window shrinks x's scale and the
       world keeps its size — f/aspect * the turn's first element
       is one constant across windows. */
    {
        float wide[16], tall[16];

        mcme_camera_mvp(&cam, 2.0f, wide);
        mcme_camera_mvp(&cam, 1.0f, tall);
        CHECK(fabsf(wide[0] * 2.0f - tall[0]) < 1e-5f);
    }

    /* The drag: yaw grows with dx at the documented slope, the
       other axis untouched. */
    mcme_camera_default(&cam);
    mcme_camera_drag(&cam, 100.0, 0.0);
    CHECK(fabsf(cam.yaw - 1.2f) < 1e-5f);
    CHECK(cam.pitch == 0.42f);

    /* The tilt clamps at the horizon either way — past ~80° the
       view inverts, which is confusion, not capability. */
    mcme_camera_drag(&cam, 0.0, 100000.0);
    CHECK(cam.pitch == 1.4f);
    mcme_camera_drag(&cam, 0.0, -200000.0);
    CHECK(cam.pitch == -1.4f);

    /* The walk: in shrinks, out grows, clamped at both ends and
       never through zero. */
    mcme_camera_default(&cam);
    mcme_camera_zoom(&cam, 3);
    CHECK(cam.dist < 2.6f && cam.dist >= 1.2f);
    mcme_camera_zoom(&cam, -6);
    CHECK(cam.dist > 2.6f && cam.dist <= 8.0f);
    mcme_camera_zoom(&cam, 10000);
    CHECK(cam.dist == 1.2f); /* walked in hard: the near clamp */
    mcme_camera_zoom(&cam, -1000000);
    CHECK(cam.dist == 8.0f); /* walked back hard: the far clamp */

    /* The hand changes the frame — and every extreme it can reach
       still answers a finite matrix (no NaN ever reaches the GPU). */
    {
        float before[16];
        int changed = 0;

        mcme_camera_default(&cam);
        mcme_camera_mvp(&cam, aspect, before);
        mcme_camera_drag(&cam, 80.0, 40.0);
        mcme_camera_zoom(&cam, 2);
        mcme_camera_mvp(&cam, aspect, m);
        for (int i = 0; i < 16; ++i)
            if (m[i] != before[i]) changed = 1;
        CHECK(changed);

        mcme_camera_drag(&cam, 0.0, 99999.0);
        mcme_camera_zoom(&cam, -99999);
        mcme_camera_mvp(&cam, aspect, m);
        for (int i = 0; i < 16; ++i)
            CHECK(isfinite(m[i]));
    }
}

/* The world bar's text half (G13): the parser the flags and the
 * entries share — one shape, one range, one refusal rule — and the
 * bar's whole question validated. The refusals are static
 * sentences: nothing here allocates, so the recoverable leak check
 * at the end stands over this test too. */
static void test_worldbar(void)
{
    long v;
    long x, y, z;
    int32_t dim;
    const char *why;

    printf("  worldbar (the parse both doors share, the bar's ask)\n");

    /* The parser: the whole token, inside int32 — strtol's own
       leading space and sign, nothing else (the CLI's
       parse_coord shape, G9). */
    CHECK(mcme_worldbar_coord("0", &v) && v == 0);
    CHECK(mcme_worldbar_coord("80", &v) && v == 80);
    CHECK(mcme_worldbar_coord("-5", &v) && v == -5);
    CHECK(mcme_worldbar_coord("+7", &v) && v == 7);
    CHECK(mcme_worldbar_coord(" 7", &v) && v == 7);
    CHECK(mcme_worldbar_coord("2147483647", &v) && v == 2147483647);
    CHECK(mcme_worldbar_coord("-2147483648", &v) && v == -2147483648L);
    CHECK(!mcme_worldbar_coord("2147483648", &v));
    CHECK(!mcme_worldbar_coord("-2147483649", &v));
    CHECK(!mcme_worldbar_coord("12abc", &v));
    CHECK(!mcme_worldbar_coord("7 ", &v));
    CHECK(!mcme_worldbar_coord("1.5", &v));
    CHECK(!mcme_worldbar_coord("", &v));
    CHECK(!mcme_worldbar_coord("abc", &v));
    CHECK(!mcme_worldbar_coord(NULL, &v));
    CHECK(!mcme_worldbar_coord("80", NULL));

    /* The bar's whole question: a world directory and the three
       coordinates, the dimension's number beside them. */
    why = NULL;
    CHECK(mcme_worldbar_ask("/tmp/w", "1", "2", "3", "0",
                            &x, &y, &z, &dim, &why));
    CHECK(x == 1 && y == 2 && z == 3 && dim == 0 && why == NULL);
    CHECK(mcme_worldbar_ask("/tmp/w", "-30000000", "255", "30000000",
                            "", &x, &y, &z, &dim, &why) &&
          x == -30000000 && y == 255 && z == 30000000 && dim == 0);
    CHECK(mcme_worldbar_ask("/tmp/w", "0", "0", "0", "-1",
                            &x, &y, &z, &dim, &why) && dim == -1);

    /* G20: the dimension spells its names — the CLI's vocabulary,
       one place in core, both doors reading it. */
    CHECK(mcme_worldbar_ask("/tmp/w", "1", "2", "3", "overworld",
                            &x, &y, &z, &dim, &why) && dim == 0);
    CHECK(mcme_worldbar_ask("/tmp/w", "1", "2", "3", "nether",
                            &x, &y, &z, &dim, &why) && dim == 1);
    CHECK(mcme_worldbar_ask("/tmp/w", "1", "2", "3", "end",
                            &x, &y, &z, &dim, &why) && dim == 2);
    CHECK(mcme_worldbar_ask("/tmp/w", "1", "2", "3", "7",
                            &x, &y, &z, &dim, &why) && dim == 7);
    CHECK(mcme_dimension_name(0) != NULL &&
          strcmp(mcme_dimension_name(0), "overworld") == 0);
    CHECK(mcme_dimension_name(1) != NULL &&
          strcmp(mcme_dimension_name(1), "nether") == 0);
    CHECK(mcme_dimension_name(2) != NULL &&
          strcmp(mcme_dimension_name(2), "end") == 0);
    CHECK(mcme_dimension_name(3) == NULL);

    /* The refusals: no world, a malformed coordinate or dimension,
       and NULL's own guards — every one static, none allocated. */
    CHECK(!mcme_worldbar_ask(NULL, "0", "0", "0", "0",
                             &x, &y, &z, &dim, &why) && why != NULL);
    CHECK(!mcme_worldbar_ask("", "0", "0", "0", "0",
                             &x, &y, &z, &dim, &why) && why != NULL);
    CHECK(!mcme_worldbar_ask("/tmp/w", "x", "0", "0", "0",
                             &x, &y, &z, &dim, &why) && why != NULL);
    CHECK(!mcme_worldbar_ask("/tmp/w", "0", "0.5", "0", "0",
                             &x, &y, &z, &dim, &why) && why != NULL);
    CHECK(!mcme_worldbar_ask("/tmp/w", "0", "0", "9999999999", "0",
                             &x, &y, &z, &dim, &why) && why != NULL);
    CHECK(!mcme_worldbar_ask("/tmp/w", "0", "0", "0", "the_nether",
                             &x, &y, &z, &dim, &why) && why != NULL);
    CHECK(!mcme_worldbar_ask("/tmp/w", "0", "0", "0", "1.5",
                             &x, &y, &z, &dim, &why) && why != NULL);
    /* G20: a name the vocabulary does not hold refuses in the
       core's own words — the one global's buffer, read the way
       every caller reads it (immediately). */
    CHECK(!mcme_worldbar_ask("/tmp/w", "0", "0", "0", "banana",
                             &x, &y, &z, &dim, &why) &&
          why != NULL && strstr(why, "not a dimension") != NULL);
    mcme_clear_error();
    CHECK(!mcme_worldbar_ask("/tmp/w", "0", "0", "0", "0",
                             NULL, &y, &z, &dim, &why));
    CHECK(!mcme_worldbar_ask("/tmp/w", "0", "0", "0", "0",
                             &x, &y, &z, &dim, NULL));
}

/* G14's point, headless: the matrix and the ray are the frame's
 * own numbers (the very composition viewport.c draws with), and
 * the cell rule is the builder's winding read back — the
 * reference's cube tables face outward (mesh.c), so a face's
 * shared coordinate is its plane and the cross product's sign is
 * the solid's side of it. Nothing allocates. */
static void test_pick(void)
{
    McmeCamera cam;
    float mvp[16], inv[16];
    long cell[3];

    printf("  pick (the point through the frame's matrix, the cell's rule)\n");

    /* The inverse the ray unprojects through: m · inv answers the
       identity element for element, or the adjugate is wrong. */
    mcme_camera_default(&cam);
    mcme_camera_mvp(&cam, 1.5f, mvp);
    CHECK(mcme_mat4_invert(mvp, inv));
    {
        bool ident = true;

        for (int c = 0; c < 4; ++c)
            for (int r = 0; r < 4; ++r) {
                float s = 0.0f;

                for (int k = 0; k < 4; ++k)
                    s += mvp[k * 4 + r] * inv[c * 4 + k];
                if (fabsf(s - (float)(c == r)) > 1e-4f)
                    ident = false;
            }
        CHECK(ident);
    }
    {
        float zero[16] = {0};

        CHECK(!mcme_mat4_invert(zero, inv));
        CHECK(!mcme_mat4_invert(NULL, inv));
    }

    /* The roundtrip: a known point projected by the frame's own
       matrix names its pixel, and the ray back through that pixel
       lies through the point — three points inside the block the
       still opens to. */
    {
        static const float pts[3][3] = {
            {0.5f, 0.5f, 0.0f}, {0.2f, 0.9f, 0.7f}, {0.9f, 0.1f, 0.3f}
        };

        for (int i = 0; i < 3; ++i) {
            float x = pts[i][0], y = pts[i][1], z = pts[i][2];
            float cx = mvp[0] * x + mvp[4] * y + mvp[8] * z +
                       mvp[12];
            float cy = mvp[1] * x + mvp[5] * y + mvp[9] * z +
                       mvp[13];
            float cw = mvp[3] * x + mvp[7] * y + mvp[11] * z +
                       mvp[15];
            float orig[3], dir[3];
            double px, py, dx, dy, dz, t, d2;

            CHECK(cw > 0.0f); /* in front of the eye */
            /* The frame's own ndc, whatever side of the picture it
               lands on — the default still sits the block's center
               column just past the left edge (px ≈ -11 at aspect
               1.5), and the sight-line math does not care. */
            px = (cx / cw + 1.0) * 0.5 * 640.0;
            py = (1.0 - cy / cw) * 0.5 * 480.0;
            CHECK(mcme_pick_ray(mvp, px, py, 640, 480, orig, dir));
            CHECK(fabsf(sqrtf(dir[0] * dir[0] + dir[1] * dir[1] +
                              dir[2] * dir[2]) - 1.0f) < 1e-5f);
            dx = x - orig[0];
            dy = y - orig[1];
            dz = z - orig[2];
            t = dx * dir[0] + dy * dir[1] + dz * dir[2];
            dx -= t * dir[0];
            dy -= t * dir[1];
            dz -= t * dir[2];
            d2 = dx * dx + dy * dy + dz * dz;
            CHECK(d2 < 1e-4); /* on the line, in world units */
        }
    }

    /* The center pixel's sight-line: camera.c's composition puts
       the translate's fixed point (0.5, 0.5, dist) on the view
       axis — every center ray's line passes through it. */
    {
        float orig[3], dir[3];
        double dx, dy, dz, t, d2;

        CHECK(mcme_pick_ray(mvp, 320.0, 240.0, 640, 480, orig, dir));
        dx = 0.5 - orig[0];
        dy = 0.5 - orig[1];
        dz = 2.6 - orig[2];
        t = dx * dir[0] + dy * dir[1] + dz * dir[2];
        dx -= t * dir[0];
        dy -= t * dir[1];
        dz -= t * dir[2];
        d2 = dx * dx + dy * dy + dz * dz;
        CHECK(d2 < 1e-6);
    }

    /* The cell rule over hand-laid quads, laid out as the stage
       writes them (stride 8, six indices a face). One face on the
       plane x = 1 wound outward +x — the reference's tables read
       outward through cross(v1-v0, v2-v0): the solid below the
       plane, so the cell answers ceil(1)-1 = 0. */
    {
        static const float quad_p[12] = {
            1, 0, 0, 1, 1, 0, 1, 1, 1, 1, 0, 1
        };
        float verts[4 * 8] = {0};
        uint32_t idx[6] = {0, 1, 2, 0, 2, 3};
        float orig[3], dir[3];

        for (int i = 0; i < 4; ++i)
            memcpy(verts + i * 8, quad_p + i * 3, 3 * sizeof(float));
        orig[0] = 4.0f;
        orig[1] = 0.5f;
        orig[2] = 0.5f;
        dir[0] = -1.0f;
        dir[1] = dir[2] = 0.0f;
        CHECK(mcme_pick_cell(verts, 4, idx, 6, orig, dir, 16, cell));
        CHECK(cell[0] == 0 && cell[1] == 0 && cell[2] == 0);
        /* The face's other half: a strike strictly inside the
           second triangle tri_face writes (0, 2, 3 — the stage's
           next three indices, a triple of their own). Reading them
           as corner arithmetic — (i, i+2, i+3) — repeated the
           face's first vertex and built a degenerate (0, 2, 0),
           skipped as zero-area. Every center strike falls on the
           diagonal both halves share, so only the stone-shell
           smoke's off-diagonal ray caught it. */
        orig[1] = 0.2f;
        orig[2] = 0.8f;
        CHECK(mcme_pick_cell(verts, 4, idx, 6, orig, dir, 16, cell));
        CHECK(cell[0] == 0 && cell[1] == 0 && cell[2] == 0);
    }

    /* The same face wound the other way reads inward: the solid
       above the plane, cell floor(1) = 1 — winding is the side. */
    {
        static const float quad_n[12] = {
            1, 0, 0, 1, 0, 1, 1, 1, 1, 1, 1, 0
        };
        float verts[4 * 8] = {0};
        uint32_t idx[6] = {0, 1, 2, 0, 2, 3};
        float orig[3], dir[3];

        for (int i = 0; i < 4; ++i)
            memcpy(verts + i * 8, quad_n + i * 3, 3 * sizeof(float));
        orig[0] = 4.0f;
        orig[1] = 0.5f;
        orig[2] = 0.5f;
        dir[0] = -1.0f;
        dir[1] = dir[2] = 0.0f;
        CHECK(mcme_pick_cell(verts, 4, idx, 6, orig, dir, 16, cell));
        CHECK(cell[0] == 1 && cell[1] == 0 && cell[2] == 0);
    }

    /* Nearest hit wins over the far face, and a face behind the
       origin never counts: from x = 4 the x = 3 face answers cell
       (2, 0, 0); from between, only the x = 1 face is ahead. */
    {
        static const float quad_p[12] = {
            1, 0, 0, 1, 1, 0, 1, 1, 1, 1, 0, 1
        };
        static const float quad_f[12] = {
            3, 0, 0, 3, 1, 0, 3, 1, 1, 3, 0, 1
        };
        float verts[8 * 8] = {0};
        uint32_t idx[12] = {0, 1, 2, 0, 2, 3,
                            4, 5, 6, 4, 6, 7};
        float orig[3], dir[3];

        for (int i = 0; i < 4; ++i) {
            memcpy(verts + i * 8, quad_p + i * 3, 3 * sizeof(float));
            memcpy(verts + (4 + i) * 8, quad_f + i * 3,
                   3 * sizeof(float));
        }
        orig[0] = 4.0f;
        orig[1] = orig[2] = 0.5f;
        dir[0] = -1.0f;
        dir[1] = dir[2] = 0.0f;
        CHECK(mcme_pick_cell(verts, 8, idx, 12, orig, dir, 16, cell));
        CHECK(cell[0] == 2 && cell[1] == 0 && cell[2] == 0);
        orig[0] = 2.5f;
        CHECK(mcme_pick_cell(verts, 8, idx, 12, orig, dir, 16, cell));
        CHECK(cell[0] == 0 && cell[1] == 0 && cell[2] == 0);
        orig[0] = 4.0f;
        dir[0] = 0.0f;
        dir[1] = 1.0f;
        CHECK(!mcme_pick_cell(verts, 8, idx, 12, orig, dir, 16,
                              cell)); /* beside, never crossed */
        orig[0] = 0.0f;
        dir[0] = -1.0f;
        dir[1] = 0.0f;
        CHECK(!mcme_pick_cell(verts, 8, idx, 12, orig, dir, 16,
                              cell)); /* both faces behind */
    }

    /* The frame's clamp: a face on the sub-chunk's far plane with
       the ray striking its very edge — the tangential x reads 16
       and clamps to the last cell, 15; the winding puts the z too
       at 15. */
    {
        static const float quad_e[12] = {
            15, 0, 16, 16, 0, 16, 16, 1, 16, 15, 1, 16
        };
        float verts[4 * 8] = {0};
        uint32_t idx[6] = {0, 1, 2, 0, 2, 3};
        float orig[3], dir[3];

        for (int i = 0; i < 4; ++i)
            memcpy(verts + i * 8, quad_e + i * 3, 3 * sizeof(float));
        orig[0] = 16.0f;
        orig[1] = 0.5f;
        orig[2] = 20.0f;
        dir[0] = dir[1] = 0.0f;
        dir[2] = -1.0f;
        CHECK(mcme_pick_cell(verts, 4, idx, 6, orig, dir, 16, cell));
        CHECK(cell[0] == 15 && cell[1] == 0 && cell[2] == 15);
    }

    /* NULL's own guards and the ragged stream: no vertex past the
       table, no face split mid-six, no frame to clamp into. */
    {
        static const float quad_p[12] = {
            1, 0, 0, 1, 1, 0, 1, 1, 1, 1, 0, 1
        };
        float verts[4 * 8] = {0};
        uint32_t idx[6] = {0, 1, 2, 0, 2, 3};
        uint32_t ragged[7] = {0, 1, 2, 0, 2, 3, 0};
        uint32_t past[6] = {0, 1, 2, 0, 2, 99};
        float orig[3] = {4, 0.5f, 0.5f}, dir[3] = {-1, 0, 0};

        for (int i = 0; i < 4; ++i)
            memcpy(verts + i * 8, quad_p + i * 3, 3 * sizeof(float));
        CHECK(!mcme_pick_cell(NULL, 4, idx, 6, orig, dir, 16, cell));
        CHECK(!mcme_pick_cell(verts, 4, NULL, 6, orig, dir, 16,
                              cell));
        CHECK(!mcme_pick_cell(verts, 4, ragged, 7, orig, dir, 16,
                              cell));
        CHECK(!mcme_pick_cell(verts, 4, past, 6, orig, dir, 16,
                              cell));
        CHECK(!mcme_pick_cell(verts, 4, idx, 6, orig, dir, 0, cell));
        CHECK(!mcme_pick_cell(verts, 4, idx, 6, orig, dir, 16, NULL));
    }

    /* The reference's own missing-block cube, through the stage the
       window uses: every one of the six faces, struck head-on from
       outside, answers the single cell it stands on — 0,0,0 with
       the sub-chunk's frame (the clamp is not what saves it; the
       winding and the plane are). */
    {
        McmeMesh cube;
        float *verts = NULL;
        uint32_t *idx = NULL;
        size_t nv = 0, ni = 0;
        static const float rays[6][6] = {
            {0.5f, -1.0f, 0.5f, 0.0f, 1.0f, 0.0f},
            {0.5f, 2.0f, 0.5f, 0.0f, -1.0f, 0.0f},
            {0.5f, 0.5f, -1.0f, 0.0f, 0.0f, 1.0f},
            {0.5f, 0.5f, 2.0f, 0.0f, 0.0f, -1.0f},
            {-1.0f, 0.5f, 0.5f, 1.0f, 0.0f, 0.0f},
            {2.0f, 0.5f, 0.5f, -1.0f, 0.0f, 0.0f}
        };

        CHECK(mcme_mesh_missing_block(&cube));
        CHECK(mcme_viewport_stage(&cube, NULL, NULL, &verts, &idx,
                                  &nv, &ni));
        for (int i = 0; i < 6; ++i) {
            CHECK(mcme_pick_cell(verts, nv, idx, ni, rays[i],
                                 rays[i] + 3, 16, cell));
            CHECK(cell[0] == 0 && cell[1] == 0 && cell[2] == 0);
        }
        free(verts); /* the stage's own contract: malloc'd, freed */
        free(idx);
        mcme_mesh_free(&cube);
    }
}

int mcme_gui_selftest(void)
{
    test_fifo();
    test_worker();
    test_repost();
    test_wake();
    test_packtask();
    test_meshtask();
    test_chunktask();
    test_filltask();
    test_prunetask();
    test_converttask();
    test_statstask();
    test_stage();
    test_camera();
    test_worldbar();
    test_pick();

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
