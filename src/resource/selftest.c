#include "mcme/resource.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mcme/core.h"

/*
 * Resource pack manifest — the self-test.
 *
 * The reference reader (Java `JavaResourcePack`) is the oracle. It calls a
 * directory a pack by one test — `pack.mcmeta` is a file — reads
 * `pack.pack_format` as an integer and `pack.description` as a string, and
 * only marks the pack valid once the format parsed. These checks pin exactly
 * that: the happy path, the three refusals, and the description default.
 */

static int g_failed;

#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) {                                                         \
            g_failed++;                                                        \
            printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);           \
        }                                                                      \
    } while (0)

#define CHECK_MSG(cond, ...)                                                   \
    do {                                                                       \
        if (!(cond)) {                                                         \
            g_failed++;                                                        \
            printf("  FAIL %s:%d  %s\n    ", __FILE__, __LINE__, #cond);       \
            printf(__VA_ARGS__);                                               \
            printf("\n");                                                      \
        }                                                                      \
    } while (0)

/* Write `text` to `path`, creating every parent directory the path names. */
static void put(const char *path, const char *text)
{
    /* mcme_fs_mkdir_p wants the DIRECTORY, so drop the last '/...' component
       by truncating at the final slash into a scratch buffer. */
    char dir[512];
    snprintf(dir, sizeof(dir), "%s", path);
    char *slash = strrchr(dir, '/');
    if (slash) {
        *slash = '\0';
        CHECK_MSG(mcme_fs_mkdir_p(dir), "mkdir '%s': %s", dir,
                  mcme_last_error());
    }
    CHECK_MSG(mcme_fs_write(path, text, strlen(text)), "write '%s': %s", path,
              mcme_last_error());
}

static void test_pack_manifest(void)
{
    printf("  resource (pack manifest)\n");

    /* The happy path: a well-formed manifest with format and description. */
    put("build/pack_fixtures/good/pack.mcmeta",
        "{\"pack\":{\"pack_format\":9,\"description\":\"A study pack\"}}");
    put("build/pack_fixtures/good/pack.png", "not-really-a-png");

    McmePack *p = NULL;
    CHECK(mcme_pack_open("build/pack_fixtures/good", &p));
    CHECK(p != NULL);
    if (p) {
        CHECK_MSG(p->format == 9, "want 9, got %d", p->format);
        CHECK_MSG(strcmp(p->description, "A study pack") == 0,
                  "want 'A study pack', got '%s'", p->description);
        CHECK(p->has_icon);
        CHECK(p->root != NULL);
        mcme_pack_free(p);
    }

    /* A missing description is the empty string, not a refusal — the
       reference defaults it and keeps the pack valid. */
    put("build/pack_fixtures/nodesc/pack.mcmeta",
        "{\"pack\":{\"pack_format\":4}}");
    p = NULL;
    CHECK(mcme_pack_open("build/pack_fixtures/nodesc", &p));
    CHECK(p != NULL);
    if (p) {
        CHECK(p->format == 4);
        CHECK(p->description && p->description[0] == '\0');
        CHECK(!p->has_icon);
        mcme_pack_free(p);
    }

    /* Refusal 1: no manifest at all. (The directory must exist; only the
       file inside it is missing.) */
    CHECK(mcme_fs_mkdir_p("build/pack_fixtures/empty"));
    p = NULL;
    CHECK(!mcme_pack_open("build/pack_fixtures/empty", &p));
    CHECK(mcme_last_error()[0] != '\0');

    /* Refusal 2: the manifest is not the game's `{pack:{...}}` shape. */
    put("build/pack_fixtures/badshape/pack.mcmeta", "{\"nope\":true}");
    p = NULL;
    CHECK(!mcme_pack_open("build/pack_fixtures/badshape", &p));

    /* Refusal 3: no integer pack_format — the reference's validity lynchpin. */
    put("build/pack_fixtures/nofmt/pack.mcmeta",
        "{\"pack\":{\"description\":\"format-less\"}}");
    p = NULL;
    CHECK(!mcme_pack_open("build/pack_fixtures/nofmt", &p));

    /* Refusal 4: the manifest is not JSON at all. */
    put("build/pack_fixtures/notjson/pack.mcmeta", "this is not json");
    p = NULL;
    CHECK(!mcme_pack_open("build/pack_fixtures/notjson", &p));

    /* A NULL out or dir is a refusal, not a crash. */
    CHECK(!mcme_pack_open(NULL, &p));
    CHECK(!mcme_pack_open("build/pack_fixtures/good", NULL));

    /* Freeing NULL is a no-op. */
    mcme_pack_free(NULL);

    /* The refusals above leave their message set; clear it the way every
       selftest that exercises an error path does, so the binary does not
       print a stale refusal when the suite passes. */
    mcme_clear_error();
}

/* ========================================================================
 * Blockstate selection (D30).
 *
 * Every expectation below is the reference reader's own answer,
 * rule for rule: variants first-match in file order with the
 * missing-property default (a states-less block selects the FIRST
 * key), the comma-paired regex that never sees a `|` second value
 * and matches everything on a key it cannot pair, multipart
 * cases each contributing with the opposite missing-property rule
 * (absent never matches), empty `when` matching, a non
 * string/boolean `when` value skipping the case, an apply list
 * taking its first element, and the three shapes of "missing" —
 * no file, no file key, no matching variant — kept apart from
 * multipart's empty selection.
 * ======================================================================== */

#define FIXDIR "build/blockstate_fixtures/pack"

/* One lookup against namespace `mc` on the fixture pack. */
static void expect_models(const McmeBlockstateIndex *idx,
                          const char *block, const McmeState *states,
                          size_t n, const char *const *want,
                          size_t want_n, bool want_missing)
{
    McmeBlockstate got;
    if (!mcme_blockstate_lookup(idx, "mc", block, states, n, &got)) {
        CHECK_MSG(false, "lookup %s failed: %s", block, mcme_last_error());
        return;
    }
    CHECK_MSG(got.missing == want_missing,
              "%s: missing=%d, want %d", block, got.missing,
              want_missing);
    CHECK_MSG(got.count == want_n,
              "%s: %zu models, want %zu", block, got.count, want_n);
    for (size_t i = 0; i < got.count && i < want_n; ++i) {
        const char *g = got.models[i].model ? got.models[i].model
                                            : "(no model)";
        const char *w = want[i] ? want[i] : "(no model)";
        CHECK_MSG(strcmp(g, w) == 0, "%s: model %zu is %s, want %s",
                  block, i, g, w);
    }
    mcme_blockstate_free(&got);
}

static void test_blockstates(void)
{
    printf("  resource (blockstates)\n");

    /* --- fixtures --- */
    put(FIXDIR "/assets/mc/blockstates/stone.json",
        "{\"variants\":{\"\":{\"model\":\"mc:block/stone\"}}}");
    put(FIXDIR "/assets/mc/blockstates/stairs.json",
        "{\"variants\":{"
          "\"facing=east,half=bottom\":"
            "{\"model\":\"mc:block/stairs_eb\",\"y\":90},"
          "\"facing=north\":"
            "{\"model\":\"mc:block/stairs_n\",\"uvlock\":true},"
          "\"\":{\"model\":\"mc:block/stairs_default\"}}}");
    put(FIXDIR "/assets/mc/blockstates/quirk.json",
        "{\"variants\":{\"a=1,b=2\":{\"model\":\"m1\"},"
                       "\"a=1\":{\"model\":\"m2\"}}}");
    put(FIXDIR "/assets/mc/blockstates/pipe.json",
        "{\"variants\":{\"x=1|2\":{\"model\":\"p1\"},"
                       "\"x=2\":{\"model\":\"p2\"}}}");
    put(FIXDIR "/assets/mc/blockstates/junkkey.json",
        "{\"variants\":{\"X=1\":{\"model\":\"j1\"},"
                       "\"\":{\"model\":\"j2\"}}}");
    put(FIXDIR "/assets/mc/blockstates/nomatch.json",
        "{\"variants\":{\"a=1\":{\"model\":\"n1\"}}}");
    put(FIXDIR "/assets/mc/blockstates/empty.json", "{}");
    put(FIXDIR "/assets/mc/blockstates/fence.json",
        "{\"multipart\":["
          "{\"apply\":{\"model\":\"post\"}},"
          "{\"when\":{\"north\":\"true\"},"
           "\"apply\":{\"model\":\"ns\"}},"
          "{\"when\":{\"OR\":[{\"east\":\"true\"},{\"west\":\"true\"}]},"
           "\"apply\":{\"model\":\"ew\"}},"
          "{\"when\":{\"AND\":[{\"a\":\"1\"},{\"b\":\"2\"}]},"
           "\"apply\":{\"model\":\"ab\"}},"
          "{\"when\":{\"x\":\"1|2\"},\"apply\":{\"model\":\"pipe\"}},"
          "{\"when\":{\"lit\":true},\"apply\":{\"model\":\"lit\"}},"
          "{\"when\":{\"gone\":\"v\"},\"apply\":{\"model\":\"gone\"}},"
          "{\"when\":{},\"apply\":{\"model\":\"emptywhen\"}},"
          "{\"when\":{\"abort\":3},\"apply\":{\"model\":\"abort\"}},"
          "{\"when\":{\"x\":\"1\"},\"apply\":{\"model\":\"x1\"}}"
        "]}");
    put(FIXDIR "/assets/mc/blockstates/zero.json",
        "{\"multipart\":[{\"when\":{\"p\":\"1\"},"
                        "\"apply\":{\"model\":\"z1\"}}]}");
    put(FIXDIR "/assets/mc/blockstates/list.json",
        "{\"multipart\":[{\"when\":{\"q\":\"1\"},"
                        "\"apply\":[{\"model\":\"L0\"},"
                                  "{\"model\":\"L1\"}]},"
                        "{\"apply\":[{\"model\":\"M0\"}]}]}");
    put(FIXDIR "/assets/mc/blockstates/nomodel.json",
        "{\"variants\":{\"\":{\"x\":90}}}");
    /* Not JSON at all: the index skips it with a warning, the
       reference skipping it too, so it answers missing. */
    put(FIXDIR "/assets/mc/blockstates/bad.json", "{oops");

    McmeBlockstateIndex *idx = NULL;
    CHECK_MSG(mcme_blockstate_index_open(FIXDIR, &idx),
              "index open: %s", mcme_last_error());
    if (!idx) { mcme_clear_error(); return; }

    /* --- variants --- */
    expect_models(idx, "stone", NULL, 0,
                  (const char *const[]){ "mc:block/stone" }, 1, false);

    static const McmeState east_bottom[] = {
        { "facing", "east" }, { "half", "bottom" } };
    expect_models(idx, "stairs", east_bottom, 2,
                  (const char *const[]){ "mc:block/stairs_eb" }, 1,
                  false);

    static const McmeState north[] = { { "facing", "north" } };
    expect_models(idx, "stairs", north, 1,
                  (const char *const[]){ "mc:block/stairs_n" }, 1,
                  false);

    static const McmeState south[] = { { "facing", "south" } };
    expect_models(idx, "stairs", south, 1,
                  (const char *const[]){ "mc:block/stairs_default" }, 1,
                  false);

    /* No states at all: every pair in the FIRST key defaults to a
       match — a states-less block selects the first key, the
       default "" key never being reached. */
    expect_models(idx, "stairs", NULL, 0,
                  (const char *const[]){ "mc:block/stairs_eb" }, 1,
                  false);

    /* The missing-property quirk: block has a, not b — key one
       still matches (b=2 defaults to itself). */
    static const McmeState a_only[] = { { "a", "1" } };
    expect_models(idx, "quirk", a_only, 1,
                  (const char *const[]){ "m1" }, 1, false);

    static const McmeState a_b3[] = { { "a", "1" }, { "b", "3" } };
    expect_models(idx, "quirk", a_b3, 2,
                  (const char *const[]){ "m2" }, 1, false);

    /* Only a PRESENT property with the wrong value fails a variant. */
    static const McmeState a_2[] = { { "a", "2" } };
    expect_models(idx, "quirk", a_2, 1, NULL, 0, true);

    /* The `|` second value is never paired: key one sees only
       x=1, fails on x=2, and key two wins. */
    static const McmeState x_2[] = { { "x", "2" } };
    expect_models(idx, "pipe", x_2, 1,
                  (const char *const[]){ "p2" }, 1, false);
    static const McmeState x_1[] = { { "x", "1" } };
    expect_models(idx, "pipe", x_1, 1,
                  (const char *const[]){ "p1" }, 1, false);

    /* A key the regex pairs nothing from matches everything. */
    static const McmeState x_0[] = { { "x", "0" } };
    expect_models(idx, "junkkey", x_0, 1,
                  (const char *const[]){ "j1" }, 1, false);

    static const McmeState nomatch_ok[] = { { "a", "1" } };
    expect_models(idx, "nomatch", nomatch_ok, 1,
                  (const char *const[]){ "n1" }, 1, false);
    static const McmeState nomatch_bad[] = { { "a", "9" } };
    expect_models(idx, "nomatch", nomatch_bad, 1, NULL, 0, true);

    /* Neither variants nor multipart: missing. */
    expect_models(idx, "empty", NULL, 0, NULL, 0, true);
    /* The skipped unparseable file: missing. */
    expect_models(idx, "bad", NULL, 0, NULL, 0, true);
    /* No file at all: missing. */
    expect_models(idx, "air", NULL, 0, NULL, 0, true);

    /* A winning apply without a model: selected, model NULL. */
    {
        McmeBlockstate got;
        CHECK(mcme_blockstate_lookup(idx, "mc", "nomodel", NULL, 0,
                                     &got));
        CHECK(got.count == 1 && !got.missing);
        if (got.count == 1) {
            CHECK(got.models[0].model == NULL);
            CHECK(got.models[0].x == 90);
            CHECK(got.models[0].y == 0);
            CHECK(!got.models[0].uvlock);
        }
        mcme_blockstate_free(&got);
    }

    /* --- multipart --- */
    static const McmeState probe[] = {
        { "north", "true" }, { "x", "2" }, { "lit", "true" } };
    expect_models(idx, "fence", probe, 3,
                  (const char *const[]){
                      "post", "ns", "pipe", "lit", "emptywhen" },
                  5, false);

    /* No states: the unguarded case and the empty `when` only. */
    expect_models(idx, "fence", NULL, 0,
                  (const char *const[]){ "post", "emptywhen" }, 2,
                  false);

    static const McmeState north_false[] = { { "north", "false" } };
    expect_models(idx, "fence", north_false, 1,
                  (const char *const[]){ "post", "emptywhen" }, 2,
                  false);

    /* All cases guarded and failing: an EMPTY selection is not
       missing — only "no file / no key / no variant" is. */
    static const McmeState p_0[] = { { "p", "0" } };
    expect_models(idx, "zero", p_0, 1, NULL, 0, false);
    static const McmeState p_1[] = { { "p", "1" } };
    expect_models(idx, "zero", p_1, 1,
                  (const char *const[]){ "z1" }, 1, false);

    /* An apply list contributes its FIRST element; the unguarded
       list case still contributes on its own. */
    static const McmeState q_1[] = { { "q", "1" } };
    expect_models(idx, "list", q_1, 1,
                  (const char *const[]){ "L0", "M0" }, 2, false);
    expect_models(idx, "list", NULL, 0,
                  (const char *const[]){ "M0" }, 1, false);

    mcme_blockstate_index_close(idx);

    /* An index over a pack with no assets answers missing. */
    idx = NULL;
    CHECK(mcme_blockstate_index_open("build/pack_fixtures", &idx));
    if (idx) {
        expect_models(idx, "stone", NULL, 0, NULL, 0, true);
        mcme_blockstate_index_close(idx);
    }

    /* Null arguments refuse; NULL frees are no-ops. */
    CHECK(!mcme_blockstate_index_open(NULL, &idx));
    CHECK(!mcme_blockstate_index_open(FIXDIR, NULL));
    mcme_blockstate_index_close(NULL);
    mcme_blockstate_free(NULL);

    mcme_clear_error();
}

int mcme_test_resource(void)
{
    g_failed = 0;
    test_pack_manifest();
    test_blockstates();
    return g_failed;
}
