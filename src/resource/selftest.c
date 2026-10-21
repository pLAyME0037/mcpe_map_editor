#include "mcme/resource.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mcme/core.h"
#include "cJSON.h"
#include "stb_image_write.h"

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
    /* A pack names itself: the loader gate (D32) loads nothing from
       a directory whose manifest does not say pack of format 2+. */
    put(FIXDIR "/pack.mcmeta",
        "{\"pack\":{\"pack_format\":9,\"description\":\"blockstates\"}}");
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

    /* --- what the index holds, in its own walk order --- */

    CHECK(mcme_blockstate_count(idx) > 0);
    {
        bool saw_stone = false, saw_stairs = false, saw_fence = false;
        const char *ns = NULL, *name = NULL;

        for (size_t i = 0; i < mcme_blockstate_count(idx); ++i) {
            CHECK(mcme_blockstate_at(idx, i, &ns, &name));
            CHECK(ns != NULL && ns[0] != '\0');
            CHECK(name != NULL && name[0] != '\0');
            /* The unparseable file was skipped whole: it is not
               enumerated as a block the index would answer. */
            CHECK(strcmp(name, "bad") != 0);
            if (strcmp(name, "stone") == 0) saw_stone = true;
            if (strcmp(name, "stairs") == 0) saw_stairs = true;
            if (strcmp(name, "fence") == 0) saw_fence = true;
        }
        CHECK(saw_stone && saw_stairs && saw_fence);
        /* Out of range and null: refused, both outs named NULL. */
        ns = name = "untouched";
        CHECK(!mcme_blockstate_at(idx, mcme_blockstate_count(idx),
                                  &ns, &name));
        CHECK(ns == NULL && name == NULL);
        CHECK(!mcme_blockstate_at(NULL, 0, &ns, &name));
        CHECK(ns == NULL && name == NULL);
    }
    CHECK(mcme_blockstate_count(NULL) == 0);

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

    /* A state the store typed (value NULL): present, and
       matching nothing — the reference compares py_data against
       the text, and a typed value is never equal to it (D43).
       The other keys still match; a key naming the typed state
       never does. */
    static const McmeState a1_typed_b[] = {
        { "a", "1" }, { "b", NULL } };
    expect_models(idx, "quirk", a1_typed_b, 2,
                  (const char *const[]){ "m2" }, 1, false);
    static const McmeState typed_a[] = { { "a", NULL } };
    expect_models(idx, "quirk", typed_a, 1, NULL, 0, true);

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

/* ========================================================================
 * Model folding (D31).
 *
 * Every expectation below is the reference's own
 * `_recursive_load_block_model` answer, rule for rule: the parent
 * chain folds first and the child merges over it; textures merge
 * per key (keys the child does not name survive from the parent);
 * elements override wholesale (whichever model names them last
 * contributes the whole list, an empty list included); every
 * other key — display, gui_light, parent — is discarded; a file
 * the index does not have, at any level, folds onto the empty
 * object. Plus mcme's two recorded answers where the reference
 * would crash: a non-string parent reads as no parent, and a
 * chain deeper than the cap is a refusal.
 * ======================================================================== */

#define MFIXDIR "build/model_fixtures/pack"

/* One load, compared against the expected fold as JSON. */
static void expect_model(const McmeModelIndex *idx, const char *ns,
                         const char *path, const char *want_json)
{
    struct cJSON *doc = NULL;
    if (!mcme_model_load(idx, ns, path, &doc)) {
        CHECK_MSG(false, "load %s:%s failed: %s", ns ? ns : "(default)",
                  path, mcme_last_error());
        return;
    }
    struct cJSON *want = cJSON_Parse(want_json);
    CHECK_MSG(want != NULL, "bad expectation JSON: %s", want_json);
    if (want) {
        if (!cJSON_Compare(doc, want, true)) {
            char *got = cJSON_PrintUnformatted(doc);
            CHECK_MSG(false, "%s:%s folds to %s, want %s",
                      ns ? ns : "(default)", path, got ? got : "?",
                      want_json);
            if (got) cJSON_free(got);
        }
        cJSON_Delete(want);
    }
    cJSON_Delete(doc);
}

static void test_models(void)
{
    printf("  resource (models)\n");

    /* --- fixtures --- */
    /* A pack names itself: the loader gate (D32) loads nothing from
       a directory whose manifest does not say pack of format 2+. */
    put(MFIXDIR "/pack.mcmeta",
        "{\"pack\":{\"pack_format\":9,\"description\":\"models\"}}");
    put(MFIXDIR "/assets/mc/models/block/base.json",
        "{\"textures\":{\"top\":\"t/base_top\",\"side\":\"t/base_side\","
          "\"bottom\":\"t/base_bottom\"},"
        "\"elements\":[{\"from\":[0,0,0],\"to\":[16,16,16]}],"
        "\"display\":{\"thirdperson\":{\"rotation\":[0,0,0]}}}");
    /* Child over parent: textures per key, elements wholesale,
       display and parent discarded. */
    put(MFIXDIR "/assets/mc/models/block/child.json",
        "{\"parent\":\"mc:block/base\","
        "\"textures\":{\"top\":\"t/child_top\",\"side\":\"t/child_side\"},"
        "\"elements\":[{\"from\":[1,1,1],\"to\":[2,2,2]}]}");
    /* Child without elements: the parent's list survives. */
    put(MFIXDIR "/assets/mc/models/block/noelem.json",
        "{\"parent\":\"mc:block/base\",\"textures\":{\"top\":\"t/noelem_top\"}}");
    /* Child with an EMPTY elements list: wholesale means the
       parent's list is wiped, not merged. */
    put(MFIXDIR "/assets/mc/models/block/wipe.json",
        "{\"parent\":\"mc:block/base\",\"elements\":[]}");
    /* A three-deep chain: nearest ancestor's keys win one by one. */
    put(MFIXDIR "/assets/mc/models/block/grand.json",
        "{\"parent\":\"mc:block/noelem\",\"textures\":{\"side\":\"t/grand_side\"}}");
    /* Nothing but discarded keys folds to the empty object. */
    put(MFIXDIR "/assets/mc/models/block/onlydisplay.json",
        "{\"display\":{\"gui\":{\"rotation\":[1,0,0]}},\"gui_light\":\"front\"}");
    /* An unknown parent folds onto nothing; the child still answers. */
    put(MFIXDIR "/assets/mc/models/block/orphan.json",
        "{\"parent\":\"mc:block/absent\",\"textures\":{\"a\":\"t/a\"}}");
    /* Texture values ride along at face value: no validation here. */
    put(MFIXDIR "/assets/mc/models/block/oddvalue.json",
        "{\"textures\":{\"num\":42,\"flag\":true,\"none\":null}}");
    /* A file that is not even an object: the reference's `in`
       checks all miss, so it folds to the empty object. */
    put(MFIXDIR "/assets/mc/models/block/junk.json", "[1,2]");
    /* Not JSON at all: the index skips it with a warning, the
       reference skipping it too, so it answers the empty model. */
    put(MFIXDIR "/assets/mc/models/block/bad.json", "{oops");
    /* A bare parent reference defaults to minecraft, the way the
       reference's own split does. */
    put(MFIXDIR "/assets/minecraft/models/block/leaf.json",
        "{\"textures\":{\"leaf\":\"t/leaf\"}}");
    put(MFIXDIR "/assets/minecraft/models/block/defns.json",
        "{\"parent\":\"block/leaf\",\"textures\":{\"x\":\"t/defns_x\"}}");
    /* A parent cycle: the reference would blow its Python stack;
       mcme refuses by name past the depth cap. */
    put(MFIXDIR "/assets/mc/models/block/loopa.json",
        "{\"parent\":\"mc:block/loopb\"}");
    put(MFIXDIR "/assets/mc/models/block/loopb.json",
        "{\"parent\":\"mc:block/loopa\"}");
    /* A chain deeper than MODEL_DEPTH_CAP, written before the
       index opens so the whole chain is indexed. */
    {
        char body[256];
        for (int i = 0; i <= 70; ++i) {
            char file[512];
            snprintf(file, sizeof file,
                     MFIXDIR "/assets/mc/models/block/deep%d.json", i);
            if (i < 70)
                snprintf(body, sizeof body,
                         "{\"parent\":\"mc:block/deep%d\"}", i + 1);
            else
                snprintf(body, sizeof body, "{\"textures\":{\"x\":\"t/x\"}}");
            put(file, body);
        }
    }

    McmeModelIndex *idx = NULL;
    CHECK_MSG(mcme_model_index_open(MFIXDIR, &idx),
              "index open: %s", mcme_last_error());
    if (!idx) { mcme_clear_error(); return; }

    /* --- what the index holds, in its own walk order --- */

    CHECK(mcme_model_count(idx) > 0);
    {
        bool saw_grand = false, saw_defns = false, saw_leaf = false;
        const char *ns = NULL, *path = NULL;

        for (size_t i = 0; i < mcme_model_count(idx); ++i) {
            CHECK(mcme_model_at(idx, i, &ns, &path));
            CHECK(ns != NULL && ns[0] != '\0');
            CHECK(path != NULL && path[0] != '\0');
            if (strcmp(ns, "mc") == 0 &&
                strcmp(path, "block/grand") == 0) saw_grand = true;
            if (strcmp(ns, "minecraft") == 0 &&
                strcmp(path, "block/defns") == 0) saw_defns = true;
            if (strcmp(ns, "minecraft") == 0 &&
                strcmp(path, "block/leaf") == 0) saw_leaf = true;
        }
        CHECK(saw_grand && saw_defns && saw_leaf);
        /* Out of range and null: refused, both outs named NULL. */
        ns = path = "untouched";
        CHECK(!mcme_model_at(idx, mcme_model_count(idx), &ns, &path));
        CHECK(ns == NULL && path == NULL);
        CHECK(!mcme_model_at(NULL, 0, &ns, &path));
        CHECK(ns == NULL && path == NULL);
    }
    CHECK(mcme_model_count(NULL) == 0);

    /* --- the fold, rule for rule --- */

    /* A chain of three: grandchild over parent over grandparent.
       textures accumulate per key down the chain; elements come
       from the nearest ancestor that names them. */
    expect_model(idx, "mc", "block/grand",
                 "{\"textures\":{\"top\":\"t/noelem_top\","
                   "\"side\":\"t/grand_side\","
                   "\"bottom\":\"t/base_bottom\"},"
                 "\"elements\":[{\"from\":[0,0,0],\"to\":[16,16,16]}]}");

    /* Child over parent: every child key overwrites that one key,
       the child's elements replace the list wholesale, and display
       and parent are gone. */
    expect_model(idx, "mc", "block/child",
                 "{\"textures\":{\"top\":\"t/child_top\","
                   "\"side\":\"t/child_side\",\"bottom\":\"t/base_bottom\"},"
                 "\"elements\":[{\"from\":[1,1,1],\"to\":[2,2,2]}]}");

    /* No elements in the child: the parent's list rides up. */
    expect_model(idx, "mc", "block/noelem",
                 "{\"textures\":{\"top\":\"t/noelem_top\","
                   "\"side\":\"t/base_side\",\"bottom\":\"t/base_bottom\"},"
                 "\"elements\":[{\"from\":[0,0,0],\"to\":[16,16,16]}]}");

    /* An EMPTY elements list still overrides wholesale. */
    expect_model(idx, "mc", "block/wipe",
                 "{\"textures\":{\"top\":\"t/base_top\","
                   "\"side\":\"t/base_side\",\"bottom\":\"t/base_bottom\"},"
                 "\"elements\":[]}");

    /* Only discarded keys: the fold is the empty object, and an
       empty fold is an answer, not a failure. */
    expect_model(idx, "mc", "block/onlydisplay", "{}");

    /* An unknown parent: the child folds onto nothing. */
    expect_model(idx, "mc", "block/orphan",
                 "{\"textures\":{\"a\":\"t/a\"}}");

    /* No validation on values: a number, a boolean and a null ride
       along exactly as written. */
    expect_model(idx, "mc", "block/oddvalue",
                 "{\"textures\":{\"num\":42,\"flag\":true,\"none\":null}}");

    /* A non-object top level folds to the empty object. */
    expect_model(idx, "mc", "block/junk", "{}");
    /* The skipped unparseable file: the empty object. */
    expect_model(idx, "mc", "block/bad", "{}");
    /* No file at all: the empty object. */
    expect_model(idx, "mc", "block/absent", "{}");
    /* A namespace the pack does not hold: the empty object. */
    expect_model(idx, "zz", "block/base", "{}");

    /* The default namespace: NULL reads as minecraft, and a bare
       parent reference resolves there too. */
    expect_model(idx, NULL, "block/defns",
                 "{\"textures\":{\"leaf\":\"t/leaf\",\"x\":\"t/defns_x\"}}");
    /* The same model asked for by its explicit namespace. */
    expect_model(idx, "minecraft", "block/defns",
                 "{\"textures\":{\"leaf\":\"t/leaf\",\"x\":\"t/defns_x\"}}");

    /* The parent's own path carries a namespace when it has one,
       and the child's `minecraft:`-free reference is not confused
       with it. */
    expect_model(idx, "minecraft", "block/leaf",
                 "{\"textures\":{\"leaf\":\"t/leaf\"}}");

    /* --- the reference would crash; mcme answers --- */

    /* A parent cycle: refuse by name. (The reference raises
       RecursionError; the pin asserts both sides fail.) */
    {
        struct cJSON *doc = NULL;
        CHECK(!mcme_model_load(idx, "mc", "block/loopa", &doc));
        CHECK(doc == NULL);
        CHECK(mcme_last_error()[0] != '\0');
        mcme_clear_error();
    }

    /* A chain deeper than the cap refuses too. */
    {
        struct cJSON *doc = NULL;
        CHECK(!mcme_model_load(idx, "mc", "block/deep0", &doc));
        CHECK(doc == NULL);
        mcme_clear_error();
    }

    mcme_model_index_close(idx);

    /* An index over a pack with no assets answers the empty model. */
    idx = NULL;
    CHECK(mcme_model_index_open("build/pack_fixtures", &idx));
    if (idx) {
        expect_model(idx, "mc", "block/base", "{}");
        mcme_model_index_close(idx);
    }

    /* Null arguments refuse; NULL closes are no-ops. A NULL
       namespace is the minecraft default, not a null argument. */
    idx = NULL;
    CHECK(!mcme_model_index_open(NULL, &idx));
    CHECK(!mcme_model_index_open(MFIXDIR, NULL));
    {
        struct cJSON *doc = NULL;
        CHECK(!mcme_model_load(NULL, "mc", "block/base", &doc));
        CHECK(!mcme_model_load(NULL, NULL, NULL, &doc));
    }
    mcme_model_index_close(NULL);

    mcme_clear_error();
}

/* ========================================================================
 * Meshes (D32).
 *
 * Every expectation is the reference's own `_load_block_model`
 * answer: the missing BLOCK (a unit cube of the missing image) for
 * a model with textures but no elements, an empty elements list
 * counting as none; the empty mesh for a model with neither; the
 * reference's cube tables vertex for vertex; default UVs per
 * direction; a face rotation cutting the corner lut, the negative
 * cut wrapping to its positive twin; cullface routing the bucket
 * while the face keeps its own corners; the namespace-blind
 * texture dedup; the #chain with its self-loop refusal; element
 * rotation; and the transparency ladder — a full unrotated
 * six-face element turning the block translucent, six opaque
 * faces making it opaque, an RGB-mode image opaque by definition,
 * a useless image group skipped whole, and a full block whose
 * texture the pack lacks refused by name (the reference's table
 * read raises there).
 * ======================================================================== */

#define TFIXDIR "build/mesh_fixtures/pack"

static const unsigned char k_opaque_px[16] = {
    255,255,255,255,  255,0,0,255,  0,255,0,255,  0,0,255,255 };
static const unsigned char k_alpha_px[16] = {
    255,255,255,255,  255,0,0,255,  0,255,0,128,  0,0,255,255 };
static const unsigned char k_rgb_px[12] = {
    255,255,255,  255,0,0,  0,255,0,  0,0,255 };

/* Writes a small PNG, creating the parent directories. */
static void put_png(const char *path, int comp, const unsigned char *px)
{
    char dir[512];
    snprintf(dir, sizeof dir, "%s", path);
    char *slash = strrchr(dir, '/');
    if (slash) {
        *slash = '\0';
        CHECK_MSG(mcme_fs_mkdir_p(dir), "mkdir '%s': %s", dir,
                  mcme_last_error());
    }
    CHECK_MSG(stbi_write_png(path, 2, 2, comp, px, 2 * comp),
              "write png '%s'", path);
}

/* A solid PNG of any size (comp 3 = RGB, comp 4 = RGBA), parent
   directories made. */
static void put_solid_png(const char *path, int comp, int w, int h,
                          unsigned char r, unsigned char g,
                          unsigned char b, unsigned char a)
{
    char dir[512];
    snprintf(dir, sizeof dir, "%s", path);
    char *slash = strrchr(dir, '/');
    if (slash) {
        *slash = '\0';
        CHECK_MSG(mcme_fs_mkdir_p(dir), "mkdir '%s': %s", dir,
                  mcme_last_error());
    }
    unsigned char *px = malloc((size_t)w * (size_t)h * (size_t)comp);
    CHECK_MSG(px, "solid png: out of memory");
    for (int i = 0; i < w * h; ++i) {
        px[i * comp + 0] = r;
        px[i * comp + 1] = g;
        px[i * comp + 2] = b;
        if (comp == 4) px[i * comp + 3] = a;
    }
    CHECK_MSG(stbi_write_png(path, w, h, comp, px, w * comp),
              "write png '%s'", path);
    free(px);
}

/* One pixel of an atlas page, NULL off the page. */
static const unsigned char *page_px(const McmeAtlas *a, int x, int y)
{
    if (!a || !a->page.rgba || x < 0 || y < 0 ||
        x >= a->size || y >= a->size)
        return NULL;
    return a->page.rgba + ((size_t)y * (size_t)a->size + (size_t)x) * 4;
}

static void check_px(const McmeAtlas *a, int x, int y, int r, int g,
                     int b, int al, const char *what)
{
    const unsigned char *p = page_px(a, x, y);
    CHECK_MSG(p, "%s: (%d,%d) is off the page", what, x, y);
    CHECK_MSG(p[0] == r && p[1] == g && p[2] == b && p[3] == al,
              "%s: (%d,%d) is %d,%d,%d,%d, want %d,%d,%d,%d",
              what, x, y, p[0], p[1], p[2], p[3], r, g, b, al);
}

static void check_bounds(const McmeAtlas *a, size_t i, double u0,
                         double v0, double u1, double v1,
                         const char *what)
{
    CHECK_MSG(i < a->count, "%s: index %zu of %zu", what, i, a->count);
    const double *b = &a->bounds[i * 4];
    CHECK_MSG(fabs(b[0] - u0) < 1e-9 && fabs(b[1] - v0) < 1e-9 &&
              fabs(b[2] - u1) < 1e-9 && fabs(b[3] - v1) < 1e-9,
              "%s: bounds %g %g %g %g, want %g %g %g %g", what,
              b[0], b[1], b[2], b[3], u0, v0, u1, v1);
}

/* Builds one mesh, clearing a refusal's message the way every error
   path below expects. */
static bool build_mesh(const McmeModelIndex *models,
                       const McmeTextureIndex *tex, const char *ns,
                       const char *path, McmeMesh *out)
{
    if (!mcme_mesh_build(models, tex, ns, path, out)) {
        mcme_clear_error();
        return false;
    }
    return true;
}

static void check_floats(const char *what, const double *got, size_t n,
                         const double *want, size_t wn)
{
    CHECK_MSG(n == wn, "%s: %zu floats, want %zu", what, n, wn);
    for (size_t i = 0; i < n && i < wn; ++i)
        CHECK_MSG(fabs(got[i] - want[i]) < 1e-9,
                  "%s[%zu] = %g, want %g", what, i, got[i], want[i]);
}

static void check_u32s(const char *what, const uint32_t *got, size_t n,
                       const uint32_t *want, size_t wn)
{
    CHECK_MSG(n == wn, "%s: %zu entries, want %zu", what, n, wn);
    for (size_t i = 0; i < n && i < wn; ++i)
        CHECK_MSG(got[i] == want[i], "%s[%zu] = %u, want %u", what, i,
                  got[i], want[i]);
}

static void check_textures(const McmeMesh *m, const char *const *want,
                           size_t wn)
{
    CHECK_MSG(m->texture_count == wn, "%zu textures, want %zu",
              m->texture_count, wn);
    for (size_t i = 0; i < m->texture_count && i < wn; ++i)
        CHECK_MSG(strcmp(m->textures[i], want[i]) == 0,
                  "texture %zu is '%s', want '%s'", i, m->textures[i],
                  want[i]);
}

/* The reference's down/up/north/east/south/west corner lists for a
   box, unit-cube or not, for spot-checking vertices. */
static void box_corners(const double lo[3], const double hi[3],
                        double box[8][3])
{
    for (int i = 0; i < 8; ++i) {
        box[i][0] = ((i >> 2) & 1) ? hi[0] : lo[0];
        box[i][1] = ((i >> 1) & 1) ? hi[1] : lo[1];
        box[i][2] = (i & 1)        ? hi[2] : lo[2];
    }
}

/* The cube face tables, written out the way the reference's lut
   reads them, for expected-vertex arrays. */
static const int k_lut[6][4] = {
    {0, 4, 5, 1}, {3, 7, 6, 2}, {4, 0, 2, 6},
    {5, 4, 6, 7}, {1, 5, 7, 3}, {0, 1, 3, 2}
};

static void face_verts(const double box[8][3], int dir, double out[12])
{
    for (int v = 0; v < 4; ++v)
        for (int j = 0; j < 3; ++j)
            out[v * 3 + j] = box[k_lut[dir][v]][j];
}

static void test_meshes(void)
{
    printf("  resource (meshes)\n");

    /* --- fixtures --- */
    put(TFIXDIR "/pack.mcmeta",
        "{\"pack\":{\"pack_format\":9,\"description\":\"meshes\"}}");
    /* Textures live under the minecraft namespace — a bare face
       reference resolves there, the way the game's own packs do. */
    put_png(TFIXDIR "/assets/minecraft/textures/block/opaque.png", 4,
            k_opaque_px);
    put_png(TFIXDIR "/assets/minecraft/textures/block/alpha.png", 4,
            k_alpha_px);
    put_png(TFIXDIR "/assets/minecraft/textures/block/rgb.png", 3, k_rgb_px);
    /* Under a useless image group: the loader skips it whole. */
    put_png(TFIXDIR "/assets/minecraft/textures/gui/skipme.png", 4,
            k_opaque_px);
    /* Two namespaces one dedup key: the namespace-blind quirk. */
    put_png(TFIXDIR "/assets/minecraft/textures/block/dupe.png", 4,
            k_opaque_px);
    put_png(TFIXDIR "/assets/other/textures/block/dupe.png", 4, k_alpha_px);

    /* The six-face element, one model per transparency outcome. */
#define CUBE6                                                                  \
    "\"faces\":{"                                                              \
      "\"down\":{\"texture\":\"#all\"},\"up\":{\"texture\":\"#all\"},"         \
      "\"north\":{\"texture\":\"#all\"},\"east\":{\"texture\":\"#all\"},"      \
      "\"south\":{\"texture\":\"#all\"},\"west\":{\"texture\":\"#all\"}}}"
    char body[1024];
#define CUBE_MODEL(name, tex)                                                  \
    do {                                                                       \
        snprintf(body, sizeof body,                                            \
                 "{\"textures\":{\"all\":\"" tex "\"},"                        \
                 "\"elements\":[{\"from\":[0,0,0],"                            \
                 "\"to\":[16,16,16]," CUBE6 "]}" );                           \
        put(TFIXDIR "/assets/mc/models/block/" name ".json", body);            \
    } while (0)
    CUBE_MODEL("full_opaque", "block/opaque");
    CUBE_MODEL("full_alpha", "block/alpha");
    CUBE_MODEL("full_rgb", "block/rgb");
    CUBE_MODEL("full_missing", "block/nonexistent");
#undef CUBE_MODEL
#undef CUBE6

    put(TFIXDIR "/assets/mc/models/block/partial.json",
        "{\"textures\":{\"a\":\"block/nonexistent\"},"
        "\"elements\":[{\"from\":[0,0,0],\"to\":[16,8,16],"
        "\"faces\":{\"up\":{\"texture\":\"#a\"},"
        "\"north\":{\"texture\":\"#a\"},\"east\":{\"texture\":\"#a\"}}}]}");

    /* Cullface routes the bucket; the face keeps its own corners. */
    put(TFIXDIR "/assets/mc/models/block/cull.json",
        "{\"textures\":{\"all\":\"block/opaque\"},"
        "\"elements\":[{\"faces\":{"
          "\"down\":{\"texture\":\"#all\"},"
          "\"up\":{\"texture\":\"#all\",\"cullface\":\"down\"},"
          "\"north\":{\"texture\":\"#all\",\"cullface\":\"up\"},"
          "\"east\":{\"texture\":\"#all\",\"cullface\":\"sideways\"},"
          "\"south\":{\"texture\":\"#all\",\"cullface\":42},"
          "\"west\":{\"texture\":\"#all\",\"cullface\":\"west\"}}}]}");

    put(TFIXDIR "/assets/mc/models/block/uvrot.json",
        "{\"textures\":{\"a\":\"block/opaque\"},"
        "\"elements\":[{\"from\":[0,0,0],\"to\":[8,8,8],"
        "\"faces\":{"
          "\"north\":{\"texture\":\"#a\",\"uv\":[0,0,16,8],"
                     "\"rotation\":90},"
          "\"south\":{\"texture\":\"#a\",\"uv\":[0,0,16,8],"
                     "\"rotation\":-90}}}]}");

    put(TFIXDIR "/assets/mc/models/block/chain.json",
        "{\"textures\":{\"a\":\"#b\",\"b\":\"block/opaque\"},"
        "\"elements\":[{\"faces\":{\"north\":{\"texture\":\"#a\"}}}]}");
    put(TFIXDIR "/assets/mc/models/block/selfloop.json",
        "{\"textures\":{\"a\":\"#a\"},"
        "\"elements\":[{\"faces\":{\"north\":{\"texture\":\"#a\"}}}]}");
    put(TFIXDIR "/assets/mc/models/block/sprite.json",
        "{\"textures\":{},"
        "\"elements\":[{\"faces\":{\"north\":"
        "{\"texture\":{\"sprite\":\"block/opaque\"}}}}]}");

    /* Textures without elements: the missing BLOCK. */
    put(TFIXDIR "/assets/mc/models/block/tex_noelem.json",
        "{\"textures\":{\"all\":\"block/opaque\"}}");
    /* An empty elements list counts as none: still the missing block. */
    put(TFIXDIR "/assets/mc/models/block/empty_elems.json",
        "{\"textures\":{\"all\":\"block/opaque\"},\"elements\":[]}");
    /* Particle alone is not "textures": the empty mesh. */
    put(TFIXDIR "/assets/mc/models/block/particle_only.json",
        "{\"textures\":{\"particle\":\"block/opaque\"}}");
    /* A textures value that is not a dict: the reference raises on
       its .items() while folding, mcme's fold drops the value
       (D31's recorded pattern) and the mesh answers empty — until
       elements survive the drop and the mesh refuses instead. */
    put(TFIXDIR "/assets/mc/models/block/texstr.json",
        "{\"textures\":\"side\"}");
    put(TFIXDIR "/assets/mc/models/block/texnum.json",
        "{\"textures\":42}");
    put(TFIXDIR "/assets/mc/models/block/texnull.json",
        "{\"textures\":null}");
    put(TFIXDIR "/assets/mc/models/block/texstr_elem.json",
        "{\"textures\":\"side\",\"elements\":"
        "[{\"faces\":{\"up\":{\"texture\":\"#a\"}}}]}");
    /* Falsy elements the reference cannot iterate — null, false
       zero — raise in its loop only when the early answer has not
       taken them, i.e. when the textures say nothing beyond
       particle; "" runs zero turns either way. */
    put(TFIXDIR "/assets/mc/models/block/elemsnull.json",
        "{\"elements\":null}");
    put(TFIXDIR "/assets/mc/models/block/elemsnum.json",
        "{\"elements\":0}");
    put(TFIXDIR "/assets/mc/models/block/elemsstr.json",
        "{\"textures\":{\"particle\":\"block/opaque\"},"
        "\"elements\":\"\"}");
    /* Elements but no textures dict: the reference's outright index
       raises; the whole mesh fails by name. */
    put(TFIXDIR "/assets/mc/models/block/notex_elem.json",
        "{\"elements\":[{\"from\":[0,0,0],\"to\":[8,8,8]}]}");
    put(TFIXDIR "/assets/mc/models/block/empty.json", "{}");

    put(TFIXDIR "/assets/mc/models/block/elementrot.json",
        "{\"textures\":{\"all\":\"block/opaque\"},"
        "\"elements\":[{\"from\":[0,0,0],\"to\":[16,8,16],"
        "\"rotation\":{\"origin\":[8,8,8],\"axis\":\"y\",\"angle\":90},"
        "\"faces\":{\"up\":{\"texture\":\"#all\"}}}]}");

    /* Namespace-blind dedup: block/dupe and other:block/dupe share
       one slot, the first face naming the path. */
    put(TFIXDIR "/assets/mc/models/block/dupe.json",
        "{\"textures\":{},"
        "\"elements\":[{\"faces\":{"
          "\"up\":{\"texture\":\"block/dupe\"},"
          "\"north\":{\"texture\":\"other:block/dupe\"}}}]}");
    put(TFIXDIR "/assets/mc/models/block/skipme.json",
        "{\"textures\":{\"s\":\"gui/skipme\"},"
        "\"elements\":[{\"faces\":{\"up\":{\"texture\":\"#s\"}}}]}");

    /* The loader gate's own fixture: a pack below format 2. */
    put("build/mesh_fixtures/oldformat/pack.mcmeta",
        "{\"pack\":{\"pack_format\":1,\"description\":\"old\"}}");
    put("build/mesh_fixtures/oldformat/assets/mc/models/block/x.json",
        "{\"textures\":{\"a\":\"block/opaque\"},"
        "\"elements\":[{\"faces\":{\"up\":{\"texture\":\"#a\"}}}]}");
    put_png("build/mesh_fixtures/oldformat/assets/minecraft/textures/"
            "block/opaque.png", 4, k_opaque_px);

    /* --- indexes --- */
    McmeTextureIndex *tex = NULL;
    CHECK_MSG(mcme_texture_index_open(TFIXDIR, &tex),
              "texture index: %s", mcme_last_error());
    McmeModelIndex *models = NULL;
    CHECK_MSG(mcme_model_index_open(TFIXDIR, &models),
              "model index: %s", mcme_last_error());
    if (!tex || !models) { mcme_clear_error(); return; }

    const char *opaque_path =
        TFIXDIR "/assets/minecraft/textures/block/opaque.png";
    const char *alpha_path =
        TFIXDIR "/assets/minecraft/textures/block/alpha.png";
    static const double unit_lo[3] = { 0, 0, 0 }, unit_hi[3] = { 1, 1, 1 };
    double unit[8][3];
    box_corners(unit_lo, unit_hi, unit);
    static const double tverts_default[8] = { 0,1, 1,1, 1,0, 0,0 };

    /* --- a full opaque cube --- */
    {
        McmeMesh m;
        CHECK(build_mesh(models, tex, "mc", "block/full_opaque", &m));
        CHECK_MSG(m.transparency == MCME_TRANSPARENCY_OPAQUE,
                  "full_opaque transparency %d", m.transparency);
        check_textures(&m, (const char *const[]){ opaque_path }, 1);
        /* No face names a cullface, so all six share the none
           bucket, in the model's own file order. */
        for (int d = 0; d < 6; ++d) CHECK(!m.present[d]);
        CHECK(m.present[MCME_CULL_NONE]);
        CHECK(m.buckets[MCME_CULL_NONE].face_count == 6);
        CHECK(m.buckets[MCME_CULL_NONE].vert_count == 24);
        double want72[72];
        static const uint32_t tri[6] = { 0, 1, 2, 0, 2, 3 };
        uint32_t want_faces[36];
        double want48[48];
        for (int d = 0; d < 6; ++d) {
            face_verts(unit, d, want72 + d * 12);
            for (int j = 0; j < 8; ++j)
                want48[d * 8 + j] = tverts_default[j];
            for (int j = 0; j < 6; ++j)
                want_faces[d * 6 + j] = tri[j] + (uint32_t)(d * 4);
        }
        check_floats("full_opaque verts", m.buckets[MCME_CULL_NONE].verts,
                     72, want72, 72);
        check_floats("full_opaque tverts", m.buckets[MCME_CULL_NONE].tverts,
                     48, want48, 48);
        for (int i = 0; i < 72; ++i)
            CHECK(m.buckets[MCME_CULL_NONE].tint_verts[i] == 1.0);
        check_u32s("full_opaque faces", m.buckets[MCME_CULL_NONE].faces, 36,
                   want_faces, 36);
        for (int f = 0; f < 6; ++f) {
            CHECK(m.buckets[MCME_CULL_NONE].texture_indexes[f * 2] == 0);
            CHECK(m.buckets[MCME_CULL_NONE].texture_indexes[f * 2 + 1] == 0);
        }
        mcme_mesh_free(&m);
    }

    /* --- the alpha and RGB ladders --- */
    {
        McmeMesh m;
        CHECK(build_mesh(models, tex, "mc", "block/full_alpha", &m));
        CHECK(m.transparency == MCME_TRANSPARENCY_TRANSLUCENT);
        check_textures(&m, (const char *const[]){ alpha_path }, 1);
        mcme_mesh_free(&m);
        /* An RGB-mode image is opaque by definition — only RGBA
           earns the alpha scan. */
        CHECK(build_mesh(models, tex, "mc", "block/full_rgb", &m));
        CHECK_MSG(m.transparency == MCME_TRANSPARENCY_OPAQUE,
                  "full_rgb transparency %d", m.transparency);
        mcme_mesh_free(&m);
    }

    /* --- a full block whose texture the pack lacks: refused --- */
    {
        McmeMesh m;
        CHECK(!build_mesh(models, tex, "mc", "block/full_missing", &m));
        CHECK(!m.present[0]);
        mcme_mesh_free(&m);
    }

    /* --- a partial block: missing textures never checked --- */
    {
        McmeMesh m;
        CHECK(build_mesh(models, tex, "mc", "block/partial", &m));
        CHECK(m.transparency == MCME_TRANSPARENCY_PARTIAL);
        check_textures(&m, (const char *const[]){ "missing_no" }, 1);
        for (int d = 0; d < 6; ++d) CHECK(!m.present[d]);
        CHECK(m.present[MCME_CULL_NONE]);
        CHECK(m.buckets[MCME_CULL_NONE].face_count == 3);
        static const double slab_lo[3] = { 0, 0, 0 }, slab_hi[3] = {1, .5, 1};
        double slab[8][3];
        box_corners(slab_lo, slab_hi, slab);
        /* The three faces, up north east, in file order. */
        double want36[36];
        face_verts(slab, 1, want36);
        face_verts(slab, 2, want36 + 12);
        face_verts(slab, 3, want36 + 24);
        check_floats("partial verts", m.buckets[MCME_CULL_NONE].verts, 36,
                     want36, 36);
        /* The box-derived default UVs: up [from0,from2,to0,to2]/16,
           north and east [from*,16-to1,to*,16-from1]/16. */
        static const double want24[24] = {
            0,1, 1,1, 1,0, 0,0,   0,1, 1,1, 1,.5, 0,.5,   0,1, 1,1, 1,.5, 0,.5 };
        check_floats("partial tverts", m.buckets[MCME_CULL_NONE].tverts, 24,
                     want24, 24);
        mcme_mesh_free(&m);
    }

    /* --- cullface routing --- */
    {
        McmeMesh m;
        CHECK(build_mesh(models, tex, "mc", "block/cull", &m));
        CHECK(m.transparency == MCME_TRANSPARENCY_OPAQUE);
        double want[12];
        /* The bucket named by cullface; the face keeps the corners
           of its own direction. */
        face_verts(unit, 1, want);   /* face up    -> bucket down */
        check_floats("cull down", m.buckets[MCME_CULL_DOWN].verts, 12,
                     want, 12);
        face_verts(unit, 2, want);   /* face north -> bucket up  */
        check_floats("cull up", m.buckets[MCME_CULL_UP].verts, 12,
                     want, 12);
        face_verts(unit, 5, want);   /* face west  -> bucket west */
        check_floats("cull west", m.buckets[MCME_CULL_WEST].verts, 12,
                     want, 12);
        /* No cullface, an unknown direction and a non-string: all
           three share the none bucket, in file order. */
        CHECK(m.present[MCME_CULL_NONE]);
        CHECK_MSG(m.buckets[MCME_CULL_NONE].face_count == 3,
                  "cull none: %zu faces, want 3",
                  m.buckets[MCME_CULL_NONE].face_count);
        double expect_none[36];
        face_verts(unit, 0, expect_none);           /* down */
        face_verts(unit, 3, expect_none + 12);      /* east */
        face_verts(unit, 4, expect_none + 24);      /* south */
        check_floats("cull none", m.buckets[MCME_CULL_NONE].verts, 36,
                     expect_none, 36);
        mcme_mesh_free(&m);
    }

    /* --- face rotation cuts the lut, and the negative wraps --- */
    {
        McmeMesh m;
        CHECK(build_mesh(models, tex, "mc", "block/uvrot", &m));
        CHECK(m.transparency == MCME_TRANSPARENCY_PARTIAL);
        static const double t90[8]  = { 1,.5, 1,0, 0,0, 0,.5 };
        static const double tm90[8] = { 0,0, 0,.5, 1,.5, 1,0 };
        check_floats("uvrot north", m.buckets[MCME_CULL_NONE].tverts, 8,
                     t90, 8);
        check_floats("uvrot south", m.buckets[MCME_CULL_NONE].tverts + 8,
                     8, tm90, 8);
        static const uint32_t faces2[12] = {
            0,1,2,0,2,3, 4,5,6,4,6,7 };
        check_u32s("uvrot faces", m.buckets[MCME_CULL_NONE].faces, 12,
                   faces2, 12);
        mcme_mesh_free(&m);
    }

    /* --- the #chain and the sprite pass-through --- */
    {
        McmeMesh m;
        CHECK(build_mesh(models, tex, "mc", "block/chain", &m));
        CHECK(m.transparency == MCME_TRANSPARENCY_PARTIAL);
        check_textures(&m, (const char *const[]){ opaque_path }, 1);
        mcme_mesh_free(&m);
        CHECK(build_mesh(models, tex, "mc", "block/sprite", &m));
        check_textures(&m, (const char *const[]){ opaque_path }, 1);
        mcme_mesh_free(&m);
        /* A self-referencing chain: refused by name. */
        CHECK(!build_mesh(models, tex, "mc", "block/selfloop", &m));
        CHECK(!m.present[0]);
        mcme_mesh_free(&m);
    }

    /* --- the missing BLOCK for textures without elements --- */
    {
        McmeMesh m;
        CHECK(build_mesh(models, tex, "mc", "block/tex_noelem", &m));
        CHECK_MSG(m.transparency == MCME_TRANSPARENCY_OPAQUE,
                  "missing block transparency %d", m.transparency);
        check_textures(&m, (const char *const[]){ "missing_no" }, 1);
        double want[12];
        face_verts(unit, 0, want);
        check_floats("missing block down", m.buckets[MCME_CULL_DOWN].verts,
                     12, want, 12);
        check_floats("missing block down tverts",
                     m.buckets[MCME_CULL_DOWN].tverts, 8, tverts_default, 8);
        mcme_mesh_free(&m);
        /* An empty elements list counts as none: the same block. */
        CHECK(build_mesh(models, tex, "mc", "block/empty_elems", &m));
        CHECK(m.transparency == MCME_TRANSPARENCY_OPAQUE);
        check_textures(&m, (const char *const[]){ "missing_no" }, 1);
        mcme_mesh_free(&m);
    }

    /* --- the empty mesh answers --- */
    {
        McmeMesh m;
        CHECK(build_mesh(models, tex, "mc", "block/particle_only", &m));
        CHECK(m.transparency == MCME_TRANSPARENCY_PARTIAL);
        CHECK(m.texture_count == 0);
        for (int d = 0; d < 7; ++d) CHECK(!m.present[d]);
        mcme_mesh_free(&m);
        CHECK(build_mesh(models, tex, "mc", "block/empty", &m));
        CHECK(m.texture_count == 0);
        mcme_mesh_free(&m);
        CHECK(build_mesh(models, tex, "mc", "block/absent", &m));
        CHECK(m.texture_count == 0);
        mcme_mesh_free(&m);
    }

    /* --- elements without a textures dict: refused --- */
    {
        McmeMesh m;
        CHECK(!build_mesh(models, tex, "mc", "block/notex_elem", &m));
        mcme_mesh_free(&m);
    }

    /* --- the textures value's garbage, dropped by the fold --- */
    {
        McmeMesh m;
        /* The reference raises on a textures value that is not a
           dict while FOLDING (its `.items()`); mcme's fold drops
           such a value the way it drops a non-string parent (the
           D31 record) and the mesh answers the empty document —
           the recorded difference, asserted below by the pin. */
        CHECK(build_mesh(models, tex, "mc", "block/texstr", &m));
        CHECK(m.transparency == MCME_TRANSPARENCY_PARTIAL);
        CHECK(m.texture_count == 0);
        for (int d = 0; d < 7; ++d) CHECK(!m.present[d]);
        mcme_mesh_free(&m);
        CHECK(build_mesh(models, tex, "mc", "block/texnum", &m));
        CHECK(m.texture_count == 0);
        mcme_mesh_free(&m);
        CHECK(build_mesh(models, tex, "mc", "block/texnull", &m));
        CHECK(m.texture_count == 0);
        mcme_mesh_free(&m);
        /* Dropped textures but kept elements: the mesh refuses on
           the missing dict — the reference failed earlier, folding.
           Both fail. */
        CHECK(!build_mesh(models, tex, "mc", "block/texstr_elem", &m));
        mcme_mesh_free(&m);
        /* Falsy elements the reference cannot iterate: null, false
           and zero raise in its loop once the early answer has not
           taken them; "" runs zero turns and answers empty. */
        CHECK(!build_mesh(models, tex, "mc", "block/elemsnull", &m));
        mcme_mesh_free(&m);
        CHECK(!build_mesh(models, tex, "mc", "block/elemsnum", &m));
        mcme_mesh_free(&m);
        CHECK(build_mesh(models, tex, "mc", "block/elemsstr", &m));
        CHECK(m.texture_count == 0);
        mcme_mesh_free(&m);
    }

    /* --- element rotation --- */
    {
        McmeMesh m;
        CHECK(build_mesh(models, tex, "mc", "block/elementrot", &m));
        CHECK(m.transparency == MCME_TRANSPARENCY_PARTIAL);
        /* The face has no cullface: the none bucket. */
        CHECK(m.present[MCME_CULL_NONE]);
        CHECK(!m.present[MCME_CULL_UP]);
        /* The slab's up face, swung 90 degrees about the origin
           (8,8,8)/16 — the cube's centre axis, so the face lands
           on the box's far corners. */
        static const double want[12] = {
            1, .5, 1,   1, .5, 0,   0, .5, 0,   0, .5, 1 };
        check_floats("elementrot", m.buckets[MCME_CULL_NONE].verts, 12,
                     want, 12);
        mcme_mesh_free(&m);
    }

    /* --- the namespace-blind dedup --- */
    {
        McmeMesh m;
        CHECK(build_mesh(models, tex, "mc", "block/dupe", &m));
        /* One slot for block/dupe and other:block/dupe, the first
           face naming the path — minecraft's own file. */
        check_textures(&m, (const char *const[]){
            TFIXDIR "/assets/minecraft/textures/block/dupe.png" }, 1);
        CHECK(m.buckets[MCME_CULL_NONE].face_count == 2);
        for (int i = 0; i < 4; ++i)
            CHECK(m.buckets[MCME_CULL_NONE].texture_indexes[i] == 0);
        mcme_mesh_free(&m);
    }

    /* --- a useless image group is not in the table --- */
    {
        McmeMesh m;
        CHECK(build_mesh(models, tex, "mc", "block/skipme", &m));
        check_textures(&m, (const char *const[]){ "missing_no" }, 1);
        mcme_mesh_free(&m);
    }

    /* --- the loader gate --- */
    CHECK(mcme_pack_loads_assets(TFIXDIR));
    CHECK(!mcme_pack_loads_assets("build/pack_fixtures/empty"));
    CHECK(!mcme_pack_loads_assets("build/mesh_fixtures/oldformat"));
    {
        McmeTextureIndex *t2 = NULL;
        CHECK(mcme_texture_index_open("build/mesh_fixtures/oldformat",
                                      &t2));
        if (t2) {
            CHECK_MSG(strcmp(mcme_texture_path(t2, "mc", "block/opaque"),
                             "missing_no") == 0,
                      "a format-1 pack should hold no textures");
            mcme_texture_index_close(t2);
        }
        McmeModelIndex *m2 = NULL;
        CHECK(mcme_model_index_open("build/mesh_fixtures/oldformat", &m2));
        if (m2) {
            struct cJSON *doc = NULL;
            CHECK(mcme_model_load(m2, "mc", "block/x", &doc));
            CHECK(doc != NULL && doc->child == NULL);
            cJSON_Delete(doc);
            mcme_model_index_close(m2);
        }
    }

    /* --- null arguments --- */
    CHECK(!mcme_texture_index_open(NULL, &tex));
    CHECK(!mcme_texture_index_open(TFIXDIR, NULL));
    CHECK(strcmp(mcme_texture_path(tex, NULL, "block/opaque"),
                 "missing_no") == 0);
    CHECK(!mcme_texture_transparent(tex, NULL));
    CHECK(!mcme_texture_transparent(NULL, opaque_path));
    CHECK(!mcme_texture_transparent(tex, "missing_no"));
    {
        McmeMesh m;
        CHECK(!mcme_mesh_build(models, tex, "mc", NULL, &m));
        CHECK(!mcme_mesh_build(NULL, tex, "mc", "block/chain", &m));
        CHECK(!mcme_mesh_build(models, NULL, "mc", "block/chain", &m));
        CHECK(!mcme_mesh_build(models, tex, "mc", "block/chain", NULL));
    }
    mcme_mesh_free(NULL);
    mcme_texture_index_close(NULL);

    mcme_texture_index_close(tex);
    mcme_model_index_close(models);
    mcme_clear_error();
}

/* ========================================================================
 * Block-level meshes (D33): the reference's `_get_model` — the
 * selection walked, each winning apply meshed and turned by its
 * x/y, refusals skipped, multipart merged.
 * ======================================================================== */

#define BFIXDIR "build/blockmesh_fixtures/pack"

static const char *const k_bmesh_dirs[7] = {
    "down", "up", "north", "east", "south", "west", "none"
};

/* Checks that `m` is the reference's missing BLOCK: the unit cube
   of the missing image, one face per cull direction, opaque. */
static void check_missing_block(const McmeMesh *m, const char *what)
{
    static const double lo[3] = { 0, 0, 0 }, hi[3] = { 1, 1, 1 };
    double unit[8][3];
    box_corners(lo, hi, unit);

    CHECK_MSG(m->transparency == MCME_TRANSPARENCY_OPAQUE,
              "%s: transparency %d, want the missing block's opaque",
              what, m->transparency);
    check_textures(m, (const char *const[]){ "missing_no" }, 1);
    for (int d = 0; d < 6; ++d) {
        CHECK_MSG(m->present[d], "%s: the missing block holds no '%s' "
                  "face", what, k_bmesh_dirs[d]);
        const McmeMeshBucket *b = &m->buckets[d];
        CHECK_MSG(b->face_count == 1 && b->vert_count == 4,
                  "%s: '%s' has %zu faces, want 1", what,
                  k_bmesh_dirs[d], b->face_count);
        double want[12];
        face_verts(unit, d, want);
        check_floats(what, b->verts, 12, want, 12);
    }
    for (int d = 6; d < 7; ++d)
        CHECK_MSG(!m->present[d], "%s: the missing block holds a "
                  "'%s' face", what, k_bmesh_dirs[d]);
}

static void test_blockmeshes(void)
{
    printf("  resource (blockmeshes)\n");

    /* --- fixtures --- */
    put(BFIXDIR "/pack.mcmeta",
        "{\"pack\":{\"pack_format\":9,\"description\":\"blockmeshes\"}}");
    put_png(BFIXDIR "/assets/minecraft/textures/block/opaque.png", 4,
            k_opaque_px);
    put_png(BFIXDIR "/assets/minecraft/textures/block/alpha.png", 4,
            k_alpha_px);

    /* One up face on a partial cube: the none bucket, and geometry
       that moves visibly under a turn. */
    put(BFIXDIR "/assets/mc/models/block/tex.json",
        "{\"textures\":{\"a\":\"block/alpha\"},"
        "\"elements\":[{\"from\":[2,2,2],\"to\":[14,14,14],"
        "\"faces\":{\"up\":{\"texture\":\"#a\",\"uv\":[0,0,16,16]}}}]}");
    /* The same shape on the other texture, for merge probes. */
    put(BFIXDIR "/assets/mc/models/block/texb.json",
        "{\"textures\":{\"a\":\"block/opaque\"},"
        "\"elements\":[{\"from\":[2,2,2],\"to\":[14,14,14],"
        "\"faces\":{\"up\":{\"texture\":\"#a\",\"uv\":[0,0,16,16]}}}]}");
    /* A full cube culling every direction: six buckets to remap. */
    put(BFIXDIR "/assets/mc/models/block/cull6.json",
        "{\"textures\":{\"a\":\"block/opaque\"},"
        "\"elements\":[{\"from\":[0,0,0],\"to\":[16,16,16],"
        "\"faces\":{"
          "\"down\":{\"texture\":\"#a\",\"cullface\":\"down\"},"
          "\"up\":{\"texture\":\"#a\",\"cullface\":\"up\"},"
          "\"north\":{\"texture\":\"#a\",\"cullface\":\"north\"},"
          "\"east\":{\"texture\":\"#a\",\"cullface\":\"east\"},"
          "\"south\":{\"texture\":\"#a\",\"cullface\":\"south\"},"
          "\"west\":{\"texture\":\"#a\",\"cullface\":\"west\"}}}]}");
    /* A full cube on a texture the pack lacks: it refuses to mesh,
       the reference's transparency-table read raising. */
    put(BFIXDIR "/assets/mc/models/block/full_missing.json",
        "{\"textures\":{\"a\":\"block/nonexistent\"},"
        "\"elements\":[{\"from\":[0,0,0],\"to\":[16,16,16],"
        "\"faces\":{"
          "\"down\":{\"texture\":\"#a\"},\"up\":{\"texture\":\"#a\"},"
          "\"north\":{\"texture\":\"#a\"},\"east\":{\"texture\":\"#a\"},"
          "\"south\":{\"texture\":\"#a\"},\"west\":{\"texture\":\"#a\"}}}]}");
    /* Six faces, no cullface: all six in the one none bucket, so a
       turn walks several faces inside one bucket. */
    put(BFIXDIR "/assets/mc/models/block/multi6.json",
        "{\"textures\":{\"a\":\"block/opaque\"},"
        "\"elements\":[{\"from\":[0,0,0],\"to\":[16,16,16],"
        "\"faces\":{"
          "\"down\":{\"texture\":\"#a\"},\"up\":{\"texture\":\"#a\"},"
          "\"north\":{\"texture\":\"#a\"},\"east\":{\"texture\":\"#a\"},"
          "\"south\":{\"texture\":\"#a\"},\"west\":{\"texture\":\"#a\"}}}]}");

    put(BFIXDIR "/assets/mc/blockstates/rot_x.json",
        "{\"variants\":{\"\":{\"model\":\"mc:block/tex\",\"x\":90}}}");
    put(BFIXDIR "/assets/mc/blockstates/rot_y.json",
        "{\"variants\":{\"\":{\"model\":\"mc:block/cull6\",\"y\":90}}}");
    put(BFIXDIR "/assets/mc/blockstates/rot_xy.json",
        "{\"variants\":{\"\":{\"model\":\"mc:block/cull6\","
        "\"x\":90,\"y\":90}}}");
    /* A turn over a bucket holding SEVERAL faces: the none bucket's
       six faces must all turn, each in place. */
    put(BFIXDIR "/assets/mc/blockstates/rot_multi.json",
        "{\"variants\":{\"\":{\"model\":\"mc:block/multi6\",\"y\":90}}}");
    /* y 450 is step 5 — out of the cull map, and with rotx zero the
       reference's guard returns the mesh untouched. */
    put(BFIXDIR "/assets/mc/blockstates/rot_ywide.json",
        "{\"variants\":{\"\":{\"model\":\"mc:block/tex\",\"y\":450}}}");
    /* x 360 is step 4 — out of the cull map with a nonzero rotx,
       where the reference's lookup raises: this apply is skipped,
       nothing matches after it, and the missing block answers. */
    put(BFIXDIR "/assets/mc/blockstates/rot_xbad.json",
        "{\"variants\":{\"\":{\"model\":\"mc:block/tex\",\"x\":360}}}");
    /* The first matching key refuses to mesh; the "" key after it
       must still answer. */
    put(BFIXDIR "/assets/mc/blockstates/rot_skip.json",
        "{\"variants\":{"
          "\"a=1\":{\"model\":\"mc:block/full_missing\"},"
          "\"\":{\"model\":\"mc:block/tex\",\"x\":90}}}");
    put(BFIXDIR "/assets/mc/blockstates/rot_allrefuse.json",
        "{\"variants\":{\"\":{\"model\":\"mc:block/full_missing\","
        "\"x\":90}}}");
    /* No model at all: the missing block stands in, unturned. */
    put(BFIXDIR "/assets/mc/blockstates/rot_nomodel.json",
        "{\"variants\":{\"\":{\"x\":90}}}");
    /* A list apply answers its first element. */
    put(BFIXDIR "/assets/mc/blockstates/rot_list.json",
        "{\"variants\":{\"\":[{\"model\":\"mc:block/tex\"},"
        "{\"model\":\"mc:block/texb\"}]}}");
    /* uvlock is read and ignored — the reference's standing TODO. */
    put(BFIXDIR "/assets/mc/blockstates/rot_uv.json",
        "{\"variants\":{\"\":{\"model\":\"mc:block/tex\","
        "\"uvlock\":true}}}");
    put(BFIXDIR "/assets/mc/blockstates/neither.json", "{\"foo\":1}");

    /* Multipart: two applies and one behind a when that only a
       matching block reaches, the third turned as it merges. */
    put(BFIXDIR "/assets/mc/blockstates/merge.json",
        "{\"multipart\":["
          "{\"apply\":{\"model\":\"mc:block/texb\"}},"
          "{\"when\":{\"a\":\"1\"},\"apply\":{\"model\":\"mc:block/tex\"}},"
          "{\"apply\":{\"model\":\"mc:block/texb\",\"y\":90}}]}");
    put(BFIXDIR "/assets/mc/blockstates/merge_empty.json",
        "{\"multipart\":[{\"when\":{\"a\":\"1\"},"
        "\"apply\":{\"model\":\"mc:block/tex\"}}]}");

    /* The block tint's own ground (D44): a full cube whose UP face
       alone declares a tintindex — the tint lands on that one
       bucket and nowhere else. */
    put(BFIXDIR "/assets/mc/models/block/tintface.json",
        "{\"textures\":{\"a\":\"block/opaque\"},"
        "\"elements\":[{\"from\":[0,0,0],\"to\":[16,16,16],"
        "\"faces\":{"
          "\"down\":{\"texture\":\"#a\",\"cullface\":\"down\"},"
          "\"up\":{\"texture\":\"#a\",\"cullface\":\"up\","
                  "\"tintindex\":0},"
          "\"north\":{\"texture\":\"#a\",\"cullface\":\"north\"},"
          "\"east\":{\"texture\":\"#a\",\"cullface\":\"east\"},"
          "\"south\":{\"texture\":\"#a\",\"cullface\":\"south\"},"
          "\"west\":{\"texture\":\"#a\",\"cullface\":\"west\"}}}]}");
    /* Three blocks under their own (minecraft) namespace, where the
       tint's branches live: the name table, the power state, and a
       multipart stem whose age is absent — the lookup's raise. */
    put(BFIXDIR "/assets/minecraft/blockstates/grass_block.json",
        "{\"variants\":{\"\":{\"model\":\"mc:block/tintface\"}}}");
    put(BFIXDIR "/assets/minecraft/blockstates/redstone_wire.json",
        "{\"variants\":{\"\":{\"model\":\"mc:block/tintface\"}}}");
    put(BFIXDIR "/assets/minecraft/blockstates/melon_stem.json",
        "{\"multipart\":["
          "{\"apply\":{\"model\":\"mc:block/tintface\"}},"
          "{\"apply\":{\"model\":\"mc:block/tex\"}}]}");

    /* --- indexes --- */
    McmeBlockstateIndex *states = NULL;
    CHECK_MSG(mcme_blockstate_index_open(BFIXDIR, &states),
              "blockstate index: %s", mcme_last_error());
    McmeTextureIndex *tex = NULL;
    CHECK_MSG(mcme_texture_index_open(BFIXDIR, &tex),
              "texture index: %s", mcme_last_error());
    McmeModelIndex *models = NULL;
    CHECK_MSG(mcme_model_index_open(BFIXDIR, &models),
              "model index: %s", mcme_last_error());
    if (!states || !tex || !models) { mcme_clear_error(); return; }

    const char *opaque_path =
        BFIXDIR "/assets/minecraft/textures/block/opaque.png";
    const char *alpha_path =
        BFIXDIR "/assets/minecraft/textures/block/alpha.png";
    static const double tverts_default[8] = { 0,1, 1,1, 1,0, 0,0 };

    /* Helper: one block query, refusal-free. */
#define BLOCKMESH(stem, qs, out) \
    do { \
        McmeState qs_[64]; \
        size_t qn_ = 0; \
        for (const char *const *p_ = (qs); p_ && *p_; p_ += 2) { \
            qs_[qn_].name = p_[0]; \
            qs_[qn_].value = p_[1]; \
            qn_++; \
        } \
        CHECK_MSG(mcme_mesh_for_block(states, models, tex, "mc", (stem), \
                                      qs_, qn_, (out)), \
                  "%s: %s", (stem), mcme_last_error()); \
    } while (0)

    /* --- the block tint lookup itself (D44) --- */
    {
        double rgb[3];
        static const McmeState p5[] = { { "power", "5" } };
        static const McmeState p15[] = { { "power", "15" } };
        static const McmeState pbad[] = { { "power", "99" } };
        static const McmeState ptyped[] = { { "power", NULL } };
        static const McmeState age7[] = { { "age", "7" } };
        static const McmeState age8[] = { { "age", "8" } };

        CHECK(mcme_block_tint("minecraft", "redstone_wire", p5, 1, rgb));
        check_floats("redstone power 5", rgb, 3,
                     (const double[]){ 153.0 / 255.0, 0, 0 }, 3);
        CHECK(mcme_block_tint("minecraft", "redstone_wire", p15, 1, rgb));
        check_floats("redstone power 15", rgb, 3,
                     (const double[]){ 1.0, 50.0 / 255.0, 0 }, 3);
        CHECK(!mcme_block_tint("minecraft", "redstone_wire", NULL, 0, rgb));
        CHECK(!mcme_block_tint("minecraft", "redstone_wire", pbad, 1, rgb));
        CHECK(!mcme_block_tint("minecraft", "redstone_wire", ptyped, 1,
                               rgb));
        CHECK(mcme_block_tint("minecraft", "melon_stem", age7, 1, rgb));
        check_floats("stem age 7", rgb, 3,
                     (const double[]){ 224.0 / 255.0, 199.0 / 255.0,
                                       28.0 / 255.0 }, 3);
        CHECK(!mcme_block_tint("minecraft", "melon_stem", NULL, 0, rgb));
        CHECK(!mcme_block_tint("minecraft", "melon_stem", age8, 1, rgb));
        CHECK(mcme_block_tint("minecraft", "grass_block", NULL, 0, rgb));
        check_floats("grass_block name", rgb, 3,
                     (const double[]){ 146.0 / 255.0, 188.0 / 255.0,
                                       88.0 / 255.0 }, 3);
        CHECK(mcme_block_tint("minecraft", "birch_leaves", NULL, 0, rgb));
        check_floats("birch_leaves name", rgb, 3,
                     (const double[]){ 128.0 / 255.0, 167.0 / 255.0,
                                       85.0 / 255.0 }, 3);
        CHECK(mcme_block_tint("minecraft", "oak_leaves", NULL, 0, rgb));
        check_floats("oak_leaves name", rgb, 3,
                     (const double[]){ 119.0 / 255.0, 171.0 / 255.0,
                                       47.0 / 255.0 }, 3);
        /* The name table never raises — absent, it defaults. */
        CHECK(mcme_block_tint("minecraft", "stone", NULL, 0, rgb));
        check_floats("stone default", rgb, 3,
                     (const double[]){ 1, 1, 1 }, 3);
        /* Outside the minecraft namespace the state branches never
           run — mc:grass_block is a different block. */
        CHECK(mcme_block_tint("mc", "grass_block", NULL, 0, rgb));
        check_floats("mc grass_block default", rgb, 3,
                     (const double[]){ 1, 1, 1 }, 3);
        /* A NULL namespace reads as minecraft. */
        CHECK(mcme_block_tint(NULL, "grass_block", NULL, 0, rgb));
        check_floats("null ns grass_block", rgb, 3,
                     (const double[]){ 146.0 / 255.0, 188.0 / 255.0,
                                       88.0 / 255.0 }, 3);
    }

    /* --- the tintindex face takes the build's tint (D44) --- */
    {
        McmeMesh m;
        static const double tint[3] = { 0.5, 0.25, 0.125 };
        CHECK_MSG(mcme_mesh_build_tinted(models, tex, "mc",
                                         "block/tintface", tint, &m),
                  "tinted build: %s", mcme_last_error());
        CHECK(m.present[MCME_CULL_UP]);
        for (int i = 0; i < 12; ++i)
            CHECK_MSG(fabs(m.buckets[MCME_CULL_UP].tint_verts[i] -
                           tint[i % 3]) < 1e-12,
                      "up tint vert %d is %g", i,
                      m.buckets[MCME_CULL_UP].tint_verts[i]);
        for (int d = MCME_CULL_DOWN; d <= MCME_CULL_WEST; ++d) {
            if (d == MCME_CULL_UP) continue;
            CHECK(m.present[d]);
            for (int i = 0; i < 12; ++i)
                CHECK_MSG(m.buckets[d].tint_verts[i] == 1.0,
                          "bucket %d tint vert %d is %g", d, i,
                          m.buckets[d].tint_verts[i]);
        }
        mcme_mesh_free(&m);
        /* The wrapper hands (1,1,1): the tintindex face carries no
           colour there either. */
        CHECK(build_mesh(models, tex, "mc", "block/tintface", &m));
        CHECK(m.present[MCME_CULL_UP]);
        for (int i = 0; i < 12; ++i)
            CHECK(m.buckets[MCME_CULL_UP].tint_verts[i] == 1.0);
        mcme_mesh_free(&m);
    }

    /* --- the tint through the block-level walk (D44) --- */
    {
        McmeMesh m, want;
        static const McmeState p5[] = { { "power", "5" } };

        /* The power branch answers, the tintindex face carries it. */
        CHECK_MSG(mcme_mesh_for_block(states, models, tex, "minecraft",
                                      "redstone_wire", p5, 1, &m),
                  "redstone powered: %s", mcme_last_error());
        CHECK(m.present[MCME_CULL_UP]);
        {
            static const double want[3] = { 153.0 / 255.0, 0, 0 };
            for (int i = 0; i < 12; ++i)
                CHECK(fabs(m.buckets[MCME_CULL_UP].tint_verts[i] -
                           want[i % 3]) < 1e-12);
        }
        mcme_mesh_free(&m);

        /* The name branch, through the same walk. */
        CHECK(mcme_mesh_for_block(states, models, tex, "minecraft",
                                  "grass_block", NULL, 0, &m));
        CHECK(m.present[MCME_CULL_UP]);
        {
            static const double want[3] = { 146.0 / 255.0,
                                            188.0 / 255.0,
                                            88.0 / 255.0 };
            for (int i = 0; i < 12; ++i)
                CHECK(fabs(m.buckets[MCME_CULL_UP].tint_verts[i] -
                           want[i % 3]) < 1e-12);
        }
        mcme_mesh_free(&m);

        /* The lookup raises without the power state: every apply
           refused, the missing block answers for a variant. */
        CHECK(mcme_mesh_for_block(states, models, tex, "minecraft",
                                  "redstone_wire", NULL, 0, &m));
        CHECK(mcme_mesh_missing_block(&want));
        CHECK_MSG(m.transparency == want.transparency,
                  "raised transparency %d want %d", m.transparency,
                  want.transparency);
        for (int d = 0; d < 7; ++d) {
            CHECK_MSG(m.present[d] == want.present[d],
                      "raised bucket %d present %d want %d", d,
                      m.present[d], want.present[d]);
            if (!want.present[d]) continue;
            CHECK_MSG(m.buckets[d].vert_count ==
                      want.buckets[d].vert_count,
                      "raised bucket %d verts %zu want %zu", d,
                      m.buckets[d].vert_count,
                      want.buckets[d].vert_count);
            for (size_t i = 0; i < m.buckets[d].vert_count * 3; ++i)
                CHECK_MSG(m.buckets[d].tint_verts[i] == 1.0,
                          "raised tint vert %zu is %g", i,
                          m.buckets[d].tint_verts[i]);
        }
        mcme_mesh_free(&want);
        mcme_mesh_free(&m);

        /* The same raise on a multipart stem: every case refused,
           the empty merge of none answers. */
        CHECK(mcme_mesh_for_block(states, models, tex, "minecraft",
                                  "melon_stem", NULL, 0, &m));
        for (int d = 0; d < 7; ++d) CHECK(!m.present[d]);
        mcme_mesh_free(&m);
    }

    /* --- x 90: the up face turns, the none bucket stays --- */
    {
        McmeMesh m;
        BLOCKMESH("rot_x", NULL, &m);
        CHECK_MSG(m.transparency == MCME_TRANSPARENCY_PARTIAL,
                  "rot_x transparency %d", m.transparency);
        check_textures(&m, (const char *const[]){ alpha_path }, 1);
        for (int d = 0; d < 6; ++d) CHECK(!m.present[d]);
        CHECK(m.present[MCME_CULL_NONE]);
        /* The X pass maps (x, y, z) to (x, z, 1 - y) about the
           block centre — the up face lands on the block's north
           side while its bucket name stays "none". */
        static const double want[12] = {
            0.125, 0.875, 0.125,  0.875, 0.875, 0.125,
            0.875, 0.125, 0.125,  0.125, 0.125, 0.125 };
        check_floats("rot_x verts", m.buckets[MCME_CULL_NONE].verts,
                     12, want, 12);
        check_floats("rot_x tverts", m.buckets[MCME_CULL_NONE].tverts,
                     8, tverts_default, 8); /* uvlock's no-op */
        check_u32s("rot_x faces", m.buckets[MCME_CULL_NONE].faces,
                   6, (const uint32_t[]){ 0, 1, 2, 0, 2, 3 }, 6);
        mcme_mesh_free(&m);
    }

    /* --- y 90 over the culling cube: buckets turn --- */
    {
        McmeMesh m;
        BLOCKMESH("rot_y", NULL, &m);
        CHECK(m.transparency == MCME_TRANSPARENCY_OPAQUE);
        check_textures(&m, (const char *const[]){ opaque_path }, 1);
        /* The horizontal ring turns: north's face now culls east,
           east south, south west, west north; the poles stay. */
        static const int want_dir[7] = {
            MCME_CULL_DOWN, MCME_CULL_UP, MCME_CULL_EAST, MCME_CULL_SOUTH,
            MCME_CULL_WEST, MCME_CULL_NORTH, MCME_CULL_NONE };
        for (int d = 0; d < 7; ++d) {
            CHECK_MSG(m.present[want_dir[d]] == (d < 6),
                      "rot_y bucket '%s'", k_bmesh_dirs[d]);
            if (d < 6) {
                const McmeMeshBucket *b = &m.buckets[want_dir[d]];
                CHECK(b->face_count == 1 && b->vert_count == 4);
            }
        }
        /* The face now in east came from north: the Y pass maps
           (x, y, z) to (1 - z, y, x). */
        static const double want_east[12] = {
            1, 0, 1,  1, 0, 0,  1, 1, 0,  1, 1, 1 };
        check_floats("rot_y east verts",
                     m.buckets[MCME_CULL_EAST].verts, 12, want_east, 12);
        mcme_mesh_free(&m);
    }

    /* --- x 90 and y 90 together: the vertical remap reads the
       turned ring --- */
    {
        McmeMesh m;
        BLOCKMESH("rot_xy", NULL, &m);
        /* north->down, up->east, south->up, west->north, down->west,
           east->south — both passes, X first. */
        static const int want_dir[6] = {
            MCME_CULL_WEST, MCME_CULL_EAST, MCME_CULL_DOWN,
            MCME_CULL_SOUTH, MCME_CULL_UP, MCME_CULL_NORTH };
        for (int d = 0; d < 6; ++d)
            CHECK_MSG(m.present[want_dir[d]], "rot_xy bucket '%s'",
                      k_bmesh_dirs[want_dir[d]]);
        /* Both passes map (x, y, z) to (y, z, x); the face now in
           west came from down. */
        static const double want_west[12] = {
            0, 0, 0,  0, 0, 1,  0, 1, 1,  0, 1, 0 };
        check_floats("rot_xy west verts", m.buckets[MCME_CULL_WEST].verts,
                     12, want_west, 12);
        mcme_mesh_free(&m);
    }

    /* --- one bucket, six faces: every face turns, each in its own
       slot (a whole-bucket walk, not just the first face) --- */
    {
        McmeMesh m;
        BLOCKMESH("rot_multi", NULL, &m);
        const McmeMeshBucket *b = &m.buckets[MCME_CULL_NONE];
        CHECK_MSG(b->face_count == 6 && b->vert_count == 24,
                  "rot_multi has %zu faces, want 6", b->face_count);
        for (int d = 0; d < 7; ++d)
            CHECK_MSG(m.present[d] == (d == MCME_CULL_NONE),
                      "rot_multi bucket '%s'", k_bmesh_dirs[d]);
        /* Faces 0 and 1 (down and up) under (1 - z, y, x). */
        static const double want_first8[24] = {
            1, 0, 0,  1, 0, 1,  0, 0, 1,  0, 0, 0,   /* down turned */
            0, 1, 0,  0, 1, 1,  1, 1, 1,  1, 1, 0 }; /* up turned */
        check_floats("rot_multi faces 0-1", b->verts, 24, want_first8, 24);
        mcme_mesh_free(&m);
    }

    /* --- the guard's quirks: roty out of range with rotx zero is
       untouched; rotx out of range raises and the apply is
       skipped --- */
    {
        McmeMesh plain, wide, refused, nomodel;
        CHECK(build_mesh(models, tex, "mc", "block/tex", &plain));
        BLOCKMESH("rot_ywide", NULL, &wide);
        check_floats("rot_ywide verts", wide.buckets[MCME_CULL_NONE].verts,
                     plain.buckets[MCME_CULL_NONE].vert_count * 3,
                     plain.buckets[MCME_CULL_NONE].verts,
                     plain.buckets[MCME_CULL_NONE].vert_count * 3);
        check_floats("rot_ywide tverts", wide.buckets[MCME_CULL_NONE].tverts,
                     plain.buckets[MCME_CULL_NONE].vert_count * 2,
                     plain.buckets[MCME_CULL_NONE].tverts,
                     plain.buckets[MCME_CULL_NONE].vert_count * 2);
        mcme_mesh_free(&wide);
        mcme_mesh_free(&plain);

        BLOCKMESH("rot_xbad", NULL, &refused);
        check_missing_block(&refused, "rot_xbad");
        mcme_mesh_free(&refused);

        /* No model: the missing block stands in, and its x 90 is
           never applied — the rotate call sits under the load. */
        BLOCKMESH("rot_nomodel", NULL, &nomodel);
        check_missing_block(&nomodel, "rot_nomodel");
        static const double want_down[12] = {
            0, 0, 0,  1, 0, 0,  1, 0, 1,  0, 0, 1 };
        check_floats("rot_nomodel down (unturned)",
                     nomodel.buckets[MCME_CULL_DOWN].verts, 12,
                     want_down, 12);
        mcme_mesh_free(&nomodel);
    }

    /* --- the refusal chain: the first matching key skips, the ""
       key answers; an all-refusing selection is the missing block;
       the block's own states change nothing (a property the block
       lacks matches) --- */
    {
        McmeMesh skipped, skipped_no_props, rotated, allrefused;
        static const char *const a1[] = { "a", "1", NULL };
        BLOCKMESH("rot_skip", a1, &skipped);
        BLOCKMESH("rot_skip", NULL, &skipped_no_props);
        BLOCKMESH("rot_x", NULL, &rotated);
        check_floats("rot_skip verts",
                     skipped.buckets[MCME_CULL_NONE].verts, 12,
                     rotated.buckets[MCME_CULL_NONE].verts, 12);
        check_floats("rot_skip (no props) verts",
                     skipped_no_props.buckets[MCME_CULL_NONE].verts, 12,
                     rotated.buckets[MCME_CULL_NONE].verts, 12);
        mcme_mesh_free(&skipped);
        mcme_mesh_free(&skipped_no_props);
        mcme_mesh_free(&rotated);

        BLOCKMESH("rot_allrefuse", NULL, &allrefused);
        check_missing_block(&allrefused, "rot_allrefuse");
        mcme_mesh_free(&allrefused);
    }

    /* --- list apply, uvlock, and both routes to the missing block
       --- */
    {
        McmeMesh listed, uvlocked, plain, neither, absent;
        CHECK(build_mesh(models, tex, "mc", "block/tex", &plain));
        BLOCKMESH("rot_list", NULL, &listed);
        BLOCKMESH("rot_uv", NULL, &uvlocked);
        check_floats("rot_list verts", listed.buckets[MCME_CULL_NONE].verts,
                     plain.buckets[MCME_CULL_NONE].vert_count * 3,
                     plain.buckets[MCME_CULL_NONE].verts,
                     plain.buckets[MCME_CULL_NONE].vert_count * 3);
        check_floats("rot_uv verts", uvlocked.buckets[MCME_CULL_NONE].verts,
                     plain.buckets[MCME_CULL_NONE].vert_count * 3,
                     plain.buckets[MCME_CULL_NONE].verts,
                     plain.buckets[MCME_CULL_NONE].vert_count * 3);
        check_floats("rot_uv tverts", uvlocked.buckets[MCME_CULL_NONE].tverts,
                     plain.buckets[MCME_CULL_NONE].vert_count * 2,
                     plain.buckets[MCME_CULL_NONE].tverts,
                     plain.buckets[MCME_CULL_NONE].vert_count * 2);
        mcme_mesh_free(&listed);
        mcme_mesh_free(&uvlocked);
        mcme_mesh_free(&plain);

        BLOCKMESH("neither", NULL, &neither);
        check_missing_block(&neither, "neither");
        mcme_mesh_free(&neither);
        BLOCKMESH("absent", NULL, &absent);
        check_missing_block(&absent, "absent");
        mcme_mesh_free(&absent);
    }

    /* --- multipart merge: buckets concatenate, faces offset, the
       texture list sorts and remaps, the turned case turns --- */
    {
        McmeMesh m;
        BLOCKMESH("merge", NULL, &m); /* no a=1: the middle case is out */
        CHECK(m.transparency == MCME_TRANSPARENCY_PARTIAL);
        /* The sorted set-union of the parts' lists: only opaque
           survives here. */
        check_textures(&m, (const char *const[]){ opaque_path }, 1);
        for (int d = 0; d < 6; ++d) CHECK(!m.present[d]);
        const McmeMeshBucket *b = &m.buckets[MCME_CULL_NONE];
        CHECK_MSG(b->face_count == 2 && b->vert_count == 8,
                  "merge has %zu faces, want 2", b->face_count);
        static const double turn[12] = {
            0.125, 0.875, 0.125,  0.125, 0.875, 0.875,
            0.875, 0.875, 0.875,  0.875, 0.875, 0.125 };
        check_floats("merge second face (y 90)", b->verts + 12, 12,
                     turn, 12);
        check_u32s("merge faces", b->faces, 12,
                   (const uint32_t[]){ 0,1,2,0,2,3, 4,5,6,4,6,7 }, 12);
        check_u32s("merge texture indexes", b->texture_indexes, 4,
                   (const uint32_t[]){ 0,0, 0,0 }, 4);
        mcme_mesh_free(&m);

        /* With the state matching, the middle case joins: alpha
           sorts before opaque and every index remaps through the
           two-entry list. */
        McmeMesh full;
        static const char *const a1[] = { "a", "1", NULL };
        BLOCKMESH("merge", a1, &full);
        check_textures(&full, (const char *const[]){ alpha_path,
                                                     opaque_path }, 2);
        const McmeMeshBucket *fb = &full.buckets[MCME_CULL_NONE];
        CHECK_MSG(fb->face_count == 3 && fb->vert_count == 12,
                  "merge[a=1] has %zu faces, want 3", fb->face_count);
        check_u32s("merge[a=1] texture indexes", fb->texture_indexes, 6,
                   (const uint32_t[]){ 1,1, 0,0, 1,1 }, 6);
        mcme_mesh_free(&full);

        /* A multipart whose cases all fall away is the EMPTY mesh —
           not the missing block. */
        McmeMesh empty;
        BLOCKMESH("merge_empty", NULL, &empty);
        CHECK_MSG(empty.transparency == MCME_TRANSPARENCY_PARTIAL,
                  "merge_empty transparency %d", empty.transparency);
        CHECK_MSG(empty.texture_count == 0,
                  "merge_empty has %zu textures", empty.texture_count);
        for (int d = 0; d < 7; ++d) CHECK(!empty.present[d]);
        mcme_mesh_free(&empty);
    }

    /* --- null arguments --- */
    {
        McmeMesh m;
        CHECK(!mcme_mesh_for_block(NULL, models, tex, "mc", "rot_x",
                                   NULL, 0, &m));
        CHECK(!mcme_mesh_for_block(states, NULL, tex, "mc", "rot_x",
                                   NULL, 0, &m));
        CHECK(!mcme_mesh_for_block(states, models, NULL, "mc", "rot_x",
                                   NULL, 0, &m));
        CHECK(!mcme_mesh_for_block(states, models, tex, NULL, "rot_x",
                                   NULL, 0, &m));
        CHECK(!mcme_mesh_for_block(states, models, tex, "mc", NULL,
                                   NULL, 0, &m));
        CHECK(!mcme_mesh_for_block(states, models, tex, "mc", "rot_x",
                                   NULL, 0, NULL));
        CHECK(!mcme_mesh_rotate(NULL, 1, 0));
        CHECK(!mcme_mesh_merge(NULL, 1, &m));
        CHECK(!mcme_mesh_merge((const McmeMesh *const[]){ &m }, 1, NULL));
    }

    mcme_blockstate_index_close(states);
    mcme_texture_index_close(tex);
    mcme_model_index_close(models);
    mcme_clear_error();
}

#define AFIXDIR "build/atlas_fixtures"

/*
 * The atlas — the editor's own packer, and every answer pinned
 * against what its create_atlas gave (the probe inputs were
 * re-run through the reference's module verbatim; decisions.md
 * #D34). The input order below is the verb's sorted order, the
 * order ties in the perimeter sort keep.
 */
static void test_atlases(void)
{
    printf("  resource (atlases)\n");

    /* --- fixtures: solid images whose sizes decide the layout --- */
    put(AFIXDIR "/pack.mcmeta",
        "{\"pack\":{\"pack_format\":4,\"description\":\"atlas\"}}");
    const char *pa = AFIXDIR "/assets/minecraft/textures/block/a.png";
    const char *pb = AFIXDIR "/assets/minecraft/textures/block/b.png";
    const char *pc = AFIXDIR "/assets/minecraft/textures/block/c.png";
    const char *pw = AFIXDIR "/assets/minecraft/textures/block/wide.png";
    const char *pt = AFIXDIR "/assets/minecraft/textures/block/tall.png";
    const char *ps = AFIXDIR "/assets/minecraft/textures/block/s3.png";
    const char *pw20 = AFIXDIR "/assets/minecraft/textures/block/wide20.png";
    put_solid_png(pa, 4, 2, 2, 255, 0, 0, 255);    /* red   */
    put_solid_png(pb, 4, 2, 2, 0, 255, 0, 255);    /* green */
    put_solid_png(pc, 3, 2, 2, 0, 0, 255, 0);      /* blue, RGB — expands
                                                      to opaque RGBA */
    put_solid_png(pw, 4, 4, 2, 255, 255, 0, 255);  /* yellow, perimeter 12 */
    put_solid_png(pt, 4, 2, 4, 255, 0, 255, 255);  /* magenta, perimeter 12 */
    put_solid_png(ps, 4, 3, 2, 0, 255, 255, 255);  /* cyan, perimeter 10 */
    put_solid_png(pw20, 4, 20, 2, 255, 128, 0, 255); /* orange — wider
                                                      than any page
                                                      half of 16 */
    /* The growth loop's own frames (D45): one set the maxdim
       floor strands once, one the area term strands twice. At the
       pack root where the index does not scan them — only
       mcme_atlas_build is asked about them, by explicit path. */
    const char *p48x2 = AFIXDIR "/g_48x2.png";
    const char *p2x48 = AFIXDIR "/g_2x48.png";
    const char *p32x16 = AFIXDIR "/g_32x16.png";
    const char *p31x5 = AFIXDIR "/g_31x5.png";
    const char *p28x6 = AFIXDIR "/g_28x6.png";
    const char *p7x29 = AFIXDIR "/g_7x29.png";
    const char *p27x8 = AFIXDIR "/g_27x8.png";
    put_solid_png(p48x2, 4, 48, 2, 255, 0, 0, 255);
    put_solid_png(p2x48, 4, 2, 48, 0, 255, 0, 255);
    put_solid_png(p32x16, 4, 32, 16, 255, 255, 0, 255);
    put_solid_png(p31x5, 4, 31, 5, 255, 0, 0, 255);
    put_solid_png(p28x6, 4, 28, 6, 0, 255, 0, 255);
    put_solid_png(p7x29, 4, 7, 29, 255, 0, 255, 255);
    put_solid_png(p27x8, 4, 27, 8, 0, 255, 255, 255);
    /* A file the loader cannot read, at the pack root where the
       index does not scan it — only the atlas is asked about it. */
    put(AFIXDIR "/broken.png", "this is not a png");

    /* --- the whole pack behind the missing image, in the verb's
           sorted input order (D35: the manager seeds its table with
           the missing image first, and mcme's page carries it too) --- */
    {
        const char *paths[] = { pa, pb, pc, ps, pt, pw };
        McmeAtlas a;
        CHECK_MSG(mcme_atlas_build(paths, 6, &a),
                  "atlas build: %s", mcme_last_error());
        /* The missing image's 256 pixels lead: 256 + 34 -> ceil
           sqrt 18 -> the 32 page. */
        CHECK_MSG(a.size == 32, "pack size %d, want 32", a.size);
        CHECK_MSG(a.count == 7, "pack count %zu, want 7", a.count);
        /* The sentinel takes the top-left quadrant. Under it the
           perimeter order: tall and wide tie at 12 and the tie
           keeps the input order — tall packs first — then s3,
           then a, b, c. Placements, all of them the reference's
           own answers for this input: */
        check_px(&a, 1, 1, 0xf8, 0, 0xf8, 255, "missing, top-left");
        check_px(&a, 8, 0, 0, 0, 0, 255, "missing, black quadrant");
        check_px(&a, 0, 8, 0, 0, 0, 255, "missing, other quadrant");
        check_px(&a, 1, 17, 255, 0, 255, 255, "tall at 0,16");
        check_px(&a, 3, 17, 255, 255, 0, 255, "wide at 2,16");
        check_px(&a, 3, 19, 0, 255, 255, 255, "s3 at 2,18");
        check_px(&a, 1, 21, 255, 0, 0, 255, "a at 0,20");
        check_px(&a, 1, 25, 0, 0, 255, 255, "c at 0,24 (RGB expanded)");
        check_px(&a, 1, 23, 0, 255, 0, 255, "b at 0,22");
        check_px(&a, 31, 31, 0, 0, 0, 0, "the page's empty corner");
        check_px(&a, 6, 16, 0, 0, 0, 0, "the empty strip right of wide");
        /* Bounds, the min(h, w) top edge included. */
        check_bounds(&a, 0, 0.0, 0.0, 0.5, 0.5, "missing_no");
        check_bounds(&a, 1, 0.0, 0.625, 0.0625, 0.6875, "a");
        check_bounds(&a, 2, 0.0, 0.6875, 0.0625, 0.75, "b");
        check_bounds(&a, 3, 0.0, 0.75, 0.0625, 0.8125, "c");
        check_bounds(&a, 4, 0.0625, 0.5625, 0.15625, 0.625, "s3");
        check_bounds(&a, 5, 0.0, 0.5, 0.0625, 0.5625, "tall");
        check_bounds(&a, 6, 0.0625, 0.5, 0.1875, 0.5625, "wide");
        CHECK_MSG(strcmp(a.names[0], "missing_no") == 0,
                  "name 0 is '%s', want the missing image", a.names[0]);
        for (size_t i = 0; i < 6; ++i)
            CHECK_MSG(strcmp(a.names[i + 1], paths[i]) == 0,
                      "name %zu is '%s', want '%s'", i + 1,
                      a.names[i + 1], paths[i]);
        mcme_atlas_free(&a);
    }

    /* --- the bounds rule: an exact name answers its own
           rectangle, anything else the missing image's own — the
           reference's texture_bounds, rule for rule (D35) --- */
    {
        const char *paths[] = { pa, pb, pc, ps, pt, pw };
        McmeAtlas a;
        CHECK_MSG(mcme_atlas_build(paths, 6, &a),
                  "rule atlas: %s", mcme_last_error());
        double r[4];
        CHECK(mcme_atlas_bounds(&a, pa, r));
        CHECK(fabs(r[0] - 0.0) < 1e-9 && fabs(r[1] - 0.625) < 1e-9 &&
              fabs(r[2] - 0.0625) < 1e-9 && fabs(r[3] - 0.6875) < 1e-9);
        CHECK(mcme_atlas_bounds(&a, "missing_no", r));
        CHECK(fabs(r[0] - 0.0) < 1e-9 && fabs(r[1] - 0.0) < 1e-9 &&
              fabs(r[2] - 0.5) < 1e-9 && fabs(r[3] - 0.5) < 1e-9);
        /* A texture the page does not hold — including one a mesh
           names after a texture the pack lacks. */
        CHECK(mcme_atlas_bounds(&a, "block/nonexistent", r));
        CHECK(fabs(r[0] - 0.0) < 1e-9 && fabs(r[1] - 0.0) < 1e-9 &&
              fabs(r[2] - 0.5) < 1e-9 && fabs(r[3] - 0.5) < 1e-9);
        CHECK(!mcme_atlas_bounds(NULL, pa, r));
        CHECK(!mcme_atlas_bounds(&a, NULL, r));
        CHECK(!mcme_atlas_bounds(&a, pa, NULL));
        mcme_clear_error();
        mcme_atlas_free(&a);
    }

    /* --- the min(h, w) quirk alone: a 2x4's top edge is 2/4, not
           4/4; behind the missing image the page is 32 --- */
    {
        McmeAtlas a;
        CHECK(mcme_atlas_build(&pt, 1, &a));
        CHECK_MSG(a.size == 32, "tall-only size %d, want 32", a.size);
        check_bounds(&a, 1, 0.0, 0.5, 0.0625, 0.5625, "tall-only");
        check_px(&a, 1, 17, 255, 0, 255, 255, "tall-only top");
        check_px(&a, 1, 19, 255, 0, 255, 255, "tall-only bottom");
        mcme_atlas_free(&a);
    }

    /* --- the estimate too small doubles: a 20-wide frame will not
           sit beside the missing image on a 32 page (296 pixels
           estimate 32, the packer refuses, the page doubles to64;
           the reference's own answers for these) --- */
    {
        const char *paths[] = { pw20, pa };
        McmeAtlas a;
        CHECK_MSG(mcme_atlas_build(paths, 2, &a),
                  "doubling atlas: %s", mcme_last_error());
        CHECK_MSG(a.size == 64, "doubling size %d, want 64", a.size);
        check_bounds(&a, 0, 0.0, 0.0, 0.25, 0.25, "doubling missing");
        check_bounds(&a, 1, 0.25, 0.0, 0.5625, 0.03125, "doubling wide20");
        check_bounds(&a, 2, 0.0, 0.25, 0.03125, 0.28125, "doubling a");
        check_px(&a, 8, 8, 0xf8, 0, 0xf8, 255, "doubling: the missing");
        check_px(&a, 26, 1, 255, 128, 0, 255, "doubling: wide20 at 16,0");
        check_px(&a, 1, 17, 255, 0, 0, 255, "doubling: a at 0,16");
        mcme_atlas_free(&a);
    }

    /* --- the growth loop's own steps (D45): ONCE at the
           estimate's maxdim floor — 48 doubles to 96 — and TWICE
           at its area term — 32 through 64 to 128, frames
           re-placed from a fresh root both times; every
           rectangle below is the reference's own answer --- */
    {
        McmeAtlas a;
        const char *g1[] = { p48x2, p2x48, p32x16 };
        CHECK_MSG(mcme_atlas_build(g1, 3, &a),
                  "grow atlas: %s", mcme_last_error());
        CHECK_MSG(a.size == 96, "grow size %d, want 96", a.size);
        CHECK_MSG(a.count == 4, "grow count %zu, want 4", a.count);
        /* The sentinel, perimeter-last, lands in the strip the
           three leave behind. */
        check_bounds(&a, 0, 2.0 / 96, 18.0 / 96, 18.0 / 96, 34.0 / 96,
                     "grow sentinel");
        check_bounds(&a, 1, 0.0, 0.0, 48.0 / 96, 2.0 / 96, "grow 48x2");
        check_bounds(&a, 2, 0.0, 2.0 / 96, 2.0 / 96, 4.0 / 96,
                     "grow 2x48");
        check_bounds(&a, 3, 2.0 / 96, 2.0 / 96, 34.0 / 96, 18.0 / 96,
                     "grow 32x16");
        mcme_atlas_free(&a);

        const char *g2[] = { p31x5, p28x6, p7x29, p27x8 };
        CHECK_MSG(mcme_atlas_build(g2, 4, &a),
                  "grow2 atlas: %s", mcme_last_error());
        CHECK_MSG(a.size == 128, "grow2 size %d, want 128", a.size);
        CHECK_MSG(a.count == 5, "grow2 count %zu, want 5", a.count);
        check_bounds(&a, 0, 7.0 / 128, 5.0 / 128, 23.0 / 128,
                     21.0 / 128, "grow2 sentinel");
        check_bounds(&a, 1, 0.0, 0.0, 31.0 / 128, 5.0 / 128,
                     "grow2 31x5");
        check_bounds(&a, 2, 58.0 / 128, 0.0, 86.0 / 128, 6.0 / 128,
                     "grow2 28x6");
        check_bounds(&a, 3, 0.0, 5.0 / 128, 7.0 / 128, 12.0 / 128,
                     "grow2 7x29");
        check_bounds(&a, 4, 31.0 / 128, 0.0, 58.0 / 128, 8.0 / 128,
                     "grow2 27x8");
        mcme_atlas_free(&a);
    }

    /* --- perimeter order beats input order; equal perimeters keep
           it (a and b both 8; wide 12 goes first) --- */
    {
        const char *paths[] = { pa, pw, pb };
        McmeAtlas a;
        CHECK(mcme_atlas_build(paths, 3, &a));
        CHECK(a.size == 32);
        check_bounds(&a, 0, 0.0, 0.0, 0.5, 0.5, "reordered missing");
        check_bounds(&a, 1, 0.0, 0.5625, 0.0625, 0.625, "reordered a");
        check_bounds(&a, 2, 0.0, 0.5, 0.125, 0.5625, "reordered wide");
        check_bounds(&a, 3, 0.0, 0.625, 0.0625, 0.6875, "reordered b");
        mcme_atlas_free(&a);
    }

    /* --- the empty pack: the app's page is never empty — the
           manager seeds its table, so the missing image alone owns
           a 16x16 page and its bounds are the whole of it --- */
    {
        McmeAtlas a;
        CHECK(mcme_atlas_build(NULL, 0, &a));
        CHECK_MSG(a.size == 16, "empty size %d, want 16", a.size);
        CHECK(a.count == 1);
        CHECK(strcmp(a.names[0], "missing_no") == 0);
        check_bounds(&a, 0, 0.0, 0.0, 1.0, 1.0, "empty page");
        /* The game's own missing texture, spelled out: the 8x8
           quadrants, magenta where (x < 8) == (y < 8). */
        check_px(&a, 0, 0, 0xf8, 0, 0xf8, 255, "missing top-left");
        check_px(&a, 8, 0, 0, 0, 0, 255, "missing top-right");
        check_px(&a, 0, 8, 0, 0, 0, 255, "missing bottom-left");
        check_px(&a, 8, 8, 0xf8, 0, 0xf8, 255, "missing bottom-right");
        check_px(&a, 15, 15, 0xf8, 0, 0xf8, 255, "missing corner");
        mcme_atlas_free(&a);
    }

    /* --- image load / write round trip --- */
    {
        McmeImage img;
        CHECK_MSG(mcme_image_load(pa, &img), "load a.png: %s",
                  mcme_last_error());
        CHECK(img.width == 2 && img.height == 2);
        CHECK(img.rgba[0] == 255 && img.rgba[1] == 0 &&
              img.rgba[3] == 255);
        const char *copy = AFIXDIR "/copy.png";
        CHECK(mcme_image_write_png(copy, &img));
        mcme_image_free(&img);
        CHECK(mcme_image_load(copy, &img));
        CHECK(img.width == 2 && img.height == 2);
        CHECK(img.rgba[4] == 255 && img.rgba[5] == 0);
        mcme_image_free(&img);
    }

    /* --- the texture index's enumeration (the verb's input) --- */
    {
        McmeTextureIndex *tex = NULL;
        CHECK_MSG(mcme_texture_index_open(AFIXDIR, &tex),
                  "index open: %s", mcme_last_error());
        CHECK_MSG(mcme_texture_count(tex) == 7, "index holds %zu, want 7",
                  mcme_texture_count(tex));
        for (size_t i = 0; i < mcme_texture_count(tex); ++i)
            CHECK(mcme_texture_at(tex, i) != NULL);
        CHECK(mcme_texture_at(tex, 7) == NULL);
        mcme_texture_index_close(tex);
    }

    /* --- refusals --- */
    {
        McmeAtlas a;
        McmeImage img;
        const char *paths[] = { pa };
        CHECK(!mcme_atlas_build(NULL, 1, &a));
        CHECK(!mcme_atlas_build(paths, 1, NULL));
        CHECK(!mcme_image_load(NULL, &img));
        CHECK(!mcme_image_load(AFIXDIR "/no_such.png", &img));
        CHECK(!mcme_image_write_png(NULL, &img));
        /* An image the loader cannot read refuses the whole atlas —
           the reference's Pillow raise kills its call (D34). */
        const char *bad[] = { AFIXDIR "/broken.png" };
        CHECK(!mcme_atlas_build(bad, 1, &a));
        CHECK(mcme_texture_count(NULL) == 0);
        CHECK(mcme_texture_at(NULL, 0) == NULL);
        mcme_image_free(NULL);
        mcme_atlas_free(NULL);
        mcme_clear_error();
    }
}

#define UFIXDIR "build/atlasuv_fixtures/pack"

/*
 * Texture coordinates in atlas space — the editor's chunk builder's
 * table and its fragment stage's mapping (D35). The rectangles come
 * from the reference's own create_atlas over this input; the
 * mapping is the shader's mix(b0, b1, mod(t, 1.0)), which wraps a
 * coordinate AT 1.0 back to the origin edge and a negative one up.
 * Every probe below is chosen so a clamp or a straight lerp would
 * answer differently, and the wrap model's out-of-range UVs are no
 * invention: the reference hands them through unclamped
 * (-0.25 .. 1.25, verified verbatim off its own mesh).
 */
static void test_atlas_uv(void)
{
    printf("  resource (atlas UVs)\n");

    /* --- fixtures: two textures with distinct rectangles, three
           models asking different questions of the mapping --- */
    put(UFIXDIR "/pack.mcmeta",
        "{\"pack\":{\"pack_format\":9,\"description\":\"atlasuv\"}}");
    /* The textures live under minecraft/ because the models'
       colon-less references default to that namespace — the
       reference answers missing_no for a file that exists only
       under the model's own namespace (verified verbatim: same
       model, same file, one location at a time). */
    const char *pop = UFIXDIR "/assets/minecraft/textures/block/opaque.png";
    const char *pta = UFIXDIR "/assets/minecraft/textures/block/tallt.png";
    put_solid_png(pop, 4, 4, 4, 255, 0, 0, 255);    /* red, 4x4   */
    put_solid_png(pta, 4, 2, 4, 255, 0, 255, 255);  /* magenta, 2x4:
                                                       the min(h, w) edge shows
                                                       in a remapped bound */
    /* A full cube culling every direction on one texture: six
       buckets, every face's coordinates at rectangle corners. */
    put(UFIXDIR "/assets/mc/models/block/cube6.json",
        "{\"textures\":{\"a\":\"block/opaque\"},"
        "\"elements\":[{\"from\":[0,0,0],\"to\":[16,16,16],"
        "\"faces\":{"
          "\"down\":{\"texture\":\"#a\",\"cullface\":\"down\"},"
          "\"up\":{\"texture\":\"#a\",\"cullface\":\"up\"},"
          "\"north\":{\"texture\":\"#a\",\"cullface\":\"north\"},"
          "\"east\":{\"texture\":\"#a\",\"cullface\":\"east\"},"
          "\"south\":{\"texture\":\"#a\",\"cullface\":\"south\"},"
          "\"west\":{\"texture\":\"#a\",\"cullface\":\"west\"}}}]}");
    /* A partial cube whose face UV runs past both edges: the
       coordinates wrap, they do not clamp. */
    put(UFIXDIR "/assets/mc/models/block/wrap.json",
        "{\"textures\":{\"a\":\"block/tallt\"},"
        "\"elements\":[{\"from\":[2,2,2],\"to\":[14,14,14],"
        "\"faces\":{\"up\":{\"texture\":\"#a\",\"uv\":[-4,0,20,16]}}}]}");
    /* A partial cube on a texture the pack lacks: the mesh keeps
       the missing word, and the lookup answers the missing image's
       own rectangle. */
    put(UFIXDIR "/assets/mc/models/block/missingpart.json",
        "{\"textures\":{\"a\":\"block/absent\"},"
        "\"elements\":[{\"from\":[2,2,2],\"to\":[14,14,14],"
        "\"faces\":{\"up\":{\"texture\":\"#a\",\"uv\":[0,0,8,8]}}}]}");
    /* A full cube on an alpha texture: the mesh's transparency is
       translucent — what the cull clauses read when something
       stands beside the block (D37). */
    put_solid_png(UFIXDIR "/assets/minecraft/textures/block/alphat.png",
                  4, 4, 4, 0, 255, 255, 128);           /* cyan, alpha 128 */
    put(UFIXDIR "/assets/mc/models/block/cubea.json",
        "{\"textures\":{\"a\":\"block/alphat\"},"
        "\"elements\":[{\"from\":[0,0,0],\"to\":[16,16,16],"
        "\"faces\":{"
          "\"down\":{\"texture\":\"#a\",\"cullface\":\"down\"},"
          "\"up\":{\"texture\":\"#a\",\"cullface\":\"up\"},"
          "\"north\":{\"texture\":\"#a\",\"cullface\":\"north\"},"
          "\"east\":{\"texture\":\"#a\",\"cullface\":\"east\"},"
          "\"south\":{\"texture\":\"#a\",\"cullface\":\"south\"},"
          "\"west\":{\"texture\":\"#a\",\"cullface\":\"west\"}}}]}");

    McmeModelIndex *models = NULL;
    McmeTextureIndex *tex = NULL;
    CHECK_MSG(mcme_model_index_open(UFIXDIR, &models),
              "models open: %s", mcme_last_error());
    CHECK_MSG(mcme_texture_index_open(UFIXDIR, &tex),
              "textures open: %s", mcme_last_error());

    /* The page, in the verb's sorted input order — the reference's
       answers for these two images behind the missing image. */
    const char *paths[2] = { pop, pta };
    McmeAtlas atlas;
    CHECK_MSG(mcme_atlas_build(paths, 2, &atlas),
              "atlasuv atlas: %s", mcme_last_error());
    CHECK(atlas.size == 32);
    check_bounds(&atlas, 0, 0.0, 0.0, 0.5, 0.5, "uv missing");
    check_bounds(&atlas, 1, 0.0, 0.5, 0.125, 0.625, "uv opaque");
    check_bounds(&atlas, 2, 0.0, 0.625, 0.0625, 0.6875, "uv tallt");

    /* --- the full cube: every corner's coordinate is 0 or 1, and
           the wrap puts BOTH at the origin edge — auv is the
           rectangle's origin on every row, where a straight lerp
           would send the 1.0 corners to the far edge --- */
    {
        McmeMesh m;
        CHECK(build_mesh(models, tex, "mc", "block/cube6", &m));
        CHECK_MSG(m.transparency == MCME_TRANSPARENCY_OPAQUE,
                  "cube6 transparency %d, want opaque",
                  (int)m.transparency);
        McmeAtlasMesh t;
        CHECK_MSG(mcme_atlas_mesh(&atlas, &m, 0, MCME_TRANSPARENCY_PARTIAL, &t),
                  "cube6 table: %s", mcme_last_error());
        for (int d = MCME_CULL_DOWN; d <= MCME_CULL_WEST; ++d) {
            CHECK_MSG(t.present[d], "cube6 bucket %d absent", d);
            CHECK_MSG(t.buckets[d].row_count == 6,
                      "cube6 bucket %d rows %zu, want 6", d,
                      t.buckets[d].row_count);
            /* A failure above is counted, not re-raised as a
               crash: nothing to index when the bucket is empty. */
            if (!t.buckets[d].bounds) continue;
            for (size_t r = 0; r < 6; ++r) {
                const double *b = &t.buckets[d].bounds[r * 4];
                const double *uv = &t.buckets[d].auvs[r * 2];
                CHECK(fabs(b[0] - 0.0) < 1e-9 &&
                      fabs(b[1] - 0.5) < 1e-9 &&
                      fabs(b[2] - 0.125) < 1e-9 &&
                      fabs(b[3] - 0.625) < 1e-9);
                CHECK_MSG(fabs(uv[0] - 0.0) < 1e-9 &&
                          fabs(uv[1] - 0.5) < 1e-9,
                          "cube6 bucket %d row %zu auv %g %g, want 0 0.5",
                          d, r, uv[0], uv[1]);
            }
        }
        CHECK(!t.present[MCME_CULL_NONE]);
        mcme_atlas_mesh_free(&t);
        mcme_mesh_free(&m);
    }

    /* --- the wrap: uv [-4,0,20,16] over the 2x4's rectangle. The
           face's corners are u -0.25/1.25 and v 0/1; the mapping
           wraps them to 0.75/0.25 and 0/0. A clamp would answer
           0/1 and 0/1. --- */
    {
        static const double k_auvs[6][2] = {
            {0.046875, 0.625}, {0.015625, 0.625}, {0.015625, 0.625},
            {0.046875, 0.625}, {0.015625, 0.625}, {0.046875, 0.625}
        };
        McmeMesh m;
        CHECK(build_mesh(models, tex, "mc", "block/wrap", &m));
        McmeAtlasMesh t;
        CHECK_MSG(mcme_atlas_mesh(&atlas, &m, 0, MCME_TRANSPARENCY_PARTIAL, &t),
                  "wrap table: %s", mcme_last_error());
        CHECK_MSG(t.present[MCME_CULL_NONE],
                  "wrap: no cullface, so the none bucket");
        const McmeAtlasBucket *b_ = &t.buckets[MCME_CULL_NONE];
        CHECK(b_->row_count == 6);
        for (size_t r = 0; b_->bounds && r < 6; ++r) {
            const double *b = &b_->bounds[r * 4];
            const double *uv = &b_->auvs[r * 2];
            CHECK(fabs(b[0] - 0.0) < 1e-9 &&
                  fabs(b[1] - 0.625) < 1e-9 &&
                  fabs(b[2] - 0.0625) < 1e-9 &&
                  fabs(b[3] - 0.6875) < 1e-9);
            CHECK_MSG(fabs(uv[0] - k_auvs[r][0]) < 1e-9 &&
                      fabs(uv[1] - k_auvs[r][1]) < 1e-9,
                      "wrap row %zu auv %g %g, want %g %g", r, uv[0],
                      uv[1], k_auvs[r][0], k_auvs[r][1]);
        }
        mcme_atlas_mesh_free(&t);
        mcme_mesh_free(&m);
    }

    /* --- the missing texture: the mesh keeps the missing word and
           the lookup answers the missing image's rectangle — the
           face's uv [0,0,8,8] lands halfway across the page's
           top-left quadrant --- */
    {
        static const double k_auvs[6][2] = {
            {0.0, 0.25}, {0.25, 0.25}, {0.25, 0.0},
            {0.0, 0.25}, {0.25, 0.0}, {0.0, 0.0}
        };
        McmeMesh m;
        CHECK(build_mesh(models, tex, "mc", "block/missingpart", &m));
        static const char *const want_tex[] = { "missing_no" };
        check_textures(&m, want_tex, 1);
        McmeAtlasMesh t;
        CHECK_MSG(mcme_atlas_mesh(&atlas, &m, 0, MCME_TRANSPARENCY_PARTIAL, &t),
                  "missingpart table: %s", mcme_last_error());
        CHECK(t.present[MCME_CULL_NONE]);
        const McmeAtlasBucket *b_ = &t.buckets[MCME_CULL_NONE];
        CHECK(b_->row_count == 6);
        for (size_t r = 0; b_->bounds && r < 6; ++r) {
            const double *b = &b_->bounds[r * 4];
            const double *uv = &b_->auvs[r * 2];
            CHECK(fabs(b[0] - 0.0) < 1e-9 && fabs(b[1] - 0.0) < 1e-9 &&
                  fabs(b[2] - 0.5) < 1e-9 && fabs(b[3] - 0.5) < 1e-9);
            CHECK_MSG(fabs(uv[0] - k_auvs[r][0]) < 1e-9 &&
                      fabs(uv[1] - k_auvs[r][1]) < 1e-9,
                      "missingpart row %zu auv %g %g, want %g %g", r,
                      uv[0], uv[1], k_auvs[r][0], k_auvs[r][1]);
        }
        mcme_atlas_mesh_free(&t);
        mcme_mesh_free(&m);
    }

    /* --- the tint fold: the direction's brightness first, then the
           band shade at the vertex's world height — the block's y
           under its own. cube6's faces sit at model heights 0 and 1;
           at the origin the wave is the rising ramp alone (t under 1,
           nothing folded back), so the shade reads 0.9 + 0.2 y/32
           and the fold the direction's step of 0.15 --- */
    {
        /* down three steps, east/west two, north/south one, up and
           the cullless bucket none — the builder's table, D36 */
        static const double k_bright[7] = {
            0.55, 1.0, 0.85, 0.70, 0.85, 0.70, 1.0
        };
        McmeMesh m;
        CHECK(build_mesh(models, tex, "mc", "block/cube6", &m));
        McmeAtlasMesh t;
        CHECK_MSG(mcme_atlas_mesh(&atlas, &m, 0, MCME_TRANSPARENCY_PARTIAL, &t),
                  "cube6 tint table: %s", mcme_last_error());
        for (int d = MCME_CULL_DOWN; d <= MCME_CULL_WEST; ++d) {
            const McmeAtlasBucket *b_ = &t.buckets[d];
            if (!b_->tints) continue;
            for (size_t r = 0; r < b_->row_count; ++r) {
                double y = m.buckets[d].verts[
                    m.buckets[d].faces[r] * 3 + 1];
                double want = k_bright[d] * (0.9 + 0.2 * y / 32.0);
                const double *tint = &b_->tints[r * 3];
                CHECK_MSG(fabs(tint[0] - want) < 1e-9 &&
                          fabs(tint[1] - want) < 1e-9 &&
                          fabs(tint[2] - want) < 1e-9,
                          "cube6 bucket %d row %zu tint %g %g %g, "
                          "want %g", d, r, tint[0], tint[1], tint[2],
                          want);
            }
        }
        /* The flat faces anchor the literals: down at height 0 the
           trough under three steps, up at height 1 the ramp's first
           notch. */
        for (size_t r = 0; r < 6; ++r) {
            const double *tint = &t.buckets[MCME_CULL_DOWN]
                                     .tints[r * 3];
            CHECK(fabs(tint[0] - 0.495) < 1e-9 &&
                  fabs(tint[1] - 0.495) < 1e-9 &&
                  fabs(tint[2] - 0.495) < 1e-9);
            tint = &t.buckets[MCME_CULL_UP].tints[r * 3];
            CHECK(fabs(tint[0] - 0.90625) < 1e-9 &&
                  fabs(tint[1] - 0.90625) < 1e-9 &&
                  fabs(tint[2] - 0.90625) < 1e-9);
        }
        mcme_atlas_mesh_free(&t);
        mcme_mesh_free(&m);
    }

    /* --- the wave, over the block's world height: at 32 the peak
           (t == 1 — the reference folds `shade > 1`, so the peak
           belongs to the top, not past it), at 64 the trough again,
           and below zero numpy's remainder walks the wave where C's
           fmod would answer negative and slide under the trough --- */
    {
        static const struct { long by; int d; double want; } k_wave[] = {
            {   0, MCME_CULL_DOWN, 0.495 },
            {   0, MCME_CULL_UP,   0.90625 },
            {  32, MCME_CULL_DOWN, 0.605 },
            {  32, MCME_CULL_UP,   1.09375 },
            {  64, MCME_CULL_DOWN, 0.495 },
            { -32, MCME_CULL_DOWN, 0.605 },
            {  -1, MCME_CULL_DOWN, 0.4984375 }
        };
        McmeMesh m;
        CHECK(build_mesh(models, tex, "mc", "block/cube6", &m));
        for (size_t i = 0; i < sizeof k_wave / sizeof *k_wave; ++i) {
            McmeAtlasMesh t;
            CHECK_MSG(mcme_atlas_mesh(&atlas, &m, k_wave[i].by,
                                       MCME_TRANSPARENCY_PARTIAL, &t),
                      "wave table: %s", mcme_last_error());
            const double *tint = t.buckets[k_wave[i].d].tints;
            CHECK_MSG(tint && fabs(tint[0] - k_wave[i].want) < 1e-9 &&
                      fabs(tint[1] - k_wave[i].want) < 1e-9 &&
                      fabs(tint[2] - k_wave[i].want) < 1e-9,
                      "wave by %ld bucket %d tint %g, want %g",
                      k_wave[i].by, k_wave[i].d, tint ? tint[0] : -1.0,
                      k_wave[i].want);
            mcme_atlas_mesh_free(&t);
        }
        mcme_mesh_free(&m);
    }

    /* --- the cull clauses: the rows are the SURVIVORS. cube6's
           faces carry cullfaces, so its buckets drop whole beside
           an opaque neighbour; beside a translucent one they drop
           only when the mesh itself is translucent — cubea is
           that translucent cube, and it drops under both clauses.
           Wrap's single up face carries no cullface: the none
           bucket never culls, under any neighbour — the test's own
           `if cull_id:` exemption (D37). --- */
    {
        static const McmeTransparency k_beside[3] = {
            MCME_TRANSPARENCY_OPAQUE, MCME_TRANSPARENCY_TRANSLUCENT,
            MCME_TRANSPARENCY_PARTIAL
        };
        McmeMesh m6, ma, mw;
        CHECK(build_mesh(models, tex, "mc", "block/cube6", &m6));
        CHECK(build_mesh(models, tex, "mc", "block/cubea", &ma));
        CHECK(build_mesh(models, tex, "mc", "block/wrap", &mw));
        CHECK_MSG(ma.transparency == MCME_TRANSPARENCY_TRANSLUCENT,
                  "cubea transparency %d, want translucent",
                  (int)ma.transparency);
        for (int d = MCME_CULL_DOWN; d <= MCME_CULL_WEST; ++d) {
            McmeAtlasMesh t;
            /* cube6 (opaque) beside opaque: every bucket drops.
               Beside translucent: neither clause fires — the rows
               stand, six per face. */
            CHECK_MSG(mcme_atlas_mesh(&atlas, &m6, 0,
                                       MCME_TRANSPARENCY_OPAQUE, &t),
                      "cube6 beside opaque: %s", mcme_last_error());
            CHECK_MSG(t.buckets[d].row_count == 0 &&
                      !t.buckets[d].tints,
                      "cube6 beside opaque bucket %d answered rows",
                      d);
            mcme_atlas_mesh_free(&t);
            CHECK_MSG(mcme_atlas_mesh(&atlas, &m6, 0,
                                       MCME_TRANSPARENCY_TRANSLUCENT,
                                       &t),
                      "cube6 beside translucent: %s",
                      mcme_last_error());
            CHECK_MSG(t.buckets[d].row_count == 6,
                      "cube6 beside translucent bucket %d rows %zu",
                      d, t.buckets[d].row_count);
            mcme_atlas_mesh_free(&t);
            /* cubea (translucent) beside opaque: the first clause.
               Beside translucent: the chained pair. Beside partial:
               neither — the rows stand. */
            CHECK_MSG(mcme_atlas_mesh(&atlas, &ma, 0,
                                       MCME_TRANSPARENCY_OPAQUE, &t),
                      "cubea beside opaque: %s", mcme_last_error());
            CHECK_MSG(t.buckets[d].row_count == 0,
                      "cubea beside opaque bucket %d answered rows",
                      d);
            mcme_atlas_mesh_free(&t);
            CHECK_MSG(mcme_atlas_mesh(&atlas, &ma, 0,
                                       MCME_TRANSPARENCY_TRANSLUCENT,
                                       &t),
                      "cubea beside translucent: %s",
                      mcme_last_error());
            CHECK_MSG(t.buckets[d].row_count == 0,
                      "cubea beside translucent bucket %d answered "
                      "rows", d);
            mcme_atlas_mesh_free(&t);
            CHECK_MSG(mcme_atlas_mesh(&atlas, &ma, 0,
                                       MCME_TRANSPARENCY_PARTIAL, &t),
                      "cubea beside partial: %s", mcme_last_error());
            CHECK_MSG(t.buckets[d].row_count == 6,
                      "cubea beside partial bucket %d rows %zu", d,
                      t.buckets[d].row_count);
            mcme_atlas_mesh_free(&t);
        }
        for (int n = 0; n < 3; ++n) {
            McmeAtlasMesh t;
            CHECK_MSG(mcme_atlas_mesh(&atlas, &mw, 0, k_beside[n], &t),
                      "wrap beside %d: %s", (int)k_beside[n],
                      mcme_last_error());
            CHECK_MSG(t.buckets[MCME_CULL_NONE].row_count == 6,
                      "wrap beside %d rows %zu, the none bucket "
                      "never culls", (int)k_beside[n],
                      t.buckets[MCME_CULL_NONE].row_count);
            mcme_atlas_mesh_free(&t);
        }
        mcme_mesh_free(&m6);
        mcme_mesh_free(&ma);
        mcme_mesh_free(&mw);
    }

    /* --- the six neighbours (D38): each bucket reads the side's
           own neighbour, so the clauses can answer differently on
           the six sides at once. cube6's down face drops beside
           the opaque standing down there while its other five
           stand beside partial; cubea's up face alone drops
           beside the translucent above it — the chained pair,
           one side. The none bucket still answers under any
           neighbour on any side. --- */
    {
        McmeTransparency nd[MCME_CULL_NONE];
        for (int i = 0; i < MCME_CULL_NONE; ++i)
            nd[i] = MCME_TRANSPARENCY_PARTIAL;
        McmeMesh m6, ma, mw;
        CHECK(build_mesh(models, tex, "mc", "block/cube6", &m6));
        CHECK(build_mesh(models, tex, "mc", "block/cubea", &ma));
        CHECK(build_mesh(models, tex, "mc", "block/wrap", &mw));

        nd[MCME_CULL_DOWN] = MCME_TRANSPARENCY_OPAQUE;
        McmeAtlasMesh t;
        CHECK_MSG(mcme_atlas_mesh_dirs(&atlas, &m6, 0, nd, &t),
                  "cube6 with opaque below: %s", mcme_last_error());
        for (int i = 0; i < 7; ++i) {
            size_t want = (i == MCME_CULL_DOWN ||
                           !m6.present[i]) ? 0 : 6;
            CHECK_MSG(t.buckets[i].row_count == want,
                      "cube6 with opaque below: bucket %d rows %zu, "
                      "want %zu", i, t.buckets[i].row_count, want);
        }
        mcme_atlas_mesh_free(&t);

        nd[MCME_CULL_UP] = MCME_TRANSPARENCY_TRANSLUCENT;
        CHECK_MSG(mcme_atlas_mesh_dirs(&atlas, &m6, 0, nd, &t),
                  "cube6 with translucent above: %s",
                  mcme_last_error());
        CHECK_MSG(t.buckets[MCME_CULL_UP].row_count == 6,
                  "cube6 opaque is no partner for the chained pair: "
                  "up rows %zu", t.buckets[MCME_CULL_UP].row_count);
        mcme_atlas_mesh_free(&t);

        /* cubea (translucent): the translucent neighbour above is
           the chained pair on that one side; the opaque below is
           clause one there; partial elsewhere stands. */
        CHECK_MSG(mcme_atlas_mesh_dirs(&atlas, &ma, 0, nd, &t),
                  "cubea, opaque below and translucent above: %s",
                  mcme_last_error());
        for (int i = 0; i < 7; ++i) {
            size_t want = (i == MCME_CULL_DOWN ||
                           i == MCME_CULL_UP || !ma.present[i])
                ? 0 : 6;
            CHECK_MSG(t.buckets[i].row_count == want,
                      "cubea mixed sides: bucket %d rows %zu, want %zu",
                      i, t.buckets[i].row_count, want);
        }
        mcme_atlas_mesh_free(&t);

        /* The none bucket under a hostile ring on every side. */
        for (int i = 0; i < MCME_CULL_NONE; ++i)
            nd[i] = MCME_TRANSPARENCY_OPAQUE;
        CHECK_MSG(mcme_atlas_mesh_dirs(&atlas, &mw, 0, nd, &t),
                  "wrap ringed by opaque: %s", mcme_last_error());
        CHECK_MSG(t.buckets[MCME_CULL_NONE].row_count == 6,
                  "wrap ringed by opaque: none rows %zu",
                  t.buckets[MCME_CULL_NONE].row_count);
        mcme_atlas_mesh_free(&t);

        /* The uniform answer is the per-direction one with every
           side alike — same survivors on the same sides. */
        McmeAtlasMesh u;
        CHECK_MSG(mcme_atlas_mesh(&atlas, &ma, 0,
                                  MCME_TRANSPARENCY_TRANSLUCENT, &u),
                  "cubea uniform: %s", mcme_last_error());
        for (int i = 0; i < MCME_CULL_NONE; ++i)
            nd[i] = MCME_TRANSPARENCY_TRANSLUCENT;
        CHECK_MSG(mcme_atlas_mesh_dirs(&atlas, &ma, 0, nd, &t),
                  "cubea per-side: %s", mcme_last_error());
        for (int i = 0; i < 7; ++i) {
            CHECK_MSG(u.present[i] == t.present[i] &&
                      u.buckets[i].row_count ==
                      t.buckets[i].row_count,
                      "uniform and per-side part ways on bucket %d",
                      i);
        }
        mcme_atlas_mesh_free(&u);
        mcme_atlas_mesh_free(&t);
        mcme_mesh_free(&m6);
        mcme_mesh_free(&ma);
        mcme_mesh_free(&mw);
    }

    /* --- refusals --- */
    {
        McmeMesh m;
        CHECK(build_mesh(models, tex, "mc", "block/wrap", &m));
        McmeAtlasMesh t;
        CHECK(!mcme_atlas_mesh(NULL, &m, 0, MCME_TRANSPARENCY_PARTIAL,
                               &t));
        CHECK(!mcme_atlas_mesh(&atlas, NULL, 0, MCME_TRANSPARENCY_PARTIAL,
                               &t));
        CHECK(!mcme_atlas_mesh(&atlas, &m, 0, MCME_TRANSPARENCY_PARTIAL,
                               NULL));
        McmeTransparency nd[MCME_CULL_NONE];
        for (int i = 0; i < MCME_CULL_NONE; ++i)
            nd[i] = MCME_TRANSPARENCY_PARTIAL;
        CHECK(!mcme_atlas_mesh_dirs(NULL, &m, 0, nd, &t));
        CHECK(!mcme_atlas_mesh_dirs(&atlas, NULL, 0, nd, &t));
        CHECK(!mcme_atlas_mesh_dirs(&atlas, &m, 0, nd, NULL));
        mcme_clear_error();
        mcme_mesh_free(&m);
        mcme_atlas_mesh_free(NULL);
    }

    mcme_atlas_free(&atlas);
    mcme_model_index_close(models);
    mcme_texture_index_close(tex);
}

int mcme_test_resource(void)
{
    g_failed = 0;
    test_pack_manifest();
    test_blockstates();
    test_models();
    test_meshes();
    test_blockmeshes();
    test_atlases();
    test_atlas_uv();
    return g_failed;
}
