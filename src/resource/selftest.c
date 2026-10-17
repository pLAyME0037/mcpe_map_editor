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

int mcme_test_resource(void)
{
    g_failed = 0;
    test_pack_manifest();
    test_blockstates();
    test_models();
    test_meshes();
    return g_failed;
}
