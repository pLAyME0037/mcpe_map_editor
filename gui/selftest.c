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
 *
 * The core queue (src/core/event_queue.c) has no glib in it and its own
 * tests can not see a main context; these checks stand where the two
 * halves meet — and where the GUI meets the core's readers — and cover
 * the queue along the way.
 */

#include "selftest.h"

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include <glib.h>
#include <sanitizer/lsan_interface.h>

#include "mcme/core.h"
#include "packtask.h"
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

int mcme_gui_selftest(void)
{
    test_fifo();
    test_worker();
    test_repost();
    test_wake();
    test_packtask();

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
