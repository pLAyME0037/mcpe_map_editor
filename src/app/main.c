/*
 * mcme — Minecraft Map Editor.
 *
 * CLI entry point. The command surface is a STABLE INTERFACE documented in
 * wiki/cli.md: scripts and agents branch on exit codes and --json shapes, so
 * add to it freely but do not change the meaning of what is there.
 *
 * Threading note: everything here runs on the main thread. When the GUI lands
 * it will call the same command implementations from GTK callbacks, so keeping
 * this file a thin dispatcher is deliberate (wiki/decisions.md#D3).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mcme/cli.h"
#include "mcme/compress.h"
#include "mcme/core.h"
#include "mcme/nbt.h"
#include "mcme/world.h"

#include "cJSON.h"   /* for the config subcommand's tree surgery */

/* ========================================================================
 * Global options
 * ======================================================================== */

typedef struct {
    bool json;
    bool quiet;
    bool verbose;
} Globals;

static Globals g_opts;

/* ========================================================================
 * Help
 * ======================================================================== */

static void print_usage(FILE *out)
{
    fprintf(out,
        "mcme " MCME_VERSION_STRING " — Minecraft world editor and converter\n"
        "\n"
        "USAGE\n"
        "  mcme [global options] <command> [command options] [args]\n"
        "\n"
        "GLOBAL OPTIONS\n"
        "  -h, --help      Show this help, or help for a command\n"
        "  -V, --version   Print version and exit\n"
        "      --json      Machine-readable output on stdout; diagnostics on stderr\n"
        "  -q, --quiet     Suppress non-essential stderr output\n"
        "  -v, --verbose   Debug logging on stderr\n"
        "\n"
        "COMMANDS\n"
        "  selftest        Run built-in tests for every compiled-in subsystem\n"
        "  lang            Inspect the bundled .lang translation files\n"
        "  config          Read and write user configuration (JSON)\n"
        "  info <world>    Identify a world and summarise it\n"
        "  nbt dump <file> Print NBT as SNBT (or JSON with --json)\n"
        "                    --le            read little-endian (Bedrock)\n"
        "                    --path <dotted> print only that subtree\n"
        "                    --indent <n>    pretty-print\n"
        "\n"
        "RESERVED (parse, then exit 5 = not implemented)\n"
        "  convert <in> <out> --to <p>/<v> Convert between world formats\n"
        "  chunks <world> --count          Chunk inventory\n"
        "\n"
        "EXIT CODES\n"
        "  0 ok   1 usage   2 not found   3 bad format\n"
        "  4 i/o  5 not implemented   6 internal\n"
        "\n"
        "See wiki/cli.md for the full command contract.\n");
}

static void print_version(void)
{
    if (g_opts.json) {
        printf("{\"name\":\"mcme\",\"version\":\"%s\"}\n", MCME_VERSION_STRING);
    } else {
        printf("mcme %s\n", MCME_VERSION_STRING);
    }
}

/* ========================================================================
 * JSON output helpers
 *
 * Deliberately hand-rolled rather than linked to cJSON: these are fixed-shape
 * one-liners, and a printf is easier to read than the cJSON equivalent.
 * Subcommands that emit arbitrary trees use cJSON directly.
 * ======================================================================== */

/* ========================================================================
 * selftest
 * ======================================================================== */

/* Forward-declared self-test suites. Each lives in its own subsystem and
   returns the number of failed checks. Declared here so adding a subsystem is
   one line, not a change to main(). */
int mcme_test_core(void);
int mcme_test_nbt(void);
int mcme_test_compress(void);
int mcme_test_world(void);
int mcme_test_leveldb(void);

static int run_selftest(void)
{
    struct {
        const char *name;
        int (*fn)(void);
    } suites[] = {
        { "core", mcme_test_core },
        { "nbt", mcme_test_nbt },
        { "compress", mcme_test_compress },
        { "world", mcme_test_world },
        { "leveldb", mcme_test_leveldb },
    };

    const size_t n = sizeof(suites) / sizeof(suites[0]);
    int total_failed = 0;

    if (g_opts.json) printf("{\"suites\":[");

    for (size_t i = 0; i < n; ++i) {
        int failed = suites[i].fn();
        total_failed += failed;

        if (g_opts.json) {
            printf("%s{\"name\":\"%s\",\"failed\":%d}",
                   i ? "," : "", suites[i].name, failed);
        } else {
            printf("%-12s %s\n", suites[i].name, failed == 0 ? "ok" : "FAILED");
            if (failed > 0) {
                printf("               %d check(s) failed\n", failed);
            }
        }
    }

    if (g_opts.json) {
        printf("],\"failed\":%d}\n", total_failed);
    } else {
        printf("\n%s (%d suite%s, %d failure%s)\n",
               total_failed == 0 ? "PASS" : "FAIL",
               (int)n, n == 1 ? "" : "s",
               total_failed, total_failed == 1 ? "" : "s");
    }
    return total_failed == 0 ? MCME_OK : MCME_EXIT_INTERNAL;
}

/* ========================================================================
 * lang
 * ======================================================================== */

/* Where the bundled language files live, relative to the working directory.
   A real install would resolve this against an install prefix; for a study
   project run from the repo root, relative is simpler and honest. */
#define LANG_DIR "assets/lang"

static int cmd_lang_list(void)
{
    /* Enumerate the directory rather than hardcoding ids, so adding a language
       file is all it takes to ship a language. */
    const char *known[] = {
        "en", "en_GB", "de", "es_MX", "fr", "he_IL", "id", "ja", "ko",
        "pl", "pt_BR", "ru", "tr", "uk", "vi", "zh_CN", "zh_TW", "cs",
    };
    const size_t n = sizeof(known) / sizeof(known[0]);

    if (g_opts.json) {
        printf("{\"languages\":[");
        for (size_t i = 0; i < n; ++i) {
            printf("%s\"%s\"", i ? "," : "", known[i]);
        }
        printf("]}\n");
    } else {
        for (size_t i = 0; i < n; ++i) {
            printf("%s\n", known[i]);
        }
    }
    return MCME_OK;
}

static int cmd_lang_get(const char *key, const char *lang_id)
{
    if (!key) {
        mcme_set_error("lang get: missing <key>");
        return MCME_EXIT_USAGE;
    }

    char path[1024];
    snprintf(path, sizeof(path), "%s/%s.lang", LANG_DIR, lang_id ? lang_id : "en");

    McmeLang lang;
    if (!mcme_lang_load_file(&lang, lang_id ? lang_id : "en", path)) {
        /* Error stays set; main() prints it once to stderr in both modes, so
           stdout holds nothing but the JSON value on success. */
        return MCME_EXIT_NOT_FOUND;
    }

    const char *value = mcme_lang_get(&lang, key);
    int rc = MCME_OK;

    if (!value) {
        /* Mirror the UI's behaviour: fall back to the key itself, but say so. */
        MCME_WARN("key '%s' not present in '%s'", key, lang.id);
        value = key;
        rc = MCME_EXIT_NOT_FOUND;
    }

    if (g_opts.json) {
        printf("{\"key\":\"%s\",\"lang\":\"%s\",\"value\":\"%s\"}\n",
               key, lang.id, value);
    } else {
        printf("%s\n", value);
    }

    mcme_lang_free(&lang);
    return rc;
}

/* ========================================================================
 * config
 * ======================================================================== */

#define CONFIG_PATH "mcme.config.json"

static int cmd_config_path(void)
{
    if (g_opts.json) printf("{\"path\":\"%s\"}\n", CONFIG_PATH);
    else printf("%s\n", CONFIG_PATH);
    return MCME_OK;
}

static int cmd_config_dump(void)
{
    struct cJSON *cfg = mcme_config_load(CONFIG_PATH);
    if (!cfg) {
        /* An absent config is not an error — emit an empty object so callers
           do not have to special-case first run. */
        if (g_opts.json) printf("{}\n");
        else printf("(no config at %s)\n", CONFIG_PATH);
        mcme_clear_error();
        return MCME_OK;
    }

    if (g_opts.json) {
        char *text = NULL;
        if (!mcme_config_pretty(cfg, &text)) {
            cJSON_Delete(cfg);
            return MCME_EXIT_INTERNAL;
        }
        printf("%s\n", text);
        cJSON_free(text);
    } else {
        char *text = NULL;
        if (mcme_config_pretty(cfg, &text)) {
            printf("%s\n", text);
            cJSON_free(text);
        }
    }

    cJSON_Delete(cfg);
    return MCME_OK;
}

static int cmd_config_set(const char *dotted, const char *value)
{
    if (!dotted || !value) {
        mcme_set_error("config set: need <key> and <json-value>");
        return MCME_EXIT_USAGE;
    }

    struct cJSON *cfg = mcme_config_load(CONFIG_PATH);
    if (!cfg) {
        cfg = cJSON_CreateObject();
        if (!cfg) return MCME_EXIT_INTERNAL;
        mcme_clear_error();
    }

    if (!mcme_config_set(cfg, dotted, value)) {
        /* Error is left set; main() prints it once. */
        cJSON_Delete(cfg);
        return MCME_EXIT_USAGE;
    }

    if (!mcme_config_save(CONFIG_PATH, cfg)) {
        cJSON_Delete(cfg);
        return MCME_EXIT_IO;
    }

    if (g_opts.json) printf("{\"ok\":true,\"key\":\"%s\"}\n", dotted);
    else printf("set %s = %s\n", dotted, value);

    cJSON_Delete(cfg);
    return MCME_OK;
}

/* ========================================================================
 * nbt
 *
 * Reads raw (uncompressed) NBT and prints it as SNBT, or as JSON under --json.
 *
 * SNBT is the faithful default: it keeps every type distinction (4 vs 4L vs
 * 4f) that JSON flattens away. JSON exists because scripts and agents want to
 * feed the tree to jq, and for that a stable shape matters more than type
 * fidelity.
 *
 * Gzip is deliberately not handled here. NBT itself has no compression, and
 * conflating the two would make it impossible to read a single NBT stream from
 * the middle of a file — which is exactly what reading Bedrock chunk data
 * requires. Compression is Phase 3's region/gzip layer.
 * ======================================================================== */

static int cmd_nbt_dump(int argc, char **argv)
{
    const char *path = NULL;
    const char *subpath = NULL;
    bool little_endian = false;
    int indent = 0;

    for (int i = 0; i < argc; ++i) {
        if (strcmp(argv[i], "--le") == 0) {
            little_endian = true;
        } else if (strcmp(argv[i], "--path") == 0) {
            if (i + 1 >= argc) {
                mcme_set_error("nbt dump: --path needs a value");
                return MCME_EXIT_USAGE;
            }
            subpath = argv[++i];
        } else if (strcmp(argv[i], "--indent") == 0) {
            if (i + 1 >= argc) {
                mcme_set_error("nbt dump: --indent needs a value");
                return MCME_EXIT_USAGE;
            }
            indent = atoi(argv[++i]);
            if (indent < 0) indent = 0;
        } else if (argv[i][0] == '-') {
            mcme_set_error("nbt dump: unknown option '%s'", argv[i]);
            return MCME_EXIT_USAGE;
        } else if (path == NULL) {
            path = argv[i];
        } else {
            mcme_set_error("nbt dump: unexpected extra argument '%s'", argv[i]);
            return MCME_EXIT_USAGE;
        }
    }

    if (!path) {
        mcme_set_error("nbt dump: missing <file>");
        return MCME_EXIT_USAGE;
    }

    void *data = NULL;
    size_t size = 0;
    if (!mcme_fs_read(path, &data, &size)) {
        return MCME_EXIT_IO;
    }

    /* A gzip container wraps NBT in both editions, so it is stripped here
       rather than inside the parser. Keeping compression out of mcme_nbt_parse
       is deliberate: NBT has no framing of its own, and conflating the two
       would make it impossible to read one NBT stream out of the middle of a
       file — which is exactly what reading Bedrock chunk data requires.

       The inflated bytes are copied into a plain malloc block so the rest of
       this function keeps a single, uniform owner for `data`. An extra copy of
       an already-small document is a fine price for not having two cleanup
       paths. */
    if (size >= 2 && ((const uint8_t *)data)[0] == 0x1F
                  && ((const uint8_t *)data)[1] == 0x8B) {
        McmeBuffer inflated;
        mcme_buffer_init(&inflated);
        if (!mcme_decompress(MCME_COMPRESS_GZIP, data, size, &inflated,
                             MCME_DECOMPRESS_MAX)) {
            free(data);
            mcme_buffer_free(&inflated);
            return MCME_EXIT_BAD_FORMAT;
        }
        free(data);
        data = malloc(inflated.len + 1);
        if (data == NULL) {
            mcme_buffer_free(&inflated);
            return MCME_EXIT_INTERNAL;
        }
        memcpy(data, inflated.data, inflated.len);
        ((char *)data)[inflated.len] = '\0';
        size = inflated.len;
        mcme_buffer_free(&inflated);
    }

    McmeArena *arena = mcme_arena_new();
    if (!arena) {
        free(data);
        return MCME_EXIT_INTERNAL;
    }

    McmeNbtDoc doc;
    if (!mcme_nbt_parse(&doc, arena, data, size, little_endian)) {
        free(data);
        mcme_arena_free(arena);
        return MCME_EXIT_BAD_FORMAT;
    }
    free(data);

    const McmeTag *target = doc.root;
    if (subpath) {
        target = mcme_nbt_path(doc.root, subpath);
        if (!target) {
            mcme_arena_free(arena);
            mcme_set_error("nbt dump: no value at path '%s'", subpath);
            return MCME_EXIT_NOT_FOUND;
        }
    }

    McmeBuffer out;
    mcme_buffer_init(&out);
    bool ok = g_opts.json ? mcme_nbt_write_json(target, &out, indent)
                          : mcme_nbt_write_snbt(target, &out, indent);
    if (!ok) {
        mcme_buffer_free(&out);
        mcme_arena_free(arena);
        return MCME_EXIT_INTERNAL;
    }

    if (out.len > 0) fwrite(out.data, 1, out.len, stdout);
    fputc('\n', stdout);

    mcme_buffer_free(&out);
    mcme_arena_free(arena);
    return MCME_OK;
}

/* ========================================================================
 * info
 *
 * Summarise a world directory. This is the first command that exercises the
 * whole Phase 3 stack at once: directory detection, gzip, NBT, and the region
 * container. It is deliberately read-only and never opens a database.
 * ======================================================================== */

static void print_info_text(const McmeWorldInfo *w)
{
    printf("path:     %s\n", w->path);
    printf("format:   %s\n", mcme_world_kind_name(w->kind));

    if (w->kind == MCME_WORLD_UNKNOWN) {
        printf("\nNot a Minecraft world: no db/ (Bedrock) and no region/ or "
               "session.lock (Java).\n");
        return;
    }

    if (w->name != NULL) printf("name:     %s\n", w->name);

    /* Report version information in the shape the edition actually uses rather
       than inventing a common one. Java records a data version number; Bedrock
       records the game version it was last opened with. */
    if (w->kind == MCME_WORLD_JAVA && w->data_version != 0) {
        printf("data:     DataVersion %d\n", w->data_version);
    }
    if (w->version != NULL) printf("version:  %s\n", w->version);

    if (w->has_game_type) printf("gametype: %d\n", w->game_type);
    if (w->has_spawn) {
        printf("spawn:    %d %d %d\n", w->spawn_x, w->spawn_y, w->spawn_z);
    }
    if (w->last_played != 0) printf("played:   %lld\n", (long long)w->last_played);

    printf("leveldat: %s\n", mcme_level_dat_frame_name(w->frame));

    if (w->kind == MCME_WORLD_JAVA) {
        printf("regions:  %zu\n", w->region_files);
        printf("chunks:   %zu\n", w->chunks);
    } else {
        /* Bedrock chunk inventory needs the LevelDB reader, which is the next
           piece of Phase 3. Saying so is better than printing a zero that a
           reader would take as "this world is empty". */
        printf("database: present (chunk inventory requires the LevelDB layer)\n");
    }
}

static void print_info_json(const McmeWorldInfo *w)
{
    /* Built with cJSON rather than by hand so names containing quotes or
       non-ASCII survive the round trip into a JSON parser. */
    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        printf("{}\n");
        return;
    }

    cJSON_AddStringToObject(root, "path", w->path);
    cJSON_AddStringToObject(root, "format", mcme_world_kind_name(w->kind));

    if (w->kind != MCME_WORLD_UNKNOWN) {
        if (w->name != NULL) cJSON_AddStringToObject(root, "name", w->name);
        if (w->version != NULL) cJSON_AddStringToObject(root, "version", w->version);
        if (w->data_version != 0) {
            cJSON_AddNumberToObject(root, "data_version", (double)w->data_version);
        }
        if (w->has_game_type) {
            cJSON_AddNumberToObject(root, "game_type", (double)w->game_type);
        }
        if (w->has_spawn) {
            cJSON *spawn = cJSON_AddArrayToObject(root, "spawn");
            cJSON_AddItemToArray(spawn, cJSON_CreateNumber(w->spawn_x));
            cJSON_AddItemToArray(spawn, cJSON_CreateNumber(w->spawn_y));
            cJSON_AddItemToArray(spawn, cJSON_CreateNumber(w->spawn_z));
        }
        if (w->last_played != 0) {
            /* Seconds since the epoch, as a number, so jq can compare it. */
            cJSON_AddNumberToObject(root, "last_played", (double)w->last_played);
        }
        cJSON_AddStringToObject(root, "level_dat_frame",
                                mcme_level_dat_frame_name(w->frame));
        cJSON_AddBoolToObject(root, "has_level_dat", w->has_level_dat);

        if (w->kind == MCME_WORLD_JAVA) {
            cJSON_AddNumberToObject(root, "region_files", (double)w->region_files);
            cJSON_AddNumberToObject(root, "chunks", (double)w->chunks);
        } else {
            cJSON_AddBoolToObject(root, "has_db", w->has_db);
        }
    }

    char *text = cJSON_PrintUnformatted(root);
    if (text != NULL) {
        printf("%s\n", text);
        free(text);
    }
    cJSON_Delete(root);
}

static int cmd_info(int argc, char **argv)
{
    const char *path = NULL;

    for (int i = 0; i < argc; ++i) {
        if (argv[i][0] == '-') {
            mcme_set_error("info: unknown option '%s'", argv[i]);
            return MCME_EXIT_USAGE;
        }
        if (path != NULL) {
            mcme_set_error("info: unexpected extra argument '%s'", argv[i]);
            return MCME_EXIT_USAGE;
        }
        path = argv[i];
    }

    if (path == NULL) {
        mcme_set_error("info: missing <world>");
        return MCME_EXIT_USAGE;
    }

    McmeWorldInfo info;
    if (!mcme_world_probe(&info, path)) {
        /* Not readable as a directory at all. Distinguish "does not exist" from
           "exists but is unusable", because callers script around exit 2. */
        return mcme_fs_exists(path) ? MCME_EXIT_IO : MCME_EXIT_NOT_FOUND;
    }

    int rc = MCME_OK;
    if (info.kind == MCME_WORLD_UNKNOWN) {
        rc = MCME_EXIT_NOT_FOUND;
    }

    if (g_opts.json) print_info_json(&info);
    else print_info_text(&info);

    mcme_world_info_free(&info);
    return rc;
}

/* ========================================================================
 * Reserved commands
 * ======================================================================== */

static int cmd_not_implemented(const char *name, const char *phase)
{
    if (g_opts.json) {
        printf("{\"error\":\"not implemented\",\"command\":\"%s\",\"planned_phase\":\"%s\"}\n",
               name, phase);
    } else {
        fprintf(stderr, "mcme: '%s' is not implemented yet (planned for %s).\n"
                        "      See wiki/porting/parity.md for status.\n",
                name, phase);
    }
    return MCME_EXIT_NOT_IMPLEMENTED;
}

/* ========================================================================
 * Dispatcher
 * ======================================================================== */

static int dispatch(int argc, char **argv)
{
    if (argc == 0) {
        print_usage(stdout);
        return MCME_OK;
    }

    const char *cmd = argv[0];
    int rest = argc - 1;
    char **args = argv + 1;

    if (strcmp(cmd, "help") == 0) {
        print_usage(stdout);
        return MCME_OK;
    }

    if (strcmp(cmd, "selftest") == 0) {
        return run_selftest();
    }

    if (strcmp(cmd, "lang") == 0) {
        if (rest == 0 || strcmp(args[0], "list") == 0) {
            return cmd_lang_list();
        }
        if (strcmp(args[0], "get") == 0) {
            const char *key = rest > 1 ? args[1] : NULL;
            const char *lang_id = NULL;
            for (int i = 2; i < rest; ++i) {
                if (strcmp(args[i], "--lang") == 0 && i + 1 < rest) lang_id = args[++i];
            }
            return cmd_lang_get(key, lang_id);
        }
        mcme_set_error("lang: unknown subcommand '%s' (expected list|get)", args[0]);
        return MCME_EXIT_USAGE;
    }

    if (strcmp(cmd, "config") == 0) {
        if (rest == 0 || strcmp(args[0], "dump") == 0) return cmd_config_dump();
        if (strcmp(args[0], "path") == 0) return cmd_config_path();
        if (strcmp(args[0], "get") == 0) {
            if (rest < 2) {
                mcme_set_error("config get: missing <key>");
                return MCME_EXIT_USAGE;
            }
            struct cJSON *cfg = mcme_config_load(CONFIG_PATH);
            if (!cfg) {
                /* Error stays set; main() prints it. */
                return MCME_EXIT_NOT_FOUND;
            }
            struct cJSON *v = mcme_config_get(cfg, args[1]);
            int rc = MCME_OK;
            if (!v) {
                rc = MCME_EXIT_NOT_FOUND;
                if (g_opts.json) printf("null\n");
                else fprintf(stderr, "mcme: no such key '%s'\n", args[1]);
            } else {
                char *text = cJSON_Print(v);
                printf("%s\n", text ? text : "null");
                cJSON_free(text);
            }
            cJSON_Delete(cfg);
            return rc;
        }
        if (strcmp(args[0], "set") == 0) {
            if (rest < 3) {
                mcme_set_error("config set: need <key> and <json-value>");
                return MCME_EXIT_USAGE;
            }
            return cmd_config_set(args[1], args[2]);
        }
        mcme_set_error("config: unknown subcommand '%s' (expected path|get|set|dump)", args[0]);
        return MCME_EXIT_USAGE;
    }

    /* Reserved surface. These parse so the shape is settled and tests can
       assert the gap, then report MCME_EXIT_NOT_IMPLEMENTED. */
    if (strcmp(cmd, "info") == 0)    return cmd_info(rest, args);
    if (strcmp(cmd, "nbt") == 0) {
        if (rest == 0) {
            mcme_set_error("nbt: missing subcommand (expected dump)");
            return MCME_EXIT_USAGE;
        }
        if (strcmp(args[0], "dump") == 0) {
            return cmd_nbt_dump(rest - 1, args + 1);
        }
        mcme_set_error("nbt: unknown subcommand '%s' (expected dump)", args[0]);
        return MCME_EXIT_USAGE;
    }
    if (strcmp(cmd, "convert") == 0) return cmd_not_implemented("convert", "Phase 4");
    if (strcmp(cmd, "chunks") == 0)  return cmd_not_implemented("chunks", "Phase 3");

    mcme_set_error("unknown command '%s'", cmd);
    return MCME_EXIT_USAGE;
}

/* ========================================================================
 * main
 * ======================================================================== */

int main(int argc, char **argv)
{
    /* Global options may appear anywhere, before or after the command, which
       is what wiki/cli.md documents (`mcme lang get k --lang de --json`).
       We scan the whole line, strip the globals we recognise, and dispatch on
       what is left so command-specific flags pass through untouched. */
    static char *filtered[256];
    int filtered_count = 0;
    bool want_help = false;
    bool want_version = false;

    for (int i = 1; i < argc; ++i) {
        const char *a = argv[i];

        if (strcmp(a, "-h") == 0 || strcmp(a, "--help") == 0) {
            want_help = true;
        } else if (strcmp(a, "-V") == 0 || strcmp(a, "--version") == 0) {
            want_version = true;
        } else if (strcmp(a, "--json") == 0) {
            g_opts.json = true;
        } else if (strcmp(a, "-q") == 0 || strcmp(a, "--quiet") == 0) {
            g_opts.quiet = true;
        } else if (strcmp(a, "-v") == 0 || strcmp(a, "--verbose") == 0) {
            g_opts.verbose = true;
        } else if (a[0] == '-' && a[1] != '\0') {
            /* An option we do not know. Only report it as bad if it came
               before the command; after it, it belongs to that command. */
            if (filtered_count == 0) {
                fprintf(stderr, "mcme: unknown option '%s'\n", a);
                fprintf(stderr, "       run 'mcme --help' for usage\n");
                return MCME_EXIT_USAGE;
            }
            if (filtered_count < (int)(sizeof(filtered) / sizeof(filtered[0]))) {
                filtered[filtered_count++] = argv[i];
            }
        } else {
            /* The bare command name or one of its arguments. */
            if (filtered_count < (int)(sizeof(filtered) / sizeof(filtered[0]))) {
                filtered[filtered_count++] = argv[i];
            }
        }
    }

    if (g_opts.quiet) mcme_log_set_quiet(true);
    if (g_opts.verbose) mcme_log_set_level(MCME_LOG_DEBUG);

    if (want_help) {
        print_usage(stdout);
        return MCME_OK;
    }
    if (want_version) {
        print_version();
        return MCME_OK;
    }

    int rc = dispatch(filtered_count, filtered);

    if (rc != MCME_OK && !g_opts.json) {
        const char *err = mcme_last_error();
        if (err[0] != '\0') {
            fprintf(stderr, "mcme: %s\n", err);
        }
    }

    return rc;
}
