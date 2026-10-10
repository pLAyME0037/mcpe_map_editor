#include "mcme/core.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"

/*
 * Core self-tests.
 *
 * Compiled into the binary so `mcme selftest` can run them anywhere, including
 * from an agent that has no test harness to invoke. The same functions are
 * linked into tests/unit/test_core.c, which is the normal way to run them.
 *
 * These are intentionally blunt: a counter and a printed line per failure.
 * A framework would be more machinery than the checks are worth.
 */

static int g_failed;

#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) {                                                         \
            g_failed++;                                                        \
            printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);           \
        }                                                                      \
    } while (0)

#define CHECK_STR(a, b)                                                        \
    do {                                                                       \
        const char *_a = (a), *_b = (b);                                       \
        if (!_a || !_b || strcmp(_a, _b) != 0) {                               \
            g_failed++;                                                        \
            printf("  FAIL %s:%d  expected \"%s\", got \"%s\"\n",              \
                   __FILE__, __LINE__, _b ? _b : "(null)", _a ? _a : "(null)");\
        }                                                                      \
    } while (0)

/* ------------------------------------------------------------------
 * Arena
 * ------------------------------------------------------------------ */

static void test_arena(void)
{
    McmeArena *a = mcme_arena_new();
    CHECK(a != NULL);

    /* Basic allocation returns distinct, aligned, writable memory. */
    char *p1 = mcme_arena_alloc(a, 10);
    char *p2 = mcme_arena_alloc(a, 100);
    CHECK(p1 != NULL && p2 != NULL);
    CHECK(p1 != p2);
    CHECK((((uintptr_t)p1) & 15u) == 0);
    CHECK((((uintptr_t)p2) & 15u) == 0);

    memset(p1, 'a', 10);
    memset(p2, 'b', 100);
    CHECK(p1[0] == 'a' && p1[9] == 'a');
    CHECK(p2[0] == 'b' && p2[99] == 'b');

    /* Allocations must not overlap. */
    CHECK(p2 >= p1 + 10 || p1 >= p2 + 100);

    /* Zero-size still returns valid memory (a real hazard in a mesher). */
    char *p3 = mcme_arena_alloc(a, 0);
    CHECK(p3 != NULL);

    /* strdup copies, and copies are independent. */
    char *s = mcme_arena_strdup(a, "nbt");
    CHECK_STR(s, "nbt");
    s[0] = 'N';
    CHECK_STR(s, "Nbt");

    /* An allocation larger than the default block gets a standalone block
       and must not clobber the small ones. */
    char *big = mcme_arena_alloc(a, 1024u * 1024u);
    CHECK(big != NULL);
    memset(big, 'c', 1024u * 1024u);
    CHECK(p1[0] == 'a');
    CHECK(p2[0] == 'b');

    /* Usage tracking is monotonic. */
    CHECK(mcme_arena_bytes(a) >= 1024u * 1024u);

    mcme_arena_free(a);
}

/* ------------------------------------------------------------------
 * Event queue
 * ------------------------------------------------------------------ */

static int g_sum;

static void add_one(void *ud)   { g_sum += *(int *)ud; }
static void double_it(void *ud) { *(int *)ud *= 2; }

static void test_event_queue(void)
{
    McmeEventQueue *q = mcme_eq_new();
    CHECK(q != NULL);
    CHECK(mcme_eq_pending(q) == 0);
    CHECK(mcme_eq_drain(q) == 0);        /* draining empty is safe */

    int one = 1, two = 2, ten = 10;
    g_sum = 0;
    CHECK(mcme_eq_post(q, add_one, &one));
    CHECK(mcme_eq_post(q, add_one, &two));
    CHECK(mcme_eq_post(q, add_one, &ten));
    CHECK(mcme_eq_pending(q) == 3);

    CHECK(mcme_eq_drain(q) == 3);
    CHECK(g_sum == 13);
    CHECK(mcme_eq_pending(q) == 0);      /* drained, not just run */

    /* Handlers may post new events: drain snapshots, so this cannot deadlock. */
    int v = 5;
    CHECK(mcme_eq_post(q, double_it, &v));
    CHECK(mcme_eq_drain(q) == 1);
    CHECK(v == 10);

    mcme_eq_free(q);
}

/* ------------------------------------------------------------------
 * Thread pool
 * ------------------------------------------------------------------ */

static pthread_mutex_t g_counter_mu = PTHREAD_MUTEX_INITIALIZER;
static int g_counter;

static void bump(void *ud)
{
    (void)ud;
    pthread_mutex_lock(&g_counter_mu);
    g_counter++;
    pthread_mutex_unlock(&g_counter_mu);
}

static void test_thread_pool(void)
{
    McmeThreadPool *tp = mcme_tp_new(4);
    CHECK(tp != NULL);

    g_counter = 0;
    const int N = 200;
    for (int i = 0; i < N; ++i) {
        CHECK(mcme_tp_submit(tp, bump, NULL));
    }
    mcme_tp_wait(tp);

    /* Every submitted job must have run exactly once. */
    CHECK(g_counter == N);
    CHECK(mcme_tp_completed(tp) == (size_t)N);

    /* Reuse after wait: the pool is not one-shot. */
    for (int i = 0; i < 50; ++i) {
        CHECK(mcme_tp_submit(tp, bump, NULL));
    }
    mcme_tp_wait(tp);
    CHECK(g_counter == N + 50);

    mcme_tp_free(tp);

    /* Freeing a NULL pool is safe. */
    mcme_tp_free(NULL);
}

/* ------------------------------------------------------------------
 * Filesystem
 * ------------------------------------------------------------------ */

static void test_fs(void)
{
    CHECK(!mcme_fs_exists("/definitely/not/a/real/path/xyz"));

    /* Round trip through a temp file. */
    const char *path = "build/selftest_fs.txt";
    const char *payload = "the quick brown fox\n";
    CHECK(mcme_fs_mkdir_p("build"));
    CHECK(mcme_fs_write(path, payload, strlen(payload)));

    void *data = NULL;
    size_t size = 0;
    CHECK(mcme_fs_read(path, &data, &size));
    CHECK(size == strlen(payload));
    CHECK(memcmp(data, payload, size) == 0);
    free(data);

    /* read_text gives a NUL-terminated buffer. */
    char *text = mcme_fs_read_text(path);
    CHECK_STR(text, payload);
    free(text);

    /* mkdir_p is idempotent. */
    CHECK(mcme_fs_mkdir_p("build/selftest_dir/a/b"));
    CHECK(mcme_fs_mkdir_p("build/selftest_dir/a/b"));

    /* Reading a missing file fails and sets an error message. */
    void *d2;
    size_t s2;
    CHECK(!mcme_fs_read("/no/such/file/here", &d2, &s2));
    CHECK(mcme_last_error()[0] != '\0');
    mcme_clear_error();

    /* Reading a DIRECTORY must fail. fopen() succeeds on one and, on Linux,
       ftell then reports LONG_MAX — which reached malloc() as 2^63 bytes and
       aborted under ASan. Found by the region reader pointing at
       tests/golden/worlds/java/region instead of an .mca file. The check is
       fstat + S_ISREG in mcme_fs_read; this pins it. */
    CHECK(!mcme_fs_read("build", &d2, &s2));
    CHECK(!mcme_fs_read(".", &d2, &s2));
    CHECK(mcme_fs_read_text("build") == NULL);
    mcme_clear_error();
}

/* ------------------------------------------------------------------
 * Lang parser
 * ------------------------------------------------------------------ */

static void test_lang(void)
{
    const char *path = "build/selftest.lang";
    const char *src =
        "# a comment line\n"
        "\n"
        "simple.key=simple value\n"
        "  spaced.key  =  value with spaces  \n"
        "newline.key=first\\nsecond\n"
        "empty.value=\n"
        "not a key value line\n"
        "utf8.key=café 東京\n"
        "hash.in.value=has # inside\n"
        "backslash.key=C:\\path\\here\n";

    CHECK(mcme_fs_write(path, src, strlen(src)));

    McmeLang lang;
    CHECK(mcme_lang_load_file(&lang, "en", path));
    CHECK_STR(lang.id, "en");
    CHECK(lang.entry_count == 7);

    CHECK_STR(mcme_lang_get(&lang, "simple.key"), "simple value");
    /* Leading/trailing whitespace around the key and value is trimmed. */
    CHECK_STR(mcme_lang_get(&lang, "spaced.key"), "value with spaces");
    /* \n is decoded to a real newline. */
    CHECK_STR(mcme_lang_get(&lang, "newline.key"), "first\nsecond");
    /* An empty value is a valid empty string, not a missing key. */
    CHECK_STR(mcme_lang_get(&lang, "empty.value"), "");
    /* '#' mid-value is literal, only a leading '#' comments. */
    CHECK_STR(mcme_lang_get(&lang, "hash.in.value"), "has # inside");
    CHECK_STR(mcme_lang_get(&lang, "backslash.key"), "C:\\path\\here");
    /* UTF-8 bytes survive unmodified. */
    CHECK_STR(mcme_lang_get(&lang, "utf8.key"), "café 東京");

    /* A missing key returns NULL rather than a bogus empty string. */
    CHECK(mcme_lang_get(&lang, "no.such.key") == NULL);
    CHECK(mcme_lang_get(&lang, NULL) == NULL);

    mcme_lang_free(&lang);

    /* Free twice is safe, and a failed load leaves nothing to free. */
    mcme_lang_free(&lang);
    McmeLang bad;
    CHECK(!mcme_lang_load_file(&bad, "xx", "/no/such.lang"));
}

/* ------------------------------------------------------------------
 * Config (JSON)
 * ------------------------------------------------------------------ */

static void test_config(void)
{
    const char *path = "build/selftest.config.json";

    struct cJSON *cfg = cJSON_CreateObject();
    CHECK(cfg != NULL);

    CHECK(mcme_config_set(cfg, "options.fov", "70"));
    CHECK(mcme_config_set(cfg, "options.render_distance", "5"));
    CHECK(mcme_config_set(cfg, "ui.theme", "\"dark\""));

    /* Setting the same key twice must REPLACE, not append a duplicate
       member. cJSON_AddItemToObject appends by default, which silently
       produces duplicate keys that parse back unpredictably. */
    struct cJSON *fov;
    CHECK(mcme_config_set(cfg, "options.fov", "90"));
    fov = mcme_config_get(cfg, "options.fov");
    CHECK(fov != NULL && cJSON_IsNumber(fov));
    CHECK(fov && fov->valueint == 90);
    /* Exactly one "fov" child under "options". */
    {
        struct cJSON *opts = mcme_config_get(cfg, "options");
        int fov_count = 0;
        if (opts) {
            for (struct cJSON *child = opts->child; child; child = child->next) {
                if (child->string && strcmp(child->string, "fov") == 0) fov_count++;
            }
        }
        CHECK(fov_count == 1);
    }
    /* Reset to the value the round-trip test below expects. */
    CHECK(mcme_config_set(cfg, "options.fov", "70"));

    fov = mcme_config_get(cfg, "options.fov");
    CHECK(fov != NULL && cJSON_IsNumber(fov));
    CHECK(fov && fov->valueint == 70);

    struct cJSON *theme = mcme_config_get(cfg, "ui.theme");
    CHECK(theme != NULL && cJSON_IsString(theme));

    /* A missing path yields NULL, not a crash. */
    CHECK(mcme_config_get(cfg, "no.such.thing") == NULL);
    CHECK(mcme_config_get(cfg, "") != NULL);   /* empty path is the root */

    CHECK(mcme_config_save(path, cfg));
    cJSON_Delete(cfg);

    /* Reload and confirm the values survived the round trip. */
    struct cJSON *reloaded = mcme_config_load(path);
    CHECK(reloaded != NULL);
    if (reloaded) {
        struct cJSON *rd = mcme_config_get(reloaded, "options.render_distance");
        CHECK(rd != NULL && rd->valueint == 5);
        cJSON_Delete(reloaded);
    }

    /* Loading a non-JSON file fails rather than returning a half-tree. */
    CHECK(mcme_fs_write("build/selftest.bad.json", "not json at all", 15));
    CHECK(mcme_config_load("build/selftest.bad.json") == NULL);
    mcme_clear_error();
}

/* ------------------------------------------------------------------
 * Entry point
 * ------------------------------------------------------------------ */

int mcme_test_core(void)
{
    g_failed = 0;

    test_arena();
    test_event_queue();
    test_thread_pool();
    test_fs();
    test_lang();
    test_config();

    return g_failed;
}
