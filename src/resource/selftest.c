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

int mcme_test_resource(void)
{
    g_failed = 0;
    test_pack_manifest();
    return g_failed;
}
