#define NOB_IMPLEMENTATION
#include "third_party/nob/nob.h"

/*
 * mcme build script.
 *
 * Design notes (wiki/simplexity.md):
 *   - We walk src/ with plain POSIX readdir rather than nob's file APIs. Fewer
 *     moving parts, and it is obvious what it does.
 *   - Each .c compiles to its own object so a one-line change does not recompile
 *     the world. Our sources get -Werror; third_party does not, because we do
 *     not own their warnings.
 *   - Two binaries: `mcme` (the tool) and `mcme_tests` (unit tests, which link
 *     everything except src/app/main.c so there is no duplicate main).
 */

#include <dirent.h>
#include <string.h>
#include <sys/stat.h>

#define MAX_SOURCES 2048

static const char *g_sources[MAX_SOURCES];
static size_t g_source_count = 0;

static bool ends_with(const char *s, const char *suffix)
{
    size_t ls = strlen(s), lx = strlen(suffix);
    return ls >= lx && strcmp(s + ls - lx, suffix) == 0;
}

/* Recursively collect every .c under `dir` into g_sources. */
static void collect_c(const char *dir)
{
    DIR *d = opendir(dir);
    if (!d) {
        nob_log(NOB_ERROR, "cannot open directory: %s", dir);
        return;
    }

    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) continue;

        char path[4096];
        int n = snprintf(path, sizeof(path), "%s/%s", dir, e->d_name);
        if (n < 0 || (size_t)n >= sizeof(path)) {
            nob_log(NOB_ERROR, "path too long: %s/%s", dir, e->d_name);
            continue;
        }

        struct stat st;
        if (stat(path, &st) != 0) continue;

        if (S_ISDIR(st.st_mode)) {
            collect_c(path);
        } else if (ends_with(e->d_name, ".c")) {
            if (g_source_count >= MAX_SOURCES) {
                nob_log(NOB_ERROR, "too many sources (max %d)", MAX_SOURCES);
                break;
            }
            g_sources[g_source_count++] = strdup(path);
        }
    }
    closedir(d);
}

/* Vendored implementation units. These are single-file C libraries we compile
   ourselves rather than link, so the build stays self-contained and offline.
   Anything added here must be permissive-licensed (see wiki/decisions.md#D5).
   Only list what the current code actually uses — an unused TU is an unused
   compile, and (as miniz showed) an unused dependency is an unused way to
   break the build.
 *
 * miniz is intentionally absent: it needs a CMake-generated miniz_export.h
 * that upstream does not ship. We need gzip for the Anvil region format in
 * Phase 3; at that point either generate the header or define the export
 * macro. Not before. */
static const char *g_vendored[] = {
    "third_party/cjson/cJSON.c",
    "third_party/lz4/lz4.c",
};
static const size_t g_vendored_count = sizeof(g_vendored) / sizeof(g_vendored[0]);

static void collect_tests(void)
{
    DIR *d = opendir("tests/unit");
    if (!d) return; /* no tests yet is fine */

    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (!ends_with(e->d_name, ".c")) continue;
        char path[4096];
        int n = snprintf(path, sizeof(path), "tests/unit/%s", e->d_name);
        if (n < 0 || (size_t)n >= sizeof(path)) continue;
        g_sources[g_source_count++] = strdup(path);
    }
    closedir(d);
}

/* src/foo/bar.c -> build/obj/src_foo_bar.o
 *
 * Returns a heap string the caller owns (and a build tool is allowed to leak
 * until exit). This matters: nob_cmd_append stores POINTERS, so returning one
 * shared static buffer would make every appended argument alias the last path
 * written into it — which is exactly the bug this avoids. */
static char *obj_path_for(const char *src)
{
    char buf[4096];
    size_t j = 0;

    const char *prefix = "build/obj/";
    for (size_t k = 0; prefix[k] != '\0' && j + 6 < sizeof(buf); ++k) {
        buf[j++] = prefix[k];
    }
    for (size_t i = 0; src[i] != '\0' && j + 4 < sizeof(buf); ++i) {
        char c = src[i];
        if (c == '/') c = '_';
        buf[j++] = c;
    }
    buf[j++] = '.';
    buf[j++] = 'o';
    buf[j] = '\0';
    return strdup(buf);
}

/* Compile one TU. Returns false on failure. */
static bool compile_one(const char *src, bool release, bool is_ours)
{
    Nob_Cmd cmd = {0};
    nob_cmd_append(&cmd, "cc", "-std=gnu11", "-c", src, "-o", obj_path_for(src));
    nob_cmd_append(&cmd,
        "-I", "include",
        "-I", "third_party",
        "-I", "third_party/stb",
        "-I", "third_party/cjson",
        "-I", "third_party/miniz",
        "-I", "third_party/lz4",
        "-Wall", "-Wextra");

    if (is_ours) {
        nob_cmd_append(&cmd, "-Werror");
        nob_cmd_append(&cmd, "-Wno-unused-parameter");
    }

    if (release) {
        nob_cmd_append(&cmd, "-O2", "-DNDEBUG");
    } else {
        /* ASan/UBSan catch the class of bug a C port of a world parser makes. */
        nob_cmd_append(&cmd, "-O0", "-g",
                       "-fsanitize=address,undefined",
                       "-fno-omit-frame-pointer");
    }

    return nob_cmd_run_sync(cmd);
}

int main(int argc, char **argv)
{
    (void)argc; (void)argv;
    /* nob 3.x needs no init: its temp allocator is a static arena and
       NOB_GO_REBUILD_URSELF is not used here because rebuild.sh already
       recompiles build.c on every run. */

    bool release = false;
    bool build_tests = false;
    bool build_fuzz = false;
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "release") == 0) release = true;
        else if (strcmp(argv[i], "test") == 0) build_tests = true;
        else if (strcmp(argv[i], "fuzz") == 0) build_fuzz = true;
        else {
            nob_log(NOB_ERROR, "unknown build option: %s", argv[i]);
            nob_log(NOB_INFO, "usage: ./rebuild.sh [release] [test] [fuzz]");
            return 1;
        }
    }

    if (!nob_mkdir_if_not_exists("build")) return 1;
    if (!nob_mkdir_if_not_exists("build/obj")) return 1;

    collect_c("src");
    if (g_source_count == 0) {
        nob_log(NOB_ERROR, "no .c files found under src/");
        return 1;
    }

    /* Vendored libraries are appended after our own sources so the compile and
       link loops can treat [0, mcme_object_count) as everything mcme needs,
       and the tail beyond that as test-only units. Vendored code skips -Werror:
       we do not own their warnings. */
    for (size_t i = 0; i < g_vendored_count; ++i) {
        g_sources[g_source_count++] = strdup(g_vendored[i]);
    }
    size_t mcme_object_count = g_source_count;

    /*
     * Fuzz targets. Built with clang because libFuzzer (-fsanitize=fuzzer) has
     * no gcc equivalent, and deliberately kept out of the default build so a
     * normal `./rebuild.sh` stays a plain cc build.
     *
     * Each harness links the whole library and supplies its own main, so the
     * sources are passed straight to the compiler rather than going through the
     * gcc object pipeline used for ./mcme. Compiling ~20 TUs a second is a fair
     * price for never having two compile paths disagree about flags.
     *
     * See the files under tests/fuzz for the invariants each target asserts.
     */
    if (build_fuzz) {
        static const struct {
            const char *src;
            const char *out;
        } targets[] = {
            { "tests/fuzz/fuzz_nbt.c",  "build/fuzz_nbt"  },
            { "tests/fuzz/fuzz_snbt.c", "build/fuzz_snbt" },
        };
        const size_t target_count = sizeof(targets) / sizeof(targets[0]);

        for (size_t t = 0; t < target_count; ++t) {
            Nob_Cmd cmd = {0};
            nob_cmd_append(&cmd,
                "clang", "-std=gnu11", "-g", "-O1",
                "-fsanitize=fuzzer,address,undefined",
                "-fno-omit-frame-pointer",
                "-I", "include",
                "-I", "third_party",
                "-I", "third_party/stb",
                "-I", "third_party/cjson",
                "-I", "third_party/lz4",
                "-Wall", "-Wextra", "-Werror", "-Wno-unused-parameter",
                "-o", targets[t].out);
            for (size_t i = 0; i < mcme_object_count; ++i) {
                /* The CLI's main would collide with libFuzzer's. */
                if (strcmp(g_sources[i], "src/app/main.c") == 0) continue;
                nob_cmd_append(&cmd, g_sources[i]);
            }
            nob_cmd_append(&cmd, targets[t].src, "-lm", "-lpthread");

            if (!nob_cmd_run_sync(cmd)) {
                nob_log(NOB_ERROR, "failed to build %s", targets[t].out);
                return 1;
            }
            nob_log(NOB_INFO, "built ./%s", targets[t].out);
        }
        nob_log(NOB_INFO, "run: ./build/fuzz_nbt -max_total_time=60");
        return 0;
    }

    if (build_tests) collect_tests();

    /* Build objects. Third-party code is compiled without -Werror: we do not
       own their warnings. */
    for (size_t i = 0; i < mcme_object_count; ++i) {
        const char *src = g_sources[i];
        bool is_ours = strncmp(src, "third_party/", 12) != 0;
        if (!compile_one(src, release, is_ours)) {
            nob_log(NOB_ERROR, "failed to compile %s", src);
            return 1;
        }
    }

    const char *sanitizers[] = { "-fsanitize=address,undefined" };

    /* --- link mcme --- */
    {
        Nob_Cmd cmd = {0};
        nob_cmd_append(&cmd, "cc");
        if (!release) nob_cmd_append(&cmd, sanitizers[0]);
        nob_cmd_append(&cmd, "-o", "mcme");
        for (size_t i = 0; i < mcme_object_count; ++i) {
            nob_cmd_append(&cmd, obj_path_for(g_sources[i]));
        }
        nob_cmd_append(&cmd, "-lm", "-lpthread");
        if (!nob_cmd_run_sync(cmd)) {
            nob_log(NOB_ERROR, "link failed for mcme");
            return 1;
        }
        nob_log(NOB_INFO, "built ./mcme (%s)", release ? "release" : "debug, ASan+UBSan");
    }

    /* --- link and optionally run mcme_tests --- */
    if (build_tests) {
        /* Test units still need compiling (they were collected after ours). */
        for (size_t i = mcme_object_count; i < g_source_count; ++i) {
            if (!compile_one(g_sources[i], release, true)) {
                nob_log(NOB_ERROR, "failed to compile %s", g_sources[i]);
                return 1;
            }
        }

        Nob_Cmd cmd = {0};
        nob_cmd_append(&cmd, "cc");
        if (!release) nob_cmd_append(&cmd, sanitizers[0]);
        nob_cmd_append(&cmd, "-o", "build/mcme_tests");
        for (size_t i = 0; i < g_source_count; ++i) {
            /* skip the CLI entry point: the test binary has its own main */
            if (strcmp(g_sources[i], "src/app/main.c") == 0) continue;
            nob_cmd_append(&cmd, obj_path_for(g_sources[i]));
        }
        nob_cmd_append(&cmd, "-lm", "-lpthread");
        if (!nob_cmd_run_sync(cmd)) {
            nob_log(NOB_ERROR, "link failed for mcme_tests");
            return 1;
        }
        nob_log(NOB_INFO, "built ./build/mcme_tests");

        Nob_Cmd run = {0};
        nob_cmd_append(&run, "build/mcme_tests");
        if (!nob_cmd_run_sync(run)) {
            nob_log(NOB_ERROR, "unit tests FAILED");
            return 1;
        }
    }

    return 0;
}
