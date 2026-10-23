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

#include <errno.h>   /* strtol's range check, for --y */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>     /* gmtime_r, for the region inventory's timestamps */

#include "mcme/bedrock_chunk.h"
#include "mcme/bedrock_entity.h"
#include "mcme/bedrock_keys.h"
#include "mcme/bedrock_write.h"
#include "mcme/cli.h"
#include "mcme/compress.h"
#include "mcme/convert.h"
#include "mcme/core.h"
#include "mcme/java_chunk.h"
#include "mcme/leveldb.h"
#include "mcme/nbt.h"
#include "mcme/prune.h"
#include "mcme/region.h"
#include "mcme/resource.h"
#include "mcme/world.h"

#include "cJSON.h"   /* for the config subcommand's tree surgery */
#include "stb_ds.h"

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
        "  pack info <dir>  Summarise a Java resource pack's manifest\n"
        "  blockstate <pack> <block>\n"
        "                    Which model(s) the pack's blockstates select\n"
        "                    for the block state — model, x, y, uvlock\n"
        "  model <pack> <model-ref>\n"
        "                    One model file with its parent chain\n"
        "                    folded in: textures per key, elements\n"
        "                    wholesale, everything else discarded\n"
        "  mesh <pack> <model-ref>\n"
        "                    One model as the game would build it:\n"
        "                    vertices, UVs and faces grouped by cull\n"
        "                    direction, its resolved textures, the\n"
        "                    block's transparency\n"
        "  blockmesh <pack> <block>\n"
        "                    One block state as the game would mesh\n"
        "                    it: the selection's winning model(s)\n"
        "                    meshed and turned by x/y, multipart's\n"
        "                    cases merged, a refusing model skipped\n"
        "                    the way the reference catches it\n"
        "  atlas <pack> [--out <png>]\n"
        "                    Every texture the pack ships on one\n"
        "                    square page: its size and where each\n"
        "                    texture landed as bounds; --out writes\n"
        "                    the page PNG\n"
        "  atlasmesh <pack> <block> [--y <n>] [--beside <block>]\n"
        "                    One block state, its texture coordinates\n"
        "                    in atlas space: every row carrying the\n"
        "                    rectangle its triangle draws from, the\n"
        "                    mesh's coordinates mapped into it, and\n"
        "                    the tint under the builder's two folds;\n"
        "                    --y is the block's world height, what\n"
        "                    the band shade reads; --beside is the\n"
        "                    block standing on all six sides — its\n"
        "                    transparency is what the cull clauses\n"
        "                    read, so the rows are the survivors\n"
        "  info <world>    Identify a world and summarise it\n"
        "  chunks <world>  Chunk inventory\n"
        "                    --count         print only the number\n"
        "  region <world> <rx> <rz>\n"
        "                    What one Java region file holds: which\n"
        "                    chunks, each payload's compression and\n"
        "                    size, each timestamp — the container\n"
        "                    only, no chunk's NBT opened. Takes\n"
        "                    --dimension overworld|nether|end|<number>.\n"
        "  region compact <world> <rx> <rz>\n"
        "                    Rewrite a region file packed: live\n"
        "                    chunks only, in slot order, the\n"
        "                    slack from earlier writes gone.\n"
        "                    Silent on success.\n"
        "  region chunk <world> <cx> <cz>\n"
        "                    One Java chunk's NBT — SNBT, or JSON\n"
        "                    with --json. cx, cz are chunk\n"
        "                    coordinates, not blocks. Takes\n"
        "                    --dimension too.\n"
        "  region chunk set <world> <cx> <cz> <snbt>\n"
        "                    Write one Java chunk back: the\n"
        "                    document replaces the payload,\n"
        "                    appended and repointed. Silent on\n"
        "                    success.\n"
        "  chunk set <world> <x> <y> <z> <block>\n"
        "                    Set one block — Bedrock, or Java\n"
        "                    (1.18+ palettes). <block> is a name,\n"
        "                    or a name with SNBT states in braces:\n"
        "                    minecraft:oak_log{pillar_axis:\"x\"}\n"
        "                    Bedrock: the subchunk is created if\n"
        "                    the world has not stored it; takes\n"
        "                    the LOCK. Java: the section and palette\n"
        "                    entry are created; the chunk itself\n"
        "                    must be stored. --dimension\n"
        "                    overworld|nether|end|<number>.\n"
        "  chunk get <world> <x> <y> <z>\n"
        "                    Print one block as the spec `chunk set`\n"
        "                    accepts — Bedrock or Java. Exit 2 if\n"
        "                    the subchunk or chunk is not stored.\n"
        "                    Takes --dimension too.\n"
        "  chunk mesh <world> <pack> <x> <y> <z>\n"
        "                    The builder's table for one block of a\n"
        "                    real chunk: its faces in atlas space,\n"
        "                    the six neighbours read from the world\n"
        "                    itself, the cull answered per side.\n"
        "                    A neighbour's chunk outside the stored\n"
        "                    world is air. Human or --json; takes\n"
        "                    --dimension too.\n"
        "  chunk submesh <world> <pack> <x> <y> <z>\n"
        "                    The same table for the WHOLE sub-chunk\n"
        "                    the block stands in: every cell the\n"
        "                    cull leaves rows, each with its six\n"
        "                    real neighbours, the sub-chunks beside\n"
        "                    it included. Human or --json; takes\n"
        "                    --dimension too.\n"
        "  chunk fill <world> <x1> <y1> <z1> <x2> <y2> <z2> <block>\n"
        "                    Set a box of blocks (corners in any\n"
        "                    order), Bedrock or Java. Bedrock:\n"
        "                    subchunks are created, one payload\n"
        "                    write each. Java: one repack per\n"
        "                    section; a chunk not stored refuses\n"
        "                    the whole fill, nothing written.\n"
        "  chunk prune <world> <x1> <y1> <z1> <x2> <y2> <z2>\n"
        "                    Delete whole chunks — the box names\n"
        "                    the chunk columns (Y is parsed and\n"
        "                    ignored). Bedrock: every key the\n"
        "                    chunk owns, one batch, LOCK taken.\n"
        "                    Java: the slot zeroed, the payload\n"
        "                    bytes left for region compact.\n"
        "                    Silent on success; --json counts.\n"
        "  chunk stats <world> <x1> <y1> <z1> <x2> <y2> <z2>\n"
        "                    Count what a box is made of: 'count\n"
        "                    spec' lines, most frequent first.\n"
        "                    Unstored subchunks count as air\n"
        "                    (exit 0, noted on stderr).\n"
        "  chunk height <world> <x> <z>\n"
        "                    How high the ground is in the column\n"
        "                    through (x, z): the subchunks stored\n"
        "                    there, the topmost and bottommost\n"
        "                    non-air block. Exit 2 if the column\n"
        "                    was never stored.\n"
        "  chunk entity <world> <x> <y> <z>\n"
        "                    The block entity at one block — a\n"
        "                    chest's contents, a sign's text —\n"
        "                    as SNBT, or JSON with --json. Exit 2\n"
        "                    if none is stored there.\n"
        "  chunk entity set <world> <x> <y> <z> <snbt>\n"
        "                    Store a block entity at one block:\n"
        "                    the document in SNBT, the position\n"
        "                    its own coordinates are set from.\n"
        "                    The record's other documents go\n"
        "                    back byte for byte.\n"
        "  chunk entity del <world> <x> <y> <z>\n"
        "                    Remove the block entity at one\n"
        "                    block; the last one removes the\n"
        "                    record. Exit 2 if none is there.\n"
        "  db <op> <world> <key-hex> [value-hex]\n"
        "                    Raw Bedrock store access. op = get | put | del.\n"
        "                    Keys and values are hex; get prints value-hex.\n"
        "                    put/del take the store LOCK; get does not.\n"
        "  db compact <world>\n"
        "                    Flush the store's log into one new table and\n"
        "                    remove what it replaces. Takes the LOCK.\n"
        "  nbt dump <file> Print NBT as SNBT (or JSON with --json)\n"
        "                    --le            read little-endian (Bedrock)\n"
        "                    --path <dotted> print only that subtree\n"
        "                    --indent <n>    pretty-print\n"
        "  convert <in> <out> --to <p>/<v>\n"
        "                    Convert a Bedrock world to a Java 1.20\n"
        "                    world: region files, level.dat with the\n"
        "                    seed carried over, blocks through the\n"
        "                    map. <out> must not exist. Entities,\n"
        "                    block entities and water layers are\n"
        "                    dropped and reported, not failed. Other\n"
        "                    targets exit 5 (not implemented yet).\n"
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
int mcme_test_resource(void);

static int run_selftest(void)
{
    struct {
        const char *name;
        int (*fn)(void);
    } suites[] = {
        { "core"    , mcme_test_core },
        { "nbt"     , mcme_test_nbt },
        { "compress", mcme_test_compress },
        { "world"   , mcme_test_world },
        { "leveldb" , mcme_test_leveldb },
        { "resource", mcme_test_resource },
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
 * pack
 *
 * A Java resource pack's manifest. The reference reader (the project's
 * oracle for Phase 5) calls a directory a pack by one test — pack.mcmeta is
 * a file — and reads pack_format, description, and validity. `pack info`
 * reports exactly that; the model and atlas layers come later.
 * ======================================================================== */

static int cmd_pack_info(const char *dir)
{
    if (!dir) {
        mcme_set_error("pack info: missing <dir>");
        return MCME_EXIT_USAGE;
    }

    McmePack *pack = NULL;
    if (!mcme_pack_open(dir, &pack)) {
        /* mcme_last_error() already names the refusal. */
        return MCME_EXIT_NOT_FOUND;
    }

    if (g_opts.json) {
        /* Build one object and print it — the same shape the human output
           carries, so --json callers parse one stable schema. */
        struct cJSON *o = cJSON_CreateObject();
        if (o) {
            cJSON_AddItemToObject(o, "dir", cJSON_CreateString(pack->root));
            cJSON_AddItemToObject(o, "format", cJSON_CreateNumber(pack->format));
            cJSON_AddItemToObject(o, "description",
                                  cJSON_CreateString(pack->description));
            cJSON_AddItemToObject(o, "has_icon",
                                  cJSON_CreateBool(pack->has_icon));
            char *t = cJSON_PrintUnformatted(o);
            printf("%s\n", t ? t : "null");
            if (t) cJSON_free(t);
            cJSON_Delete(o);
        } else {
            printf("null\n");
        }
    } else {
        printf("%s\n", pack->root);
        printf("  pack_format  %d\n", pack->format);
        printf("  description  %s\n", pack->description);
        printf("  pack.png     %s\n", pack->has_icon ? "yes" : "no");
    }

    mcme_pack_free(pack);
    return MCME_OK;
}

/* ========================================================================
 * blockstate
 *
 * `mcme blockstate <pack-dir> <block>` — which model(s) the pack's
 * blockstates select for one block state, the CLI face of the D30
 * selection layer. A selection is an answer, never a refusal: a
 * block the pack holds no state file for prints `missing` and
 * exits 0, because that is what the reference answers too (its
 * renderer then draws the missing texture). Only the arguments
 * refuse — a pack directory that does not exist is exit 2, so a
 * typo never reads as "every block is missing".
 * ======================================================================== */

/* Shared with the chunk verbs below; defined with them. */
static int parse_block_spec(const char *verb, const char *spec,
                            McmeArena *arena, char *name,
                            size_t name_cap, const McmeTag **states);

static int cmd_blockstate(const char *pack_dir, const char *spec)
{
    if (!mcme_fs_exists(pack_dir)) {
        mcme_set_error("blockstate: '%s' is not a directory", pack_dir);
        return MCME_EXIT_NOT_FOUND;
    }

    McmeArena *arena = mcme_arena_new();
    if (!arena) {
        mcme_set_error("blockstate: out of memory");
        return MCME_EXIT_INTERNAL;
    }

    char name[256];
    const McmeTag *states = NULL;
    int rc = parse_block_spec("blockstate", spec, arena, name,
                              sizeof name, &states);
    if (rc != MCME_OK) {
        mcme_arena_free(arena);
        return rc;
    }

    /* Split the name into namespace and block, defaulting the way
       the game does. */
    char ns[256];
    const char *colon = strchr(name, ':');
    const char *base;
    if (colon) {
        size_t ns_len = (size_t)(colon - name);
        if (ns_len == 0 || ns_len >= sizeof ns) {
            mcme_set_error("blockstate: block '%s' has an empty or "
                           "too-long namespace", spec);
            mcme_arena_free(arena);
            return MCME_EXIT_USAGE;
        }
        memcpy(ns, name, ns_len);
        ns[ns_len] = '\0';
        base = colon + 1;
    } else {
        snprintf(ns, sizeof ns, "minecraft");
        base = name;
    }
    if (*base == '\0') {
        mcme_set_error("blockstate: block '%s' has an empty name", spec);
        mcme_arena_free(arena);
        return MCME_EXIT_USAGE;
    }

    /* A state the game's own properties never spell: Java
       properties are strings, but a store's states are its own
       tags — Bedrock types them. The reference compares a
       property's py_data against the variant's text, and a
       typed value is never equal to text, so a typed state
       MATCHES NOTHING and the selection falls through around it
       (D43). It is carried, not coerced: a coercion could
       masquerade as a property the key names. */
    McmeState props[64];
    size_t prop_count = 0;
    if (states) {
        for (size_t i = 0; i < states->compound.len; ++i) {
            const McmeCompoundEntry *e = &states->compound.entries[i];
            if (prop_count >= sizeof props / sizeof props[0]) {
                mcme_set_error("blockstate: block '%s' has more than "
                               "%zu states", spec,
                               sizeof props / sizeof props[0]);
                mcme_arena_free(arena);
                return MCME_EXIT_USAGE;
            }
            props[prop_count].name  = e->name.data;
            props[prop_count].value = e->value->type == MCME_TAG_String
                ? e->value->string.data : NULL;
            prop_count++;
        }
    }

    McmeBlockstateIndex *index = NULL;
    if (!mcme_blockstate_index_open(pack_dir, &index)) {
        char why[256];
        snprintf(why, sizeof why, "%s", mcme_last_error());
        mcme_set_error("blockstate: %s", why);
        mcme_arena_free(arena);
        return MCME_EXIT_INTERNAL;
    }

    McmeBlockstate answer;
    if (!mcme_blockstate_lookup(index, ns, base, props, prop_count,
                                &answer)) {
        char why[256];
        snprintf(why, sizeof why, "%s", mcme_last_error());
        mcme_set_error("blockstate: %s", why);
        mcme_blockstate_index_close(index);
        mcme_arena_free(arena);
        return MCME_EXIT_INTERNAL;
    }
    mcme_blockstate_index_close(index);

    if (g_opts.json) {
        struct cJSON *o = cJSON_CreateObject();
        if (o) {
            char full[512];
            snprintf(full, sizeof full, "%s:%s", ns, base);
            cJSON_AddItemToObject(o, "block", cJSON_CreateString(full));
            cJSON_AddItemToObject(o, "missing", cJSON_CreateBool(answer.missing));
            cJSON_AddItemToObject(o, "models", cJSON_CreateArray());
            cJSON *arr = cJSON_GetObjectItemCaseSensitive(o, "models");
            for (size_t i = 0; i < answer.count; ++i) {
                const McmeModelRef *ref = &answer.models[i];
                cJSON *m = cJSON_CreateObject();
                cJSON_AddItemToObject(m, "model",
                    ref->model ? cJSON_CreateString(ref->model)
                               : cJSON_CreateNull());
                cJSON_AddItemToObject(m, "x", cJSON_CreateNumber(ref->x));
                cJSON_AddItemToObject(m, "y", cJSON_CreateNumber(ref->y));
                cJSON_AddItemToObject(m, "uvlock", cJSON_CreateBool(ref->uvlock));
                cJSON_AddItemToArray(arr, m);
            }
            char *text = cJSON_PrintUnformatted(o);
            printf("%s\n", text ? text : "null");
            if (text) cJSON_free(text);
            cJSON_Delete(o);
        } else {
            printf("null\n");
        }
    } else {
        printf("%s:%s", ns, base);
        for (size_t i = 0; i < prop_count; ++i) {
            if (props[i].value) {
                printf("%s%s:\"%s\"", i ? "," : "{", props[i].name,
                       props[i].value);
            } else {
                /* a typed state, spelled as the spec format
                   spells it: the tag's own SNBT, round-tripping
                   what was asked (D43) */
                McmeBuffer out;
                mcme_buffer_init(&out);
                if (mcme_nbt_write_snbt(
                        states->compound.entries[i].value, &out, 0))
                    printf("%s%s:%.*s", i ? "," : "{",
                           props[i].name, (int)out.len, out.data);
                else
                    printf("%s%s:?", i ? "," : "{", props[i].name);
                mcme_buffer_free(&out);
            }
        }
        if (prop_count) printf("}");
        if (answer.missing) {
            printf("  (no blockstate in this pack)\n");
        } else {
            printf("  %zu model%s\n", answer.count,
                   answer.count == 1 ? "" : "s");
            for (size_t i = 0; i < answer.count; ++i) {
                const McmeModelRef *ref = &answer.models[i];
                printf("  %s  x %d y %d  uvlock %s\n",
                       ref->model ? ref->model : "(no model)",
                       ref->x, ref->y, ref->uvlock ? "true" : "false");
            }
        }
    }

    mcme_blockstate_free(&answer);
    mcme_arena_free(arena);
    return MCME_OK;
}

/* ========================================================================
 * model
 *
 * `mcme model <pack-dir> <model-ref>` — one model file with every
 * parent folded in, the CLI face of the D31 layer. The fold is the
 * reference reader's own answer: textures merge per key up the
 * chain, elements override wholesale, every other key (display,
 * gui_light, parent) is discarded. A model the pack does not hold
 * answers the empty object `{}` and exits 0 — the reference's own
 * answer for a missing file, at any level of the chain. The
 * arguments refuse: a pack directory that does not exist is exit
 * 2, a malformed reference exit 1, and a parent chain that will
 * not fold (a cycle, in practice) is the pack's bad data, exit 3.
 * ======================================================================== */

/* Splits a model or mesh reference the way the game writes it: an
   optional `namespace:` prefix, defaulting to minecraft. Returns
   MCME_OK, or MCME_EXIT_USAGE with the error saying which
   malformed shape was refused. */
static int split_ref(const char *what, const char *ref,
                     char *ns, size_t ns_size, const char **base)
{
    const char *colon = strchr(ref, ':');
    if (colon) {
        size_t ns_len = (size_t)(colon - ref);
        if (ns_len == 0 || ns_len >= ns_size) {
            mcme_set_error("%s: reference '%s' has an empty or "
                           "too-long namespace", what, ref);
            return MCME_EXIT_USAGE;
        }
        memcpy(ns, ref, ns_len);
        ns[ns_len] = '\0';
        *base = colon + 1;
    } else {
        snprintf(ns, ns_size, "minecraft");
        *base = ref;
    }
    if (**base == '\0') {
        mcme_set_error("%s: reference '%s' has an empty path", what, ref);
        return MCME_EXIT_USAGE;
    }
    return MCME_OK;
}

static int cmd_model(const char *pack_dir, const char *ref)
{
    if (!mcme_fs_exists(pack_dir)) {
        mcme_set_error("model: '%s' is not a directory", pack_dir);
        return MCME_EXIT_NOT_FOUND;
    }

    /* Split the reference the way the game writes it: an optional
       `namespace:` prefix, defaulting to minecraft. An empty
       namespace or path is a typo, refused here so it can never
       read as "the pack holds no such model". */
    char ns[256];
    const char *base;
    int rc = split_ref("model", ref, ns, sizeof ns, &base);
    if (rc != MCME_OK) return rc;

    McmeModelIndex *index = NULL;
    if (!mcme_model_index_open(pack_dir, &index)) {
        char why[256];
        snprintf(why, sizeof why, "%s", mcme_last_error());
        mcme_set_error("model: %s", why);
        return MCME_EXIT_INTERNAL;
    }

    struct cJSON *doc = NULL;
    if (!mcme_model_load(index, ns, base, &doc)) {
        /* The fold refused: the chain is too deep to be anything
           but a cycle (an OOM would say so in the same message). */
        char why[256];
        snprintf(why, sizeof why, "%s", mcme_last_error());
        mcme_set_error("model: %s", why);
        mcme_model_index_close(index);
        return MCME_EXIT_BAD_FORMAT;
    }
    mcme_model_index_close(index);

    if (g_opts.json) {
        /* The folded document IS the answer, so it is printed
           bare: the same JSON the reference's own fold returns. */
        char *text = cJSON_PrintUnformatted(doc);
        printf("%s\n", text ? text : "{}");
        if (text) cJSON_free(text);
    } else {
        printf("%s:%s", ns, base);
        if (doc->child == NULL) {
            printf("  (nothing in this pack folds into this model)\n");
        } else {
            printf("\n");
            char *text = cJSON_Print(doc);
            printf("%s\n", text ? text : "");
            if (text) cJSON_free(text);
        }
    }

    cJSON_Delete(doc);
    return MCME_OK;
}

/* ========================================================================
 * mesh
 *
 * `mcme mesh <pack-dir> <model-ref>` — one model as the game would
 * build it: four vertices per face with texture coordinates and
 * tint, faces grouped by the direction they cull against, the
 * resolved texture list, and the block's transparency — the CLI
 * face of the D32 layer, the reference's `_load_block_model`
 * answer by answer. `--json` prints that answer in the shape the
 * pins compare: face_mode, textures, transparent, and the buckets
 * under their own names. A model the pack does not know is the
 * empty mesh, exit 0 — an answer. A model that will not mesh (the
 * reference raises on it too) is the pack's bad data, exit 3; a
 * pack directory that does not exist is exit 2, a malformed
 * reference exit 1.
 * ======================================================================== */

static const char *const k_dir_names[7] = {
    "down", "up", "north", "east", "south", "west", "none"
};

/* One mesh array as a JSON array of numbers. */
static void json_add_floats(struct cJSON *obj, const char *name,
                            const double *vals, size_t count)
{
    struct cJSON *arr = cJSON_AddArrayToObject(obj, name);
    if (!arr) return;
    for (size_t i = 0; i < count; ++i)
        cJSON_AddItemToArray(arr, cJSON_CreateNumber(vals[i]));
}

static void json_add_u32s(struct cJSON *obj, const char *name,
                          const uint32_t *vals, size_t count)
{
    struct cJSON *arr = cJSON_AddArrayToObject(obj, name);
    if (!arr) return;
    for (size_t i = 0; i < count; ++i)
        cJSON_AddItemToArray(arr, cJSON_CreateNumber(vals[i]));
}

/* The mesh document, the shape every mesh-printing verb shares
   (D32's `mcme mesh`, D33's `mcme blockmesh`). */
static struct cJSON *json_mesh(const McmeMesh *mesh)
{
    struct cJSON *doc = cJSON_CreateObject();
    if (!doc) return NULL;
    cJSON_AddNumberToObject(doc, "face_mode", 3);
    struct cJSON *tex = cJSON_AddArrayToObject(doc, "textures");
    for (size_t i = 0; i < mesh->texture_count; ++i)
        cJSON_AddItemToArray(tex, cJSON_CreateString(mesh->textures[i]));
    cJSON_AddNumberToObject(doc, "transparent", (double)mesh->transparency);
    struct cJSON *dirs = cJSON_AddObjectToObject(doc, "dirs");
    for (int i = 0; i < 7; ++i) {
        if (!mesh->present[i]) continue;
        const McmeMeshBucket *b = &mesh->buckets[i];
        struct cJSON *d = cJSON_AddObjectToObject(dirs, k_dir_names[i]);
        if (!d) continue;
        json_add_floats(d, "verts", b->verts, b->vert_count * 3);
        json_add_floats(d, "tverts", b->tverts, b->vert_count * 2);
        json_add_floats(d, "tint_verts", b->tint_verts,
                        b->vert_count * 3);
        json_add_u32s(d, "faces", b->faces, b->face_count * 6);
        json_add_u32s(d, "texture_indexes", b->texture_indexes,
                      b->face_count * 2);
    }
    return doc;
}

static bool print_mesh(const McmeMesh *mesh, const char *heading,
                       const McmeAtlas *atlas)
{
    if (g_opts.json) {
        struct cJSON *doc = json_mesh(mesh);
        if (!doc) return false; /* the caller reports the heap */
        char *text = cJSON_PrintUnformatted(doc);
        printf("%s\n", text ? text : "{}");
        if (text) cJSON_free(text);
        cJSON_Delete(doc);
        return true;
    }

    static const char *const trans_words[3] = {
        "opaque", "translucent", "partial" };
    printf("%s\n", heading);
    printf("  transparency  %s\n", trans_words[mesh->transparency]);
    printf("  textures      %zu\n", mesh->texture_count);
    for (size_t i = 0; i < mesh->texture_count; ++i)
        printf("    %s\n", mesh->textures[i]);
    if (atlas)
        printf("  atlas         %dx%d page\n", atlas->size,
               atlas->size);
    bool any = false;
    for (int i = 0; i < 7; ++i) {
        if (!mesh->present[i]) continue;
        any = true;
        printf("  %-6s        %zu faces, %zu verts\n",
               k_dir_names[i], mesh->buckets[i].face_count,
               mesh->buckets[i].vert_count);
    }
    if (!any) printf("  (this mesh has no faces)\n");
    return true;
}

static int cmd_mesh(const char *pack_dir, const char *ref)
{
    if (!mcme_fs_exists(pack_dir)) {
        mcme_set_error("mesh: '%s' is not a directory", pack_dir);
        return MCME_EXIT_NOT_FOUND;
    }

    char ns[256];
    const char *base;
    int rc = split_ref("mesh", ref, ns, sizeof ns, &base);
    if (rc != MCME_OK) return rc;

    McmeModelIndex *models = NULL;
    if (!mcme_model_index_open(pack_dir, &models)) {
        char why[256];
        snprintf(why, sizeof why, "%s", mcme_last_error());
        mcme_set_error("mesh: %s", why);
        return MCME_EXIT_INTERNAL;
    }
    McmeTextureIndex *tex_index = NULL;
    if (!mcme_texture_index_open(pack_dir, &tex_index)) {
        char why[256];
        snprintf(why, sizeof why, "%s", mcme_last_error());
        mcme_set_error("mesh: %s", why);
        mcme_model_index_close(models);
        return MCME_EXIT_INTERNAL;
    }

    McmeMesh mesh;
    if (!mcme_mesh_build(models, tex_index, ns, base, &mesh)) {
        /* The model would not mesh — its data, not mcme's: the
           reference raises on the same file. */
        char why[256];
        snprintf(why, sizeof why, "%s", mcme_last_error());
        mcme_set_error("mesh: %s", why);
        mcme_model_index_close(models);
        mcme_texture_index_close(tex_index);
        return MCME_EXIT_BAD_FORMAT;
    }
    mcme_model_index_close(models);
    mcme_texture_index_close(tex_index);

    char heading[512];
    snprintf(heading, sizeof heading, "%s:%s", ns, base);
    bool shown = print_mesh(&mesh, heading, NULL);
    mcme_mesh_free(&mesh);
    if (!shown) {
        mcme_set_error("mesh: out of memory");
        return MCME_EXIT_INTERNAL;
    }
    return MCME_OK;
}

/*
 * The block-level prelude every block verb shares: the spec parsed
 * into the game's own shape, the three indexes opened, the
 * selection walked and the mesh built. On success `*tex_index`
 * stays OPEN — the atlas layer reads the pack's texture list from
 * it — and everything else is closed. The answer is total: false
 * happens only where the heap refuses, never the pack.
 */
static int build_block_mesh(const char *verb, const char *pack_dir,
                            const char *spec, McmeMesh *mesh,
                            McmeTextureIndex **tex_index)
{
    if (!mcme_fs_exists(pack_dir)) {
        mcme_set_error("%s: '%s' is not a directory", verb, pack_dir);
        return MCME_EXIT_NOT_FOUND;
    }

    McmeArena *arena = mcme_arena_new();
    if (!arena) {
        mcme_set_error("%s: out of memory", verb);
        return MCME_EXIT_INTERNAL;
    }

    char name[256];
    const McmeTag *states = NULL;
    int rc = parse_block_spec(verb, spec, arena, name,
                              sizeof name, &states);
    if (rc != MCME_OK) {
        mcme_arena_free(arena);
        return rc;
    }

    /* Split the name into namespace and block, the way
       cmd_blockstate does. */
    char ns[256];
    const char *colon = strchr(name, ':');
    const char *base;
    if (colon) {
        size_t ns_len = (size_t)(colon - name);
        if (ns_len == 0 || ns_len >= sizeof ns) {
            mcme_set_error("%s: block '%s' has an empty or "
                           "too-long namespace", verb, spec);
            mcme_arena_free(arena);
            return MCME_EXIT_USAGE;
        }
        memcpy(ns, name, ns_len);
        ns[ns_len] = '\0';
        base = colon + 1;
    } else {
        snprintf(ns, sizeof ns, "minecraft");
        base = name;
    }
    if (*base == '\0') {
        mcme_set_error("%s: block '%s' has an empty name", verb,
                       spec);
        mcme_arena_free(arena);
        return MCME_EXIT_USAGE;
    }

    /* The block's states, as the lookup wants them — a typed
       state carried with a NULL value, exactly as
       cmd_blockstate carries it: present, and matching nothing
       (D43). */
    McmeState props[64];
    size_t prop_count = 0;
    if (states) {
        for (size_t i = 0; i < states->compound.len; ++i) {
            const McmeCompoundEntry *e = &states->compound.entries[i];
            if (prop_count >= sizeof props / sizeof props[0]) {
                mcme_set_error("%s: block '%s' has more than "
                               "%zu states", verb, spec,
                               sizeof props / sizeof props[0]);
                mcme_arena_free(arena);
                return MCME_EXIT_USAGE;
            }
            props[prop_count].name  = e->name.data;
            props[prop_count].value = e->value->type == MCME_TAG_String
                ? e->value->string.data : NULL;
            prop_count++;
        }
    }

    McmeBlockstateIndex *blockstates = NULL;
    if (!mcme_blockstate_index_open(pack_dir, &blockstates)) {
        char why[256];
        snprintf(why, sizeof why, "%s", mcme_last_error());
        mcme_set_error("%s: %s", verb, why);
        mcme_arena_free(arena);
        return MCME_EXIT_INTERNAL;
    }
    McmeModelIndex *models = NULL;
    if (!mcme_model_index_open(pack_dir, &models)) {
        char why[256];
        snprintf(why, sizeof why, "%s", mcme_last_error());
        mcme_set_error("%s: %s", verb, why);
        mcme_blockstate_index_close(blockstates);
        mcme_arena_free(arena);
        return MCME_EXIT_INTERNAL;
    }
    McmeTextureIndex *tex = NULL;
    if (!mcme_texture_index_open(pack_dir, &tex)) {
        char why[256];
        snprintf(why, sizeof why, "%s", mcme_last_error());
        mcme_set_error("%s: %s", verb, why);
        mcme_model_index_close(models);
        mcme_blockstate_index_close(blockstates);
        mcme_arena_free(arena);
        return MCME_EXIT_INTERNAL;
    }

    bool ok = mcme_mesh_for_block(blockstates, models, tex, ns, base,
                                  props, prop_count, mesh);
    mcme_model_index_close(models);
    mcme_blockstate_index_close(blockstates);
    mcme_arena_free(arena);
    if (!ok) {
        /* The heap refused, never the pack: the block-level answer
           is total. */
        char why[256];
        snprintf(why, sizeof why, "%s", mcme_last_error());
        mcme_set_error("%s: %s", verb, why);
        mcme_texture_index_close(tex);
        return MCME_EXIT_INTERNAL;
    }
    *tex_index = tex;
    return MCME_OK;
}

/*
 * blockmesh — one block state as the game would mesh it: the
 * blockstate selection walked, the winning model(s) meshed and
 * turned by x/y (uvlock read and ignored, the reference's own
 * no-op), multipart's cases merged. The block-level answer is
 * total — a model that refuses to mesh is skipped the way the
 * reference catches it, and the missing block stands in when
 * nothing survives. Human and --json (the mesh document).
 */
static int cmd_blockmesh(const char *pack_dir, const char *spec)
{
    McmeMesh mesh;
    McmeTextureIndex *tex = NULL;
    int rc = build_block_mesh("blockmesh", pack_dir, spec, &mesh,
                              &tex);
    if (rc != MCME_OK) return rc;
    mcme_texture_index_close(tex);

    bool shown = print_mesh(&mesh, spec, NULL);
    mcme_mesh_free(&mesh);
    if (!shown) {
        mcme_set_error("blockmesh: out of memory");
        return MCME_EXIT_INTERNAL;
    }
    return MCME_OK;
}

/* ========================================================================
 * atlas
 * ======================================================================== */

/* The packer's input order, sorted: the layout only rides on the
   input order wherever the perimeter sort leaves ties, and a pack
   of 16x16 textures is one long tie — the filesystem's directory
   order must not decide the answer (D34). */
static int cmp_path_ptr(const void *a, const void *b)
{
    return strcmp(*(const char *const *)a, *(const char *const *)b);
}

/*
 * atlas — every texture the pack ships on one square page, and
 * where each one landed: the editor's own packer, quirks
 * included (D34, decisions.md#D34). Human: a summary line;
 * --json: {size, textures: {path: [u0, v0, u1, v1]}}. `--out`
 * writes the page PNG beside the answer — the renderer's image.
 */
static int cmd_atlas(const char *pack_dir, const char *out_png)
{
    if (!mcme_fs_exists(pack_dir)) {
        mcme_set_error("atlas: '%s' is not a directory", pack_dir);
        return MCME_EXIT_NOT_FOUND;
    }

    McmeTextureIndex *tex = NULL;
    if (!mcme_texture_index_open(pack_dir, &tex)) {
        char why[256];
        snprintf(why, sizeof why, "%s", mcme_last_error());
        mcme_set_error("atlas: %s", why);
        return MCME_EXIT_INTERNAL;
    }

    size_t count = mcme_texture_count(tex);
    const char **paths = count ? malloc(count * sizeof *paths) : NULL;
    if (count && !paths) {
        mcme_texture_index_close(tex);
        mcme_set_error("atlas: out of memory");
        return MCME_EXIT_INTERNAL;
    }
    for (size_t i = 0; i < count; ++i)
        paths[i] = mcme_texture_at(tex, i);
    if (count > 1) qsort(paths, count, sizeof *paths, cmp_path_ptr);

    McmeAtlas atlas;
    bool ok = mcme_atlas_build(paths, count, &atlas);
    free(paths);
    mcme_texture_index_close(tex);
    if (!ok) {
        /* One image would not load — the reference's Pillow raise
           kills the whole call; mcme refuses by name. */
        char why[256];
        snprintf(why, sizeof why, "%s", mcme_last_error());
        mcme_set_error("atlas: %s", why);
        return MCME_EXIT_INTERNAL;
    }

    if (out_png && !mcme_image_write_png(out_png, &atlas.page)) {
        char why[256];
        snprintf(why, sizeof why, "%s", mcme_last_error());
        mcme_set_error("atlas: %s", why);
        mcme_atlas_free(&atlas);
        return MCME_EXIT_INTERNAL;
    }

    bool shown;
    if (g_opts.json) {
        struct cJSON *doc = cJSON_CreateObject();
        if (doc) {
            cJSON_AddNumberToObject(doc, "size", atlas.size);
            struct cJSON *list = cJSON_AddObjectToObject(doc, "textures");
            for (size_t i = 0; i < atlas.count && list; ++i)
                json_add_floats(list, atlas.names[i],
                                &atlas.bounds[i * 4], 4);
            char *text = cJSON_PrintUnformatted(doc);
            printf("%s\n", text ? text : "{}");
            if (text) cJSON_free(text);
            cJSON_Delete(doc);
        }
        shown = doc != NULL;
    } else {
        printf("%zu textures on a %dx%d page\n",
               atlas.count, atlas.size, atlas.size);
        if (out_png) printf("  page written to %s\n", out_png);
        shown = true;
    }
    mcme_atlas_free(&atlas);
    if (!shown) {
        mcme_set_error("atlas: out of memory");
        return MCME_EXIT_INTERNAL;
    }
    return MCME_OK;
}

/*
 * atlasmesh — one block state, its texture coordinates in atlas
 * space: the block-level mesh, with every row of every bucket
 * carrying the rectangle its triangle draws from, the mesh's
 * own coordinates mapped into it, and the tint the builder's
 * two folds leave — the editor's chunk builder's table and its
 * stages' arithmetic, quirks included (D35, D36,
 * decisions.md#D35). The page is the pack's own (missing image
 * and all, the same build `mcme atlas` answers). --json adds the
 * page's `size` and, per direction, `bounds`, `auvs` and `tints`
 * beside the mesh document; human adds the page line. --y is the
 * block's world height for the band shade, an integer of any
 * sign, default 0: the builder adds the chunk offset to the
 * vertex before shading, and the block-level verb takes the one
 * number the fold reads. --beside is the block state standing on
 * all six sides, through the same prelude — its transparency is
 * what the cull clauses read (D37); default nothing, the air
 * answer that fires neither clause, and a spec the pack does not
 * have is the missing BLOCK, opaque, culling.
 */
static int cmd_atlasmesh(const char *pack_dir, const char *spec,
                         const char *y_arg, const char *beside_arg)
{
    /* The block's world height for the fold: any integer, the
       wave's own input. */
    long block_y = 0;
    if (y_arg) {
        char *end = NULL;
        errno = 0;
        block_y = strtol(y_arg, &end, 10);
        if (errno != 0 || end == y_arg || *end != '\0') {
            mcme_set_error("atlasmesh: usage: --y wants an integer, "
                           "got '%s'", y_arg);
            return MCME_EXIT_USAGE;
        }
    }

    McmeMesh mesh;
    McmeTextureIndex *tex = NULL;
    int rc = build_block_mesh("atlasmesh", pack_dir, spec, &mesh,
                              &tex);
    if (rc != MCME_OK) return rc;

    /* What stands beside the block on all six sides: its own mesh,
       through the same prelude, the transparency its classification
       answers. Nothing stands beside by default — the air answer,
       which fires neither cull clause. */
    McmeTransparency beside = MCME_TRANSPARENCY_PARTIAL;
    if (beside_arg) {
        McmeMesh bm;
        McmeTextureIndex *btex = NULL;
        int brc = build_block_mesh("atlasmesh", pack_dir, beside_arg,
                                   &bm, &btex);
        if (brc != MCME_OK) {
            mcme_mesh_free(&mesh);
            return brc;
        }
        beside = bm.transparency;
        mcme_mesh_free(&bm);
        mcme_texture_index_close(btex);
    }

    /* The page the coordinates are in — the pack's textures plus
       the missing image, exactly what `mcme atlas` packs. */
    size_t count = mcme_texture_count(tex);
    const char **paths = count ? malloc(count * sizeof *paths) : NULL;
    if (count && !paths) {
        mcme_texture_index_close(tex);
        mcme_mesh_free(&mesh);
        mcme_set_error("atlasmesh: out of memory");
        return MCME_EXIT_INTERNAL;
    }
    for (size_t i = 0; i < count; ++i)
        paths[i] = mcme_texture_at(tex, i);
    if (count > 1) qsort(paths, count, sizeof *paths, cmp_path_ptr);

    McmeAtlas atlas;
    bool ok = mcme_atlas_build(paths, count, &atlas);
    free(paths);
    mcme_texture_index_close(tex);
    if (!ok) {
        /* One image would not load — the reference's Pillow raise
           kills the whole call; mcme refuses by name (D34). */
        char why[256];
        snprintf(why, sizeof why, "%s", mcme_last_error());
        mcme_set_error("atlasmesh: %s", why);
        mcme_mesh_free(&mesh);
        return MCME_EXIT_INTERNAL;
    }

    McmeAtlasMesh table;
    if (!mcme_atlas_mesh(&atlas, &mesh, block_y, beside, &table)) {
        char why[256];
        snprintf(why, sizeof why, "%s", mcme_last_error());
        mcme_set_error("atlasmesh: %s", why);
        mcme_atlas_free(&atlas);
        mcme_mesh_free(&mesh);
        return MCME_EXIT_INTERNAL;
    }

    bool shown;
    if (g_opts.json) {
        struct cJSON *doc = json_mesh(&mesh);
        if (doc) {
            cJSON_AddNumberToObject(doc, "size", atlas.size);
            struct cJSON *dirs = cJSON_GetObjectItem(doc, "dirs");
            for (int i = 0; i < 7; ++i) {
                if (!table.present[i]) continue;
                struct cJSON *d =
                    cJSON_GetObjectItem(dirs, k_dir_names[i]);
                if (!d) continue;
                json_add_floats(d, "bounds", table.buckets[i].bounds,
                                table.buckets[i].row_count * 4);
                json_add_floats(d, "auvs", table.buckets[i].auvs,
                                table.buckets[i].row_count * 2);
                json_add_floats(d, "tints", table.buckets[i].tints,
                                table.buckets[i].row_count * 3);
            }
            char *text = cJSON_PrintUnformatted(doc);
            printf("%s\n", text ? text : "{}");
            if (text) cJSON_free(text);
            cJSON_Delete(doc);
        }
        shown = doc != NULL;
    } else {
        shown = print_mesh(&mesh, spec, &atlas);
    }
    mcme_atlas_mesh_free(&table);
    mcme_atlas_free(&atlas);
    mcme_mesh_free(&mesh);
    if (!shown) {
        mcme_set_error("atlasmesh: out of memory");
        return MCME_EXIT_INTERNAL;
    }
    return MCME_OK;
}

/* ========================================================================
 * info
 *
 * Summarise a world directory. This is the first command that exercises the
 * whole Phase 3 stack at once: directory detection, gzip, NBT, and the region
 * container. It is deliberately read-only and never opens a database.
 * ======================================================================== */

static void print_info_text(const McmeWorldInfo *w,
                            const McmeBedrockInventory *inv,
                            const McmeJavaInventory *jinv)
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
        /* The inventory's totals (G22): the same numbers the probe's
           overworld leg counted, plus every dimension tree the world
           holds — for an overworld-only world, the same two lines. */
        printf("regions:  %zu\n", jinv->region_files);
        printf("chunks:   %zu\n", jinv->chunks);
    } else if (inv != NULL && inv->opened) {
        printf("keys:     %zu\n", inv->keys);
        printf("chunks:   %zu\n", inv->chunks);
    } else {
        /* The store is there but could not be read. Reporting a chunk count of
           zero would say the world is empty, which is the one thing we do not
           know; say what failed instead. */
        printf("database: present, unreadable: %s\n", mcme_last_error());
    }
}

/* Prints a finished JSON tree, one line, and frees it. Shared by every
   --json emitter so the "one object, one newline" contract is written once. */
static void print_json(cJSON *root)
{
    if (root == NULL) return;
    char *text = cJSON_PrintUnformatted(root);
    if (text != NULL) {
        printf("%s\n", text);
        free(text);
    }
    cJSON_Delete(root);
}

static void print_info_json(const McmeWorldInfo *w,
                            const McmeBedrockInventory *inv,
                            const McmeJavaInventory *jinv)
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
            cJSON_AddNumberToObject(root, "region_files",
                                    (double)jinv->region_files);
            cJSON_AddNumberToObject(root, "chunks", (double)jinv->chunks);
            /* G22: java answers the breakdown question the bedrock json
               already answered — one key, one shape, both formats. */
            cJSON *dims = cJSON_AddArrayToObject(root, "chunks_by_dimension");
            for (size_t i = 0; i < jinv->dims; ++i) {
                cJSON *row = cJSON_CreateObject();
                if (row == NULL) break;
                cJSON_AddNumberToObject(row, "dimension",
                                        (double)jinv->by_dim[i].dimension);
                cJSON_AddNumberToObject(row, "chunks",
                                        (double)jinv->by_dim[i].chunks);
                cJSON_AddItemToArray(dims, row);
            }
        } else {
            cJSON_AddBoolToObject(root, "has_db", w->has_db);
            if (inv != NULL && inv->opened) {
                cJSON_AddNumberToObject(root, "keys", (double)inv->keys);
                cJSON_AddNumberToObject(root, "chunks", (double)inv->chunks);
                cJSON *dims = cJSON_AddArrayToObject(root, "chunks_by_dimension");
                for (size_t i = 0; i < inv->dims; ++i) {
                    cJSON *row = cJSON_CreateObject();
                    if (row == NULL) break;
                    cJSON_AddNumberToObject(row, "dimension",
                                            (double)inv->by_dim[i].dimension);
                    cJSON_AddNumberToObject(row, "chunks",
                                            (double)inv->by_dim[i].chunks);
                    cJSON_AddItemToArray(dims, row);
                }
            } else {
                cJSON_AddStringToObject(root, "db_error", mcme_last_error());
            }
        }
    }

    print_json(root);
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

    /* G22: the java count is the inventory's — every dimension tree,
       through the same scanner the probe walked for the world root.
       Java counting never fails the way opening a store can: a world
       without a nether simply holds no DIM-1 rows. */
    McmeJavaInventory jinv;
    memset(&jinv, 0, sizeof jinv);
    if (info.kind == MCME_WORLD_JAVA)
        mcme_java_inventory(&jinv, info.path);

    /* Only opened for a store that is actually there. Probing is cheap and
       opening is not, and a Java world has no store to open. */
    McmeBedrockInventory inv;
    memset(&inv, 0, sizeof inv);
    if (info.kind == MCME_WORLD_BEDROCK && info.db_path != NULL) {
        if (!mcme_bedrock_inventory(&inv, info.db_path)) {
            /* Keep the exit code at "found": the world was identified, only
               its contents could not be read, and a script that wants to
               distinguish those can ask for --json and read db_error. */
            MCME_WARN("info: cannot read %s: %s", info.db_path, mcme_last_error());
            mcme_clear_error();
        }
    }

    if (g_opts.json) print_info_json(&info, &inv, &jinv);
    else print_info_text(&info, &inv, &jinv);

    mcme_bedrock_inventory_free(&inv);
    mcme_java_inventory_free(&jinv);
    mcme_world_info_free(&info);
    return rc;
}

/* ========================================================================
 * chunks
 * ======================================================================== */

/*
 * `mcme chunks <world>` — what chunks a world contains.
 *
 * `--count` prints one number and nothing else, so a script can do
 * `mcme chunks w --count | xargs ...` without parsing prose. Everything else
 * goes to stdout as text, or as JSON under --json.
 */
static int cmd_chunks(int argc, char **argv)
{
    const char *path = NULL;
    bool count_only = false;

    for (int i = 0; i < argc; ++i) {
        if (strcmp(argv[i], "--count") == 0) {
            count_only = true;
            continue;
        }
        if (argv[i][0] == '-') {
            mcme_set_error("chunks: unknown option '%s'", argv[i]);
            return MCME_EXIT_USAGE;
        }
        if (path != NULL) {
            mcme_set_error("chunks: unexpected extra argument '%s'", argv[i]);
            return MCME_EXIT_USAGE;
        }
        path = argv[i];
    }

    if (path == NULL) {
        mcme_set_error("chunks: missing <world>");
        return MCME_EXIT_USAGE;
    }

    McmeWorldInfo info;
    if (!mcme_world_probe(&info, path)) {
        return mcme_fs_exists(path) ? MCME_EXIT_IO : MCME_EXIT_NOT_FOUND;
    }

    size_t chunks = 0;

    if (info.kind == MCME_WORLD_JAVA) {
        /* G22: the java count comes from the inventory — every
           dimension tree, one scanner with the probe's leg — and the
           breakdown prints the way the bedrock branch prints it. */
        McmeJavaInventory jinv;

        mcme_java_inventory(&jinv, info.path);
        chunks = jinv.chunks;
        if (g_opts.json) {
            cJSON *root = cJSON_CreateObject();
            if (root != NULL) {
                cJSON_AddStringToObject(root, "path", info.path);
                cJSON_AddStringToObject(root, "format", "java");
                cJSON_AddNumberToObject(root, "region_files",
                                        (double)jinv.region_files);
                cJSON_AddNumberToObject(root, "chunks", (double)chunks);
                cJSON *dims =
                    cJSON_AddArrayToObject(root, "chunks_by_dimension");
                for (size_t i = 0; i < jinv.dims; ++i) {
                    cJSON *row = cJSON_CreateObject();
                    if (row == NULL) break;
                    cJSON_AddNumberToObject(row, "dimension",
                                            (double)jinv.by_dim[i].dimension);
                    cJSON_AddNumberToObject(row, "chunks",
                                            (double)jinv.by_dim[i].chunks);
                    cJSON_AddItemToArray(dims, row);
                }
                print_json(root);
            }
        } else if (count_only) {
            /* The count face of the road: one bare number, the way the
               bedrock leg prints it (cli.md promises --count prints a
               number and nothing else, and every leg owes it). */
            printf("%zu\n", chunks);
        } else {
            printf("format:   java (anvil)\n");
            printf("regions:  %zu\n", jinv.region_files);
            printf("chunks:   %zu\n", chunks);
            if (jinv.dims > 0) {
                printf("by dimension:\n");
                for (size_t i = 0; i < jinv.dims; ++i) {
                    /* The inventory spells the names the vocabulary
                       spells (G20): one place decides them, and a
                       number with no name prints as the number. */
                    const char *name =
                        mcme_dimension_name(jinv.by_dim[i].dimension);
                    if (name != NULL) {
                        printf("  %-10s %zu\n", name,
                               jinv.by_dim[i].chunks);
                    } else {
                        printf("  %-10d %zu\n", jinv.by_dim[i].dimension,
                               jinv.by_dim[i].chunks);
                    }
                }
            }
        }
        mcme_java_inventory_free(&jinv);
    } else if (info.kind == MCME_WORLD_BEDROCK && info.db_path != NULL) {
        McmeBedrockInventory inv;
        if (!mcme_bedrock_inventory(&inv, info.db_path)) {
            mcme_set_error("chunks: cannot read %s: %s", info.db_path,
                           mcme_last_error());
            mcme_bedrock_inventory_free(&inv);
            mcme_world_info_free(&info);
            return MCME_EXIT_IO;
        }

        chunks = inv.chunks;
        if (g_opts.json) {
            cJSON *root = cJSON_CreateObject();
            if (root != NULL) {
                cJSON_AddStringToObject(root, "path", info.path);
                cJSON_AddStringToObject(root, "format", "bedrock");
                cJSON_AddNumberToObject(root, "keys", (double)inv.keys);
                cJSON_AddNumberToObject(root, "chunks", (double)chunks);
                cJSON *dims = cJSON_AddArrayToObject(root, "chunks_by_dimension");
                for (size_t i = 0; i < inv.dims; ++i) {
                    cJSON *row = cJSON_CreateObject();
                    if (row == NULL) break;
                    cJSON_AddNumberToObject(row, "dimension",
                                            (double)inv.by_dim[i].dimension);
                    cJSON_AddNumberToObject(row, "chunks",
                                            (double)inv.by_dim[i].chunks);
                    cJSON_AddItemToArray(dims, row);
                }
                print_json(root);
            }
        } else if (count_only) {
            printf("%zu\n", chunks);
        } else {
            printf("format:   bedrock (leveldb)\n");
            printf("keys:     %zu\n", inv.keys);
            printf("chunks:   %zu\n", chunks);
            printf("by dimension:\n");
            for (size_t i = 0; i < inv.dims; ++i) {
                /* The inventory spells the names the vocabulary
                   spells (G20): one place decides them, and a
                   number with no name prints as the number. */
                const char *name =
                    mcme_dimension_name(inv.by_dim[i].dimension);
                if (name != NULL) {
                    printf("  %-10s %zu\n", name, inv.by_dim[i].chunks);
                } else {
                    printf("  %-10d %zu\n", inv.by_dim[i].dimension,
                           inv.by_dim[i].chunks);
                }
            }
        }
        mcme_bedrock_inventory_free(&inv);
    } else {
        mcme_set_error("chunks: '%s' is not a world mcme recognises", path);
        mcme_world_info_free(&info);
        return MCME_EXIT_NOT_FOUND;
    }

    mcme_world_info_free(&info);
    return MCME_OK;
}


/* ========================================================================
 * chunk set — the first editing verb
 * ======================================================================== */

/*
 * `mcme chunk set <world> <x> <y> <z> <block>` sets one block in a
 * Bedrock world, per D3: anything worth doing is a verb.
 *
 * The block is a namespaced name, optionally with SNBT states in
 * braces — `minecraft:stone` or `minecraft:oak_log{pillar_axis:"x"}`.
 *
 * A subchunk that is not stored is exit 2, not exit 4: the edit
 * refuses to create chunks, so the CLI checks first and reports
 * "not found" for what the caller asked to edit. Everything else is
 * exit 4 with mcme_last_error() on stderr. Success prints nothing,
 * like `db put`.
 *
 * --dimension names the dimension to edit (overworld, nether, end,
 * or a raw number); without it, the overworld. The key omits a
 * zero dimension the way the game does, so naming the overworld
 * changes nothing on disk.
 */

/* Splits a block spec into name and states. States, when present,
   must parse as one SNBT compound; the tag is arena-rooted and
   borrowed for the call that uses it. `verb` names the command
   that asked — the parser is shared, and every message it sets
   names the command that asked, like parse_coord. */
static int parse_block_spec(const char *verb, const char *spec,
                            McmeArena *arena,
                            char *name, size_t name_cap,
                            const McmeTag **states)
{
    char why[512];

    if (mcme_block_parse_spec(spec, arena, name, name_cap, states))
        return MCME_OK;
    /* The core's own message under the family's own name — the
       texts these verbs have always printed (G9's move). */
    snprintf(why, sizeof why, "%s", mcme_last_error());
    mcme_set_error("%s: %s", verb, why);
    return MCME_EXIT_USAGE;
}

/* A coordinate as a whole block position. strtol, not atoi: the
   text is untrusted and "12x" is not 12. `verb` is the family the
   error belongs to — the parser is shared, and every message it
   sets names the command that asked. */
static bool parse_coord(const char *verb, const char *text, long *out)
{
    char *end = NULL;
    long v = strtol(text, &end, 10);
    if (end == text || *end != '\0') {
        mcme_set_error("%s: '%s' is not a whole number", verb, text);
        return false;
    }
    if (v < INT32_MIN || v > INT32_MAX) {
        mcme_set_error("%s: '%s' is outside the coordinate "
                       "range", verb, text);
        return false;
    }
    *out = v;
    return true;
}

/* Pulls a `--dimension <name|number>` out of the argument vector.
   Names exist because numbers are for machines; 0 is the
   overworld, and the key builder omits a zero dimension the way
   the game does. Absent, the dimension is the overworld. `verb`
   names the command the message belongs to. */
static bool extract_dimension(const char *verb, int *argc, char **argv,
                              int32_t *dim)
{
    for (int i = 0; i < *argc; ++i) {
        if (strcmp(argv[i], "--dimension") != 0) continue;
        if (i + 1 >= *argc) {
            mcme_set_error("%s: --dimension needs a value "
                           "(overworld, nether, end, or a "
                           "number)", verb);
            return false;
        }
        const char *v = argv[i + 1];
        int32_t parsed;

        /* The vocabulary lives in core now (G20): the core's own
           words under the family's own name — the message these
           verbs have always printed, byte for byte. */
        if (!mcme_dimension_parse(v, &parsed)) {
            char why[512];

            snprintf(why, sizeof why, "%s", mcme_last_error());
            mcme_set_error("%s: %s", verb, why);
            return false;
        }
        *dim = parsed;
        for (int j = i; j + 2 < *argc; ++j) argv[j] = argv[j + 2];
        *argc -= 2;
        return true;
    }
    return true;
}

/* A Y the key cannot name is a position the world cannot hold:
   refuse as a coordinate problem, not a missing one. */
static bool box_y_range_fits(const long *c)
{
    long ay1 = c[1] < c[4] ? c[1] : c[4];
    long ay2 = c[1] < c[4] ? c[4] : c[1];
    long cy1 = ay1 / 16, cy2 = ay2 / 16;
    if (ay1 % 16 != 0 && ay1 < 0) cy1 -= 1;
    if (ay2 % 16 != 0 && ay2 < 0) cy2 -= 1;
    if (cy1 < INT8_MIN || cy2 > INT8_MAX) {
        mcme_set_error("chunk: the box spans subchunk Y %ld to %ld, "
                       "beyond the key's signed byte", cy1, cy2);
        return false;
    }
    return true;
}

/*
 * The exit code a write verb reports for a refusal the library
 * has already classified: the subchunk the game removed is not
 * there any more, world data mcme cannot read is a format
 * problem, and so on. `chunk set` and `chunk fill` share it so
 * one condition is one code whichever verb met it.
 */
static int refusal_exit(McmeWriteRefusal refusal)
{
    switch (refusal) {
    case MCME_WRITE_REFUSAL_NOT_THERE:    return MCME_EXIT_NOT_FOUND;
    case MCME_WRITE_REFUSAL_NOT_READABLE: return MCME_EXIT_BAD_FORMAT;
    case MCME_WRITE_REFUSAL_STORE:        return MCME_EXIT_IO;
    case MCME_WRITE_REFUSAL_USAGE:        return MCME_EXIT_USAGE;
    case MCME_WRITE_REFUSAL_NONE:         /* false without a
                                             reason is a bug */
    case MCME_WRITE_REFUSAL_INTERNAL:     return MCME_EXIT_INTERNAL;
    }
    return MCME_EXIT_INTERNAL;
}

/* ========================================================================
 * region — what one Java region file holds
 * ======================================================================== */

/* The file's compression byte as the inventory prints it: the four
   values the format defines, and the raw number for anything else —
   an inventory answers what the file says, not what mcme can open.
   The caller keeps the scratch buffer only for the unknown case. */
static const char *compression_name(uint8_t code, char *scratch, size_t cap)
{
    switch (code) {
    case MCME_ANVIL_COMP_GZIP:     return "gzip";
    case MCME_ANVIL_COMP_ZLIB:     return "zlib";
    case MCME_ANVIL_COMP_STORED:   return "stored";
    case MCME_ANVIL_COMP_EXTERNAL: return "external";
    default:
        snprintf(scratch, cap, "%u", (unsigned)code);
        return scratch;
    }
}

/* A timestamp the file stores as seconds since the epoch, printed the
   way an agent can read without a conversion: UTC, one form. */
static void format_timestamp(uint32_t seconds, char *out, size_t cap)
{
    time_t t = (time_t)seconds;
    struct tm utc;
    if (gmtime_r(&t, &utc) == NULL
        || strftime(out, cap, "%Y-%m-%dT%H:%M:%SZ", &utc) == 0) {
        snprintf(out, cap, "%u", (unsigned)seconds);
    }
}

/*
 * `mcme region <world> <rx> <rz>` inventories one Java region file
 * from the container's own tables: which slots are filled, each
 * payload's length and compression byte, each timestamp. No chunk's
 * NBT is decompressed or parsed, so this answers what the file
 * holds rather than what the chunks contain.
 *
 * Exit codes: 2 for a world or region file that is not there, 3 for
 * a file that is there but is not a region file mcme reads — the
 * header will not validate, or a present slot's payload header
 * will not read — 4 for store errors, 1 for usage. An empty region
 * is exit 0: "0 of 1024 chunks" is an answer, not a fault.
 */
static int cmd_region(int argc, char **argv)
{
    int32_t dimension = 0;
    if (!extract_dimension("region", &argc, argv, &dimension)) {
        return MCME_EXIT_USAGE;
    }
    if (argc != 3) {
        mcme_set_error("region: usage: region <world> <rx> <rz> "
                       "[--dimension <name|number>]");
        return MCME_EXIT_USAGE;
    }
    const char *path = argv[0];
    long rx, rz;
    if (!parse_coord("region", argv[1], &rx)) return MCME_EXIT_USAGE;
    if (!parse_coord("region", argv[2], &rz)) return MCME_EXIT_USAGE;

    /* Java stores its three dimensions in directories, not in the
       key, and has no third spelling mcme could guess at. */
    const char *dim_dir = NULL;
    if (!mcme_java_dimension_dir(dimension, &dim_dir)) {
        return MCME_EXIT_USAGE;
    }

    McmeWorldInfo info;
    if (!mcme_world_probe(&info, path)) {
        return mcme_fs_exists(path) ? MCME_EXIT_IO : MCME_EXIT_NOT_FOUND;
    }
    if (info.kind != MCME_WORLD_JAVA) {
        mcme_set_error("region: '%s' is not a Java world", path);
        mcme_world_info_free(&info);
        return MCME_EXIT_NOT_FOUND;
    }

    char file[MCME_REGION_PATH_MAX];
    if (mcme_region_path(file, sizeof file, info.path, dim_dir,
                         (int)rx, (int)rz) == NULL) {
        mcme_set_error("region: the path to r.%ld.%ld is too long",
                       rx, rz);
        mcme_world_info_free(&info);
        return MCME_EXIT_INTERNAL;
    }

    /* The world is there and it is Java; what may be missing is this
       region's file, which is an absence the caller asked about. A
       file that is present but will not open is a region file mcme
       cannot read. */
    McmeRegion reg;
    if (!mcme_region_open(&reg, file)) {
        int rc = mcme_fs_exists(file) ? MCME_EXIT_BAD_FORMAT
                                      : MCME_EXIT_NOT_FOUND;
        mcme_world_info_free(&info);
        return rc;
    }

    int slots[MCME_REGION_ENTRIES * 2];
    size_t count = mcme_region_slots(&reg, slots, MCME_REGION_ENTRIES);

    /* The header line (text) and the envelope (JSON) both come before
       the rows so a failure mid-walk prints nothing partial: the
       region is the unit, and half a listing is not an answer. */
    cJSON *root = NULL;
    cJSON *entries = NULL;
    if (g_opts.json) {
        root = cJSON_CreateObject();
        if (root != NULL) {
            cJSON_AddStringToObject(root, "path", file);
            cJSON_AddNumberToObject(root, "rx", (double)rx);
            cJSON_AddNumberToObject(root, "rz", (double)rz);
            cJSON_AddNumberToObject(root, "chunks", (double)count);
            cJSON_AddNumberToObject(root, "bytes", (double)reg.size);
            entries = cJSON_AddArrayToObject(root, "entries");
        }
        if (root == NULL || entries == NULL) {
            cJSON_Delete(root);
            mcme_region_close(&reg);
            mcme_world_info_free(&info);
            mcme_set_error("region: out of memory");
            return MCME_EXIT_INTERNAL;
        }
    } else {
        printf("%s: %zu of %u chunks, %zu bytes\n",
               file, count, MCME_REGION_ENTRIES, reg.size);
    }

    int rc = MCME_OK;
    for (size_t i = 0; i < count; ++i) {
        int cx = slots[i * 2 + 0];
        int cz = slots[i * 2 + 1];

        McmeChunkPayload p;
        if (!mcme_region_chunk_payload(&reg, cx, cz, &p)) {
            /* The library's message already names the verb and the
               slot; mcme_last_error() is left as it set it. */
            rc = MCME_EXIT_BAD_FORMAT;
            break;
        }

        char comp[16];
        const char *name = compression_name(p.compression, comp, sizeof comp);
        uint32_t stamp = mcme_region_chunk_timestamp(&reg, cx, cz);
        long wx = rx * 32 + cx;
        long wz = rz * 32 + cz;

        if (g_opts.json) {
            cJSON *row = cJSON_CreateObject();
            if (row == NULL) {
                rc = MCME_EXIT_INTERNAL;
                break;
            }
            cJSON_AddNumberToObject(row, "x", (double)wx);
            cJSON_AddNumberToObject(row, "z", (double)wz);
            cJSON_AddStringToObject(row, "compression", name);
            cJSON_AddNumberToObject(row, "bytes", (double)p.length);
            cJSON_AddNumberToObject(row, "timestamp", (double)stamp);
            cJSON_AddItemToArray(entries, row);
        } else {
            char when[32];
            char coord[32];
            format_timestamp(stamp, when, sizeof when);
            snprintf(coord, sizeof coord, "(%ld, %ld)", wx, wz);
            printf("  chunk %-11s %-6s %7u bytes  %s\n",
                   coord, name, p.length, when);
        }
    }

    if (g_opts.json) {
        if (rc == MCME_OK) {
            print_json(root);
        } else {
            cJSON_Delete(root);
        }
    }
    mcme_region_close(&reg);
    mcme_world_info_free(&info);
    return rc;
}

/*
 * One present chunk, printed: inflate, decode, write the tree out the
 * way `nbt dump` writes a file. The chunk is the unit — a payload,
 * compression or document that will not read refuses the whole answer
 * rather than printing what survived.
 */
static int read_one_chunk(const McmeRegion *reg, long cx, long cz)
{
    McmeBuffer payload;
    mcme_buffer_init(&payload);
    if (!mcme_region_read_chunk(reg, (int)cx, (int)cz, NULL, &payload)) {
        mcme_buffer_free(&payload);
        return MCME_EXIT_BAD_FORMAT;   /* the library names the slot */
    }

    McmeArena *arena = mcme_arena_new();
    if (arena == NULL) {
        mcme_buffer_free(&payload);
        return MCME_EXIT_INTERNAL;
    }

    int rc = MCME_OK;
    McmeNbtDoc doc;
    if (!mcme_nbt_parse(&doc, arena, payload.data, payload.len, false)) {
        char why[256];
        snprintf(why, sizeof why, "%s", mcme_last_error());
        mcme_set_error("region: the chunk at (%ld, %ld) will not decode: %s",
                       cx, cz, why);
        rc = MCME_EXIT_BAD_FORMAT;
    } else {
        McmeBuffer out;
        mcme_buffer_init(&out);
        bool ok = g_opts.json ? mcme_nbt_write_json(doc.root, &out, 0)
                              : mcme_nbt_write_snbt(doc.root, &out, 0);
        if (!ok) {
            rc = MCME_EXIT_INTERNAL;
        } else {
            fwrite(out.data, 1, out.len, stdout);
            fputc('\n', stdout);
        }
        mcme_buffer_free(&out);
    }

    mcme_arena_free(arena);
    mcme_buffer_free(&payload);
    return rc;
}

/*
 * `mcme region chunk <world> <cx> <cz>` reads one Java chunk: the
 * region file follows from the coordinates by floor(chunk / 32), and
 * the chunk's NBT prints as SNBT or, under --json, the JSON view.
 *
 * Exit codes: 2 for a world or region file that is not there and for
 * a chunk the world does not store, 3 for a chunk stored but not in a
 * format mcme reads, 4 for store errors, 1 for usage.
 */
static int cmd_region_chunk(int argc, char **argv)
{
    int32_t dimension = 0;
    if (!extract_dimension("region", &argc, argv, &dimension)) {
        return MCME_EXIT_USAGE;
    }
    if (argc != 3) {
        mcme_set_error("region: usage: region chunk <world> <cx> <cz> "
                       "[--dimension <name|number>]");
        return MCME_EXIT_USAGE;
    }
    const char *path = argv[0];
    long cx, cz;
    if (!parse_coord("region", argv[1], &cx)) return MCME_EXIT_USAGE;
    if (!parse_coord("region", argv[2], &cz)) return MCME_EXIT_USAGE;

    const char *dim_dir = NULL;
    if (!mcme_java_dimension_dir(dimension, &dim_dir)) {
        return MCME_EXIT_USAGE;
    }

    McmeWorldInfo info;
    if (!mcme_world_probe(&info, path)) {
        return mcme_fs_exists(path) ? MCME_EXIT_IO : MCME_EXIT_NOT_FOUND;
    }
    if (info.kind != MCME_WORLD_JAVA) {
        mcme_set_error("region: '%s' is not a Java world", path);
        mcme_world_info_free(&info);
        return MCME_EXIT_NOT_FOUND;
    }

    int rx, rz;
    mcme_region_of((int)cx, (int)cz, &rx, &rz);

    char file[MCME_REGION_PATH_MAX];
    if (mcme_region_path(file, sizeof file, info.path, dim_dir,
                         rx, rz) == NULL) {
        mcme_set_error("region: the path to r.%d.%d is too long", rx, rz);
        mcme_world_info_free(&info);
        return MCME_EXIT_INTERNAL;
    }

    /* The same two answers as the inventory: a region file that is not
       there is an absence, a file present but not readable as a region
       is format. */
    McmeRegion reg;
    if (!mcme_region_open(&reg, file)) {
        int rc = mcme_fs_exists(file) ? MCME_EXIT_BAD_FORMAT
                                      : MCME_EXIT_NOT_FOUND;
        mcme_world_info_free(&info);
        return rc;
    }

    int rc;
    if (!mcme_region_chunk_present(&reg, (int)cx, (int)cz)) {
        /* Not stored is an absence the caller asked about — a
           different answer from the slot being there and unreadable. */
        mcme_set_error("region: no chunk is stored at (%ld, %ld)", cx, cz);
        rc = MCME_EXIT_NOT_FOUND;
    } else {
        rc = read_one_chunk(&reg, cx, cz);
    }

    mcme_region_close(&reg);
    mcme_world_info_free(&info);
    return rc;
}

/*
 * `mcme region chunk set <world> <cx> <cz> <snbt>` replaces one Java
 * chunk's payload. The document is the read verb's own output, edited
 * in between — read, edit, write, silent on success like every other
 * write verb. The library answers every refusal; refusal_exit gives it
 * the code it would carry anywhere else in the CLI.
 */
static int cmd_region_chunk_write(int argc, char **argv)
{
    int32_t dimension = 0;
    if (!extract_dimension("region", &argc, argv, &dimension)) {
        return MCME_EXIT_USAGE;
    }
    if (argc != 4) {
        mcme_set_error("region: usage: region chunk set <world> <cx> <cz> "
                       "<snbt-document> [--dimension <name|number>]");
        return MCME_EXIT_USAGE;
    }
    const char *path = argv[0];
    long cx, cz;
    if (!parse_coord("region", argv[1], &cx)) return MCME_EXIT_USAGE;
    if (!parse_coord("region", argv[2], &cz)) return MCME_EXIT_USAGE;
    const char *snbt = argv[3];

    const char *dim_dir = NULL;
    if (!mcme_java_dimension_dir(dimension, &dim_dir)) {
        return MCME_EXIT_USAGE;
    }

    McmeWorldInfo info;
    if (!mcme_world_probe(&info, path)) {
        return mcme_fs_exists(path) ? MCME_EXIT_IO : MCME_EXIT_NOT_FOUND;
    }
    if (info.kind != MCME_WORLD_JAVA) {
        mcme_set_error("region: '%s' is not a Java world", path);
        mcme_world_info_free(&info);
        return MCME_EXIT_NOT_FOUND;
    }

    int rx, rz;
    mcme_region_of((int)cx, (int)cz, &rx, &rz);

    char file[MCME_REGION_PATH_MAX];
    if (mcme_region_path(file, sizeof file, info.path, dim_dir,
                         rx, rz) == NULL) {
        mcme_set_error("region: the path to r.%d.%d is too long", rx, rz);
        mcme_world_info_free(&info);
        return MCME_EXIT_INTERNAL;
    }

    McmeArena *arena = mcme_arena_new();
    if (arena == NULL) {
        mcme_world_info_free(&info);
        mcme_set_error("region: out of memory");
        return MCME_EXIT_INTERNAL;
    }

    /* The document arrives as SNBT — the read verb's own output. What
       will not parse is usage, before any file is touched. */
    McmeTag *root = NULL;
    if (!mcme_nbt_parse_snbt(arena, snbt, strlen(snbt), &root)) {
        mcme_arena_free(arena);
        mcme_world_info_free(&info);
        return MCME_EXIT_USAGE;      /* the parser set the error */
    }

    McmeWriteRefusal refusal = MCME_WRITE_REFUSAL_NONE;
    bool wrote = mcme_region_set_chunk(file, (int)cx, (int)cz, root, arena,
                                       (uint32_t)time(NULL), &refusal);

    int rc = wrote ? MCME_OK : refusal_exit(refusal);
    mcme_arena_free(arena);
    mcme_world_info_free(&info);
    return rc;
}

/*
 * `mcme region compact <world> <rx> <rz>` rewrites one region file
 * packed: live chunks only, in slot order, the slack every set leaves
 * behind gone. The region file is the unit, the same unit the chunk
 * write's append-then-repoint discipline serves — this is that
 * discipline's other half, taken from the side where the old bytes
 * are safe to lose.
 *
 * Silent on success, like db compact and every write verb here: what
 * happened is the inventory's next listing, and a verb that always
 * succeeds has no report its caller needs. The refusals are the
 * chunk write's vocabulary; refusal_exit gives them the codes they
 * would carry anywhere else in the CLI.
 */
static int cmd_region_compact(int argc, char **argv)
{
    int32_t dimension = 0;
    if (!extract_dimension("region", &argc, argv, &dimension)) {
        return MCME_EXIT_USAGE;
    }
    if (argc != 3) {
        mcme_set_error("region: usage: region compact <world> <rx> <rz> "
                       "[--dimension <name|number>]");
        return MCME_EXIT_USAGE;
    }
    const char *path = argv[0];
    long rx, rz;
    if (!parse_coord("region", argv[1], &rx)) return MCME_EXIT_USAGE;
    if (!parse_coord("region", argv[2], &rz)) return MCME_EXIT_USAGE;

    const char *dim_dir = NULL;
    if (!mcme_java_dimension_dir(dimension, &dim_dir)) {
        return MCME_EXIT_USAGE;
    }

    McmeWorldInfo info;
    if (!mcme_world_probe(&info, path)) {
        return mcme_fs_exists(path) ? MCME_EXIT_IO : MCME_EXIT_NOT_FOUND;
    }
    if (info.kind != MCME_WORLD_JAVA) {
        mcme_set_error("region: '%s' is not a Java world", path);
        mcme_world_info_free(&info);
        return MCME_EXIT_NOT_FOUND;
    }

    char file[MCME_REGION_PATH_MAX];
    if (mcme_region_path(file, sizeof file, info.path, dim_dir,
                         (int)rx, (int)rz) == NULL) {
        mcme_set_error("region: the path to r.%ld.%ld is too long",
                       rx, rz);
        mcme_world_info_free(&info);
        return MCME_EXIT_INTERNAL;
    }

    McmeRegionCompact summary;
    McmeWriteRefusal refusal = MCME_WRITE_REFUSAL_NONE;
    bool compacted = mcme_region_compact(file, &summary, &refusal);

    int rc = compacted ? MCME_OK : refusal_exit(refusal);
    mcme_world_info_free(&info);
    return rc;
}

/*
 * The Java half of the block verbs: find the chunk's region file,
 * read the chunk document, hand the tree to the palette layer. The
 * chunk is the unit the region verbs made familiar — an absent chunk
 * is "not stored", since a chunk document carries bookkeeping the
 * game would have to trust and mcme will not invent it, and a file
 * that will not read is exit 3. Reading takes no lock; region files
 * have none.
 *
 * The caller frees `*arena` when it is done with `*doc`; `file` is
 * the region file the chunk came from, which the write side needs.
 */
/* The verbs' road to one block's spec goes through the core now
   (G9): the status the store answered, the exit code the family
   has always given it — and the "chunk: ..." message it set,
   standing verbatim. */
static int world_block_exit(McmeWorldBlockStatus st)
{
    switch (st) {
    case MCME_WORLDBLOCK_OK:          return MCME_OK;
    case MCME_WORLDBLOCK_NOT_FOUND:   return MCME_EXIT_NOT_FOUND;
    case MCME_WORLDBLOCK_BAD_FORMAT:  return MCME_EXIT_BAD_FORMAT;
    case MCME_WORLDBLOCK_IO:          return MCME_EXIT_IO;
    case MCME_WORLDBLOCK_USAGE:       return MCME_EXIT_USAGE;
    case MCME_WORLDBLOCK_INTERNAL:    return MCME_EXIT_INTERNAL;
    }
    return MCME_EXIT_INTERNAL;
}

static int java_set_block(const McmeWorldInfo *info, int32_t dimension,
                          long x, long y, long z,
                          const char *name, const McmeTag *states)
{
    long y_sec = y >> 4;
    if (y_sec < -128 || y_sec > 127) {
        mcme_set_error("chunk: the Y of block (%ld, %ld, %ld) is past "
                       "the sections a chunk can list", x, y, z);
        return MCME_EXIT_USAGE;
    }

    McmeArena *arena = NULL;
    McmeNbtDoc doc;
    char file[MCME_REGION_PATH_MAX];
    int rc = world_block_exit(mcme_java_chunk_doc(
        info->path, dimension, x, y, z, &arena, &doc,
        file, sizeof file));
    if (rc == MCME_OK) {
        bool changed = false;
        McmeWriteRefusal refusal = MCME_WRITE_REFUSAL_NONE;
        if (!mcme_java_set_block(arena, doc.root, (int)x, (int)y,
                                 (int)z, name, states, &changed,
                                 &refusal)) {
            char why[256];
            snprintf(why, sizeof why, "%s", mcme_last_error());
            mcme_set_error("chunk: %s", why);
            rc = refusal_exit(refusal);
        } else if (changed) {
            /* The edited tree goes out the way `region chunk set`
               sends a document: encoded, compressed, appended, the
               slot repointed. A no-op edit writes nothing — the
               region file keeps its bytes, like Bedrock's. */
            if (!mcme_region_set_chunk(file, (int)(x >> 4),
                                       (int)(z >> 4), doc.root, arena,
                                       (uint32_t)time(NULL),
                                       &refusal)) {
                char why[256];
                snprintf(why, sizeof why, "%s", mcme_last_error());
                mcme_set_error("chunk: %s", why);
                rc = refusal_exit(refusal);
            }
        }
    }
    if (arena != NULL) mcme_arena_free(arena);
    return rc;
}

/*
 * The Java half of `chunk fill`: check every region file the box
 * touches first, then edit and write — a chunk the region file
 * does not have refuses the whole fill with nothing written,
 * where Bedrock creates the subchunks it needs. No LOCK: a Java
 * world is region files, no single store to take one on.
 *
 * Both passes moved whole into the core (G17, mcme_java_fill_check_world
 * / mcme_java_fill_world); the CLI keeps only its boundary — the
 * fill's own failure wrapped in "chunk: ", the check's message
 * standing as it always did.
 */
static int java_fill(const McmeWorldInfo *info, int32_t dimension,
                     const McmeBox *box, const char *name,
                     const McmeTag *states)
{
    McmeWriteRefusal refusal = MCME_WRITE_REFUSAL_NONE;

    if (!mcme_java_fill_check_world(info, dimension, box, name, states,
                                    &refusal)) {
        return refusal_exit(refusal);
    }
    if (!mcme_java_fill_world(info, dimension, box, name, states,
                              &refusal)) {
        char why[256];
        snprintf(why, sizeof why, "%s", mcme_last_error());
        mcme_set_error("chunk: %s", why);
        return refusal_exit(refusal);
    }
    return MCME_OK;
}

/*
 * The Bedrock half of `chunk get`, the body this verb grew: the
 * subchunk key, the store's read, the layer's palette, the block
 * spec. Reading takes no LOCK — the game can hold the world open
 * while a block is read.
 */
/* The Bedrock read lives in the core now too — mcme_world_block_spec
   dispatches on the world's kind (G9). */

static int cmd_chunk_set(int argc, char **argv)
{
    int32_t dimension = 0;
    if (!extract_dimension("chunk", &argc, argv, &dimension)) {
        return MCME_EXIT_USAGE;
    }
    if (argc != 5) {
        mcme_set_error("chunk: usage: chunk set <world> <x> <y> <z> "
                       "<block> [--dimension <name|number>]");
        return MCME_EXIT_USAGE;
    }
    const char *path = argv[0];
    long x, y, z;
    if (!parse_coord("chunk", argv[1], &x)) return MCME_EXIT_USAGE;
    if (!parse_coord("chunk", argv[2], &y)) return MCME_EXIT_USAGE;
    if (!parse_coord("chunk", argv[3], &z)) return MCME_EXIT_USAGE;

    McmeWorldInfo info;
    if (!mcme_world_probe(&info, path)) {
        return mcme_fs_exists(path) ? MCME_EXIT_IO : MCME_EXIT_NOT_FOUND;
    }
    if (info.kind != MCME_WORLD_BEDROCK && info.kind != MCME_WORLD_JAVA) {
        mcme_set_error("chunk: '%s' is not a Bedrock or Java world", path);
        mcme_world_info_free(&info);
        return MCME_EXIT_NOT_FOUND;
    }

    McmeArena *arena = mcme_arena_new();
    if (arena == NULL) {
        mcme_world_info_free(&info);
        mcme_set_error("chunk: out of memory");
        return MCME_EXIT_INTERNAL;
    }

    char name[256];
    const McmeTag *states = NULL;
    int rc = parse_block_spec("chunk", argv[4], arena, name, sizeof name,
                              &states);
    if (rc == MCME_OK) {
        if (info.kind == MCME_WORLD_JAVA) {
            /* The chunk document, its palettes, one position — and
               the whole chunk goes back out through the region
               write when the edit says something new. */
            rc = java_set_block(&info, dimension, x, y, z,
                                name, states);
        } else {
            /* A subchunk the world has not stored is created, so
               there is nothing to ask before the edit: set_block
               opens the store, takes the LOCK, and either writes or
               explains itself. */
            McmeNbtDoc entry;
            if (!mcme_bedrock_block_entry(arena, name, states,
                                          &entry)) {
                rc = MCME_EXIT_INTERNAL;
            } else {
                McmeWriteRefusal refusal = MCME_WRITE_REFUSAL_NONE;
                if (!mcme_bedrock_set_block(info.db_path,
                                            (int32_t)x, (int32_t)y,
                                            (int32_t)z, dimension,
                                            &entry, &refusal)) {
                    char why[256];
                    snprintf(why, sizeof why, "%s", mcme_last_error());
                    mcme_set_error("chunk: %s", why);
                    rc = refusal_exit(refusal);
                }
            }
        }
    }

    mcme_arena_free(arena);
    mcme_world_info_free(&info);
    return rc;
}

/*
 * `mcme chunk get <world> <x> <y> <z>` prints the block at the
 * coordinate as the same spec `chunk set` accepts — the name, with
 * states in braces when the palette entry has any. `set` followed
 * by `get` prints what went in, so an agent can verify its own
 * edit, and the text `get` prints can be passed straight back to
 * `set`. Both worlds answer: Bedrock from the subchunk's layer
 * palette, Java from the chunk's section palettes — the same spec
 * text either way, so a script reads one dialect.
 *
 * The palette entry's version is not printed: the spec syntax has
 * no version field, and the version mcme writes for entries it
 * adds is a constant. What is printed is what an edit would
 * repeat.
 *
 * Exit codes mirror `chunk set`: 2 for a subchunk or chunk that is
 * not stored, 3 when the stored payload is not the format mcme
 * reads, 4 for store errors. Reading does not take the LOCK — the
 * game can hold the world open while a block is read; Java worlds
 * have no lock to take.
 */
static int cmd_chunk_get(int argc, char **argv)
{
    int32_t dimension = 0;
    if (!extract_dimension("chunk", &argc, argv, &dimension)) {
        return MCME_EXIT_USAGE;
    }
    if (argc != 4) {
        mcme_set_error("chunk: usage: chunk get <world> <x> <y> <z> "
                       "[--dimension <name|number>]");
        return MCME_EXIT_USAGE;
    }
    const char *path = argv[0];
    long x, y, z;
    if (!parse_coord("chunk", argv[1], &x)) return MCME_EXIT_USAGE;
    if (!parse_coord("chunk", argv[2], &y)) return MCME_EXIT_USAGE;
    if (!parse_coord("chunk", argv[3], &z)) return MCME_EXIT_USAGE;

    McmeWorldInfo info;
    if (!mcme_world_probe(&info, path)) {
        return mcme_fs_exists(path) ? MCME_EXIT_IO : MCME_EXIT_NOT_FOUND;
    }
    if (info.kind != MCME_WORLD_BEDROCK && info.kind != MCME_WORLD_JAVA) {
        mcme_set_error("chunk: '%s' is not a Bedrock or Java world", path);
        mcme_world_info_free(&info);
        return MCME_EXIT_NOT_FOUND;
    }

    McmeBuffer spec;
    mcme_buffer_init(&spec);
    /* The store's own answer, through the core's one road (G9). */
    int rc = world_block_exit(
        mcme_world_block_spec(&info, dimension, x, y, z, &spec));

    if (rc == MCME_OK) {
        if (g_opts.json) {
            /* The buffer is bytes, not a C string; cJSON wants
               a NUL-terminated one. */
            char *text_spec = malloc((size_t)spec.len + 1);
            if (text_spec == NULL) {
                mcme_set_error("chunk: out of memory");
                rc = MCME_EXIT_INTERNAL;
            } else {
                memcpy(text_spec, spec.data, spec.len);
                text_spec[spec.len] = '\0';
                cJSON *root = cJSON_CreateObject();
                cJSON_AddStringToObject(root, "block", text_spec);
                cJSON_AddNumberToObject(root, "x", (double)x);
                cJSON_AddNumberToObject(root, "y", (double)y);
                cJSON_AddNumberToObject(root, "z", (double)z);
                char *text = cJSON_PrintUnformatted(root);
                if (text == NULL) {
                    mcme_set_error("chunk: out of memory");
                    rc = MCME_EXIT_INTERNAL;
                } else {
                    printf("%s\n", text);
                    cJSON_free(text);
                }
                cJSON_Delete(root);
                free(text_spec);
            }
        } else {
            printf("%.*s\n", (int)spec.len,
                   spec.data != NULL ? (const char *)spec.data : "");
        }
    }

    mcme_buffer_free(&spec);
    mcme_world_info_free(&info);
    return rc;
}

/* The seven block states `chunk mesh` reads: the target plus its
   six neighbours, each a NUL-terminated copy of the world's own
   answer for the prelude. */
static void free_block_specs(McmeBuffer *specs, char **text, int n)
{
    for (int i = 0; i < n; ++i) {
        free(text[i]);
        mcme_buffer_free(&specs[i]);
    }
}

/*
 * `mcme chunk mesh <world> <pack> <x> <y> <z>` answers the
 * builder's table (D35-D38) for the block standing at one
 * position of a real chunk: its mesh through the block prelude,
 * and the six neighbours READ OUT OF THE WORLD ITSELF — each
 * side's own transparency, so the cull clauses answer per side
 * (D38), at the world's own height for the band shade (D36).
 *
 * The target's chunk absent is exit 2, what `chunk get` answers.
 * A NEIGHBOUR's chunk outside the stored world is air — the
 * world's edge stands open, partial, firing neither clause; any
 * other read failure is the read's own refusal. JSON carries the
 * mesh document, the page's size, the block and the coordinates
 * beside the table's bounds/auvs/tints; human adds the position
 * to the heading and the six answers below the table.
 */
static int cmd_chunk_mesh(int argc, char **argv)
{
    int32_t dimension = 0;
    if (!extract_dimension("chunk", &argc, argv, &dimension)) {
        return MCME_EXIT_USAGE;
    }
    if (argc != 5) {
        mcme_set_error("chunk: usage: chunk mesh <world> <pack> <x> "
                       "<y> <z> [--dimension <name|number>]");
        return MCME_EXIT_USAGE;
    }
    const char *path = argv[0];
    const char *pack_dir = argv[1];
    long x, y, z;
    if (!parse_coord("chunk", argv[2], &x)) return MCME_EXIT_USAGE;
    if (!parse_coord("chunk", argv[3], &y)) return MCME_EXIT_USAGE;
    if (!parse_coord("chunk", argv[4], &z)) return MCME_EXIT_USAGE;

    McmeWorldInfo info;
    if (!mcme_world_probe(&info, path)) {
        return mcme_fs_exists(path) ? MCME_EXIT_IO : MCME_EXIT_NOT_FOUND;
    }
    if (info.kind != MCME_WORLD_BEDROCK && info.kind != MCME_WORLD_JAVA) {
        mcme_set_error("chunk: '%s' is not a Bedrock or Java world", path);
        mcme_world_info_free(&info);
        return MCME_EXIT_NOT_FOUND;
    }

    /* The neighbour each side reads, DOWN through WEST — the
       builder's CULL_MAP (chunk_builder_cy.pyx 49-55) one face at
       a time, the same offsets the cull test walks; the core's one
       table since G10 (mcme_cull_offset). */

    McmeBuffer specs[7];
    char *text[7] = { 0 };
    for (int s = 0; s < 7; ++s) {
        int dx = 0, dy = 0, dz = 0;
        if (s != 0) mcme_cull_offset(s - 1, &dx, &dy, &dz);
        mcme_buffer_init(&specs[s]);
        long sx = x + dx;
        long sy = y + dy;
        long sz = z + dz;
        int rc = world_block_exit(mcme_world_block_spec(
            &info, dimension, sx, sy, sz, &specs[s]));
        if (rc == MCME_OK) {
            /* The buffer is bytes, not a C string; the prelude
               wants one. */
            text[s] = malloc((size_t)specs[s].len + 1);
            if (text[s] == NULL) {
                mcme_set_error("chunk mesh: out of memory");
                rc = MCME_EXIT_INTERNAL;
            } else {
                memcpy(text[s], specs[s].data, specs[s].len);
                text[s][specs[s].len] = '\0';
            }
        } else if (s > 0 && rc == MCME_EXIT_NOT_FOUND) {
            /* Outside the stored world stands air — the world's
               edge, partial, firing neither clause. */
            mcme_clear_error();
            if (!mcme_buffer_append_cstr(&specs[s], "minecraft:air")) {
                mcme_set_error("chunk mesh: out of memory");
                rc = MCME_EXIT_INTERNAL;
            } else {
                text[s] = malloc((size_t)specs[s].len + 1);
                if (text[s] == NULL) {
                    mcme_set_error("chunk mesh: out of memory");
                    rc = MCME_EXIT_INTERNAL;
                } else {
                    memcpy(text[s], specs[s].data, specs[s].len);
                    text[s][specs[s].len] = '\0';
                    rc = MCME_OK;
                }
            }
        }
        if (rc != MCME_OK) {
            free_block_specs(specs, text, 7);
            mcme_world_info_free(&info);
            return rc;
        }
    }

    /* The target's own mesh, and the six neighbours' transparencies
       through the same prelude — what each side's cull test reads. */
    McmeMesh mesh;
    McmeTextureIndex *tex = NULL;
    int rc = build_block_mesh("chunk mesh", pack_dir, text[0], &mesh,
                              &tex);
    if (rc != MCME_OK) {
        free_block_specs(specs, text, 7);
        mcme_world_info_free(&info);
        return rc;
    }
    McmeTransparency beside[MCME_CULL_NONE];
    for (int i = 0; i < MCME_CULL_NONE; ++i)
        beside[i] = MCME_TRANSPARENCY_PARTIAL;
    for (int s = 1; s < 7; ++s) {
        McmeMesh nm;
        McmeTextureIndex *ntex = NULL;
        int brc = build_block_mesh("chunk mesh", pack_dir, text[s],
                                   &nm, &ntex);
        if (brc != MCME_OK) {
            mcme_mesh_free(&mesh);
            mcme_texture_index_close(tex);
            free_block_specs(specs, text, 7);
            mcme_world_info_free(&info);
            return brc;
        }
        beside[s - 1] = nm.transparency;
        mcme_mesh_free(&nm);
        mcme_texture_index_close(ntex);
    }

    /* The page the coordinates are in — the pack's textures plus
       the missing image, exactly what `mcme atlas` packs. */
    size_t count = mcme_texture_count(tex);
    const char **paths = count ? malloc(count * sizeof *paths) : NULL;
    if (count && !paths) {
        mcme_texture_index_close(tex);
        mcme_mesh_free(&mesh);
        free_block_specs(specs, text, 7);
        mcme_world_info_free(&info);
        mcme_set_error("chunk mesh: out of memory");
        return MCME_EXIT_INTERNAL;
    }
    for (size_t i = 0; i < count; ++i)
        paths[i] = mcme_texture_at(tex, i);
    if (count > 1) qsort(paths, count, sizeof *paths, cmp_path_ptr);

    McmeAtlas atlas;
    bool ok = mcme_atlas_build(paths, count, &atlas);
    free(paths);
    mcme_texture_index_close(tex);
    if (!ok) {
        char why[256];
        snprintf(why, sizeof why, "%s", mcme_last_error());
        mcme_set_error("chunk mesh: %s", why);
        mcme_mesh_free(&mesh);
        free_block_specs(specs, text, 7);
        mcme_world_info_free(&info);
        return MCME_EXIT_INTERNAL;
    }

    McmeAtlasMesh table;
    if (!mcme_atlas_mesh_dirs(&atlas, &mesh, y, beside, &table)) {
        char why[256];
        snprintf(why, sizeof why, "%s", mcme_last_error());
        mcme_set_error("chunk mesh: %s", why);
        mcme_atlas_free(&atlas);
        mcme_mesh_free(&mesh);
        free_block_specs(specs, text, 7);
        mcme_world_info_free(&info);
        return MCME_EXIT_INTERNAL;
    }

    bool shown;
    if (g_opts.json) {
        struct cJSON *doc = json_mesh(&mesh);
        if (doc) {
            cJSON_AddNumberToObject(doc, "size", atlas.size);
            cJSON_AddStringToObject(doc, "block", text[0]);
            cJSON_AddNumberToObject(doc, "x", (double)x);
            cJSON_AddNumberToObject(doc, "y", (double)y);
            cJSON_AddNumberToObject(doc, "z", (double)z);
            struct cJSON *dirs = cJSON_GetObjectItem(doc, "dirs");
            for (int i = 0; i < 7; ++i) {
                if (!table.present[i]) continue;
                struct cJSON *d =
                    cJSON_GetObjectItem(dirs, k_dir_names[i]);
                if (!d) continue;
                json_add_floats(d, "bounds", table.buckets[i].bounds,
                                table.buckets[i].row_count * 4);
                json_add_floats(d, "auvs", table.buckets[i].auvs,
                                table.buckets[i].row_count * 2);
                json_add_floats(d, "tints", table.buckets[i].tints,
                                table.buckets[i].row_count * 3);
            }
            char *text_doc = cJSON_PrintUnformatted(doc);
            printf("%s\n", text_doc ? text_doc : "{}");
            if (text_doc) cJSON_free(text_doc);
            cJSON_Delete(doc);
        }
        shown = doc != NULL;
    } else {
        static const char *const trans_words[3] = {
            "opaque", "translucent", "partial" };
        char heading[512];
        snprintf(heading, sizeof heading, "%s at (%ld, %ld, %ld)",
                 text[0], x, y, z);
        shown = print_mesh(&mesh, heading, &atlas);
        if (shown) {
            printf("  beside\n");
            for (int i = 0; i < MCME_CULL_NONE; ++i)
                printf("    %-10s %s (%s)\n", k_dir_names[i],
                       text[i + 1], trans_words[beside[i]]);
        }
    }
    mcme_atlas_mesh_free(&table);
    mcme_atlas_free(&atlas);
    mcme_mesh_free(&mesh);
    free_block_specs(specs, text, 7);
    mcme_world_info_free(&info);
    if (!shown) {
        mcme_set_error("chunk mesh: out of memory");
        return MCME_EXIT_INTERNAL;
    }
    return MCME_OK;
}

/* ========================================================================
 * The whole sub-chunk's table (D40)
 *
 * The grid the builder is handed for one sub-chunk — the 16x16x16 of
 * its own cells with one cell of every neighbour standing around them
 * — composed by the core whole since G11 (mcme_world_subgrid, the
 * GUI's section view asking the same road); this verb walks it and
 * answers the table for every cell the cull leaves rows. The loop is
 * the builder's own: x, then y, then z (chunk_builder_cy.pyx
 * 273-275), each cell's six neighbours its real ones — D38's clauses
 * per side, D36's shade at the cell's own world height. The edge
 * rule and the refusals are the core's — the "chunk: ..." messages
 * the verbs have always printed — a refused compose mapping its
 * status to this verb's exit code.
 */

/* A distinct state the grid holds: its spec through the block
   prelude once — mesh, transparency, textures for the page — the
   sub-chunk's handful of states instead of one run per cell. */
typedef struct {
    const char       *spec;
    McmeMesh          mesh;
    McmeTextureIndex *tex;
    McmeTransparency  transparency;
} SubMeshState;

static int sub_mesh_state(const char *pack_dir, const char *spec,
                          SubMeshState *out)
{
    out->spec = spec;
    out->tex = NULL;
    int rc = build_block_mesh("chunk submesh", pack_dir, spec,
                              &out->mesh, &out->tex);
    if (rc != MCME_OK) return rc;
    out->transparency = out->mesh.transparency;
    return MCME_OK;
}

/*
 * `mcme chunk submesh <world> <pack> <x> <y> <z>` answers the
 * builder's table for the WHOLE sub-chunk the block stands in
 * (D40): every cell the cull leaves rows, in the builder's own
 * loop order, each cell's six neighbours read out of the world
 * itself — the sub-chunks beside this one included, their edge
 * layers standing as the padding the cull reads.
 *
 * JSON carries the page's size, the sub-chunk's coordinates, the
 * two streams' row totals, and the cells that answer rows — each
 * with its position, its block and its dirs' bounds/auvs/tints,
 * the per-position table's own columns. A cell the cull leaves
 * without a single face is not listed: there are no rows for it
 * to name. Human prints the totals, the per-bucket counts among
 * them.
 */
static int cmd_chunk_submesh(int argc, char **argv)
{
    int32_t dimension = 0;
    if (!extract_dimension("chunk", &argc, argv, &dimension)) {
        return MCME_EXIT_USAGE;
    }
    if (argc != 5) {
        mcme_set_error("chunk: usage: chunk submesh <world> <pack> "
                       "<x> <y> <z> [--dimension <name|number>]");
        return MCME_EXIT_USAGE;
    }
    const char *path = argv[0];
    const char *pack_dir = argv[1];
    long x, y, z;
    if (!parse_coord("chunk", argv[2], &x)) return MCME_EXIT_USAGE;
    if (!parse_coord("chunk", argv[3], &y)) return MCME_EXIT_USAGE;
    if (!parse_coord("chunk", argv[4], &z)) return MCME_EXIT_USAGE;

    McmeWorldInfo info;
    if (!mcme_world_probe(&info, path)) {
        return mcme_fs_exists(path) ? MCME_EXIT_IO : MCME_EXIT_NOT_FOUND;
    }
    if (info.kind != MCME_WORLD_BEDROCK && info.kind != MCME_WORLD_JAVA) {
        mcme_set_error("chunk: '%s' is not a Bedrock or Java world",
                       path);
        mcme_world_info_free(&info);
        return MCME_EXIT_NOT_FOUND;
    }

    int sx = (int)(x >> 4), sy = (int)(y >> 4), sz = (int)(z >> 4);

    /* The composed grid — the core's whole (G11): the seven
       layers, the padded 18³ cells, the edge rule and the
       refusals among them, the same road the GUI's section view
       asks. */
    McmeSubgrid grid;
    McmeWorldBlockStatus gst = mcme_world_subgrid(&info, dimension,
                                                  x, y, z, &grid);
    if (gst != MCME_WORLDBLOCK_OK) {
        mcme_world_info_free(&info);
        return world_block_exit(gst);
    }

    /* Every distinct state the grid holds, through the block
       prelude once each. */
    SubMeshState *states = NULL;
    size_t st_count = 0, st_cap = 0;
    uint16_t *st_cell = malloc(MCME_SUB_CELLS * sizeof *st_cell);
    int rc2 = MCME_OK;
    for (int i = 0; i < MCME_SUB_CELLS && rc2 == MCME_OK; ++i) {
        /* The padded cell this index is: -1..MCME_SUB_EDGE on each
           axis, the grid's own totality answering air past them. */
        int gx = i / (MCME_SUB_WIDTH * MCME_SUB_WIDTH) - 1;
        int gy = (i / MCME_SUB_WIDTH) % MCME_SUB_WIDTH - 1;
        int gz = i % MCME_SUB_WIDTH - 1;
        const char *spec = mcme_subgrid_at(&grid, gx, gy, gz);
        size_t s;
        for (s = 0; s < st_count; ++s)
            if (strcmp(states[s].spec, spec) == 0) break;
        if (s == st_count) {
            if (st_count == st_cap) {
                size_t cap = st_cap ? st_cap * 2 : 8;
                SubMeshState *grow = realloc(
                    states, cap * sizeof *states);
                if (grow == NULL) {
                    mcme_set_error("chunk submesh: out of memory");
                    rc2 = MCME_EXIT_INTERNAL;
                    break;
                }
                states = grow;
                st_cap = cap;
            }
            rc2 = sub_mesh_state(pack_dir, spec, &states[st_count]);
            if (rc2 != MCME_OK) break;
            st_count++;
        }
        st_cell[i] = (uint16_t)s;
    }
    if (rc2 != MCME_OK) {
        for (size_t s = 0; s < st_count; ++s) {
            mcme_texture_index_close(states[s].tex);
            mcme_mesh_free(&states[s].mesh);
        }
        free(states);
        free(st_cell);
        mcme_subgrid_free(&grid);
        mcme_world_info_free(&info);
        return rc2;
    }

    /* The page the coordinates are in — the pack's textures plus
       the missing image, exactly what `chunk mesh` builds for one
       block and what `mcme atlas` packs. Every state's texture
       index is one opening of that same pack table, so the page is
       the pack's whole table whichever state asks for it: one
       index answers, and concatenating them would pack the same
       textures over and over and shift every rectangle the
       reference's page has already settled. */
    size_t path_count = mcme_texture_count(states[0].tex);
    const char **paths = path_count
        ? malloc(path_count * sizeof *paths) : NULL;
    if (path_count && !paths) {
        rc2 = MCME_EXIT_INTERNAL;
        mcme_set_error("chunk submesh: out of memory");
    }
    if (rc2 == MCME_OK) {
        for (size_t i = 0; i < path_count; ++i)
            paths[i] = mcme_texture_at(states[0].tex, i);
        if (path_count > 1)
            qsort(paths, path_count, sizeof *paths, cmp_path_ptr);
    }

    McmeAtlas atlas;
    bool atlas_ok = false;
    if (rc2 == MCME_OK)
        atlas_ok = mcme_atlas_build(paths, path_count, &atlas);
    free(paths);
    if (rc2 == MCME_OK && !atlas_ok) {
        char why[256];
        snprintf(why, sizeof why, "%s", mcme_last_error());
        mcme_set_error("chunk submesh: %s", why);
        rc2 = MCME_EXIT_INTERNAL;
    }

    /* The builder's own loop over the cells: x, then y, then z.
       Each cell's six neighbours are the grid's own cells — the
       padding included — and the shade reads the cell's world
       height. */
    struct cJSON *root = NULL;
    struct cJSON *cells = NULL;
    size_t cell_count = 0, opaque_rows = 0, translucent_rows = 0;
    /* The table's seven buckets, the none one among them — the
       faces no cullface ever drops, whose rows the cull can never
       take away. */
    size_t dir_rows[MCME_CULL_NONE + 1];
    for (int i = 0; i <= MCME_CULL_NONE; ++i) dir_rows[i] = 0;
    if (rc2 == MCME_OK && g_opts.json) {
        root = cJSON_CreateObject();
        if (root) {
            cJSON_AddNumberToObject(root, "size", atlas.size);
            struct cJSON *sub = cJSON_AddObjectToObject(root,
                                                        "subchunk");
            if (sub) {
                cJSON_AddNumberToObject(sub, "x", sx);
                cJSON_AddNumberToObject(sub, "y", sy);
                cJSON_AddNumberToObject(sub, "z", sz);
            }
            cells = cJSON_AddArrayToObject(root, "cells");
        }
    }
    for (int cx = 0; cx < MCME_SUB_EDGE && rc2 == MCME_OK; ++cx) {
        for (int cy = 0; cy < MCME_SUB_EDGE && rc2 == MCME_OK; ++cy) {
            for (int cz = 0; cz < MCME_SUB_EDGE; ++cz) {
                size_t gi = (size_t)(cx + 1) * MCME_SUB_WIDTH
                                * MCME_SUB_WIDTH
                    + (size_t)(cy + 1) * MCME_SUB_WIDTH
                    + (size_t)(cz + 1);
                /* The cell's six neighbours, DOWN through WEST:
                   the grid's own cells, the padding among them. */
                McmeTransparency beside[MCME_CULL_NONE] = {
                    states[st_cell[gi - MCME_SUB_WIDTH]]
                        .transparency,
                    states[st_cell[gi + MCME_SUB_WIDTH]]
                        .transparency,
                    states[st_cell[gi - 1]].transparency,
                    states[st_cell[gi + MCME_SUB_WIDTH
                                        * MCME_SUB_WIDTH]]
                        .transparency,
                    states[st_cell[gi + 1]].transparency,
                    states[st_cell[gi - MCME_SUB_WIDTH
                                        * MCME_SUB_WIDTH]]
                        .transparency,
                };
                const SubMeshState *st = &states[st_cell[gi]];
                McmeAtlasMesh table;
                if (!mcme_atlas_mesh_dirs(&atlas, &st->mesh,
                                          (long)sy * 16 + cy, beside,
                                          &table)) {
                    char why[256];
                    snprintf(why, sizeof why, "%s",
                             mcme_last_error());
                    mcme_set_error("chunk submesh: %s", why);
                    rc2 = MCME_EXIT_INTERNAL;
                    break;
                }
                size_t rows = 0;
                int any = 0;
                for (int i = 0; i <= MCME_CULL_NONE; ++i) {
                    if (!table.present[i]) continue;
                    any = 1;
                    rows += table.buckets[i].row_count;
                    dir_rows[i] += table.buckets[i].row_count;
                }
                if (any) {
                    cell_count++;
                    if (st->transparency
                            == MCME_TRANSPARENCY_TRANSLUCENT)
                        translucent_rows += rows;
                    else
                        opaque_rows += rows;
                    if (cells) {
                        struct cJSON *cell = cJSON_CreateObject();
                        if (cell) {
                            cJSON_AddNumberToObject(
                                cell, "x",
                                (double)((long)sx * 16 + cx));
                            cJSON_AddNumberToObject(
                                cell, "y",
                                (double)((long)sy * 16 + cy));
                            cJSON_AddNumberToObject(
                                cell, "z",
                                (double)((long)sz * 16 + cz));
                            cJSON_AddStringToObject(cell, "block",
                                                    st->spec);
                            struct cJSON *dirs =
                                cJSON_AddObjectToObject(cell, "dirs");
                            if (dirs) {
                                for (int i = 0;
                                     i <= MCME_CULL_NONE; ++i) {
                                    if (!table.present[i]) continue;
                                    struct cJSON *d =
                                        cJSON_AddObjectToObject(
                                            dirs, k_dir_names[i]);
                                    if (!d) continue;
                                    json_add_floats(
                                        d, "bounds",
                                        table.buckets[i].bounds,
                                        table.buckets[i].row_count
                                        * 4);
                                    json_add_floats(
                                        d, "auvs",
                                        table.buckets[i].auvs,
                                        table.buckets[i].row_count
                                        * 2);
                                    json_add_floats(
                                        d, "tints",
                                        table.buckets[i].tints,
                                        table.buckets[i].row_count
                                        * 3);
                                }
                            }
                            cJSON_AddItemToArray(cells, cell);
                        }
                    }
                }
                mcme_atlas_mesh_free(&table);
            }
        }
    }

    bool shown = true;
    if (rc2 == MCME_OK && g_opts.json) {
        if (root) {
            cJSON_AddNumberToObject(root, "opaque",
                                    (double)opaque_rows);
            cJSON_AddNumberToObject(root, "translucent",
                                    (double)translucent_rows);
            char *text_doc = cJSON_PrintUnformatted(root);
            printf("%s\n", text_doc ? text_doc : "{}");
            if (text_doc) cJSON_free(text_doc);
        } else {
            shown = false;
        }
        cJSON_Delete(root);
    } else if (rc2 == MCME_OK) {
        printf("sub-chunk (%d, %d, %d): page %u\n", sx, sy, sz,
               atlas.size);
        printf("  %zu cells answered: %zu opaque rows, %zu "
               "translucent rows\n", cell_count, opaque_rows,
               translucent_rows);
        printf("    per bucket:");
        for (int i = 0; i <= MCME_CULL_NONE; ++i)
            printf(" %s %zu", k_dir_names[i], dir_rows[i]);
        printf("\n");
    }

    for (size_t s = 0; s < st_count; ++s) {
        mcme_texture_index_close(states[s].tex);
        mcme_mesh_free(&states[s].mesh);
    }
    free(states);
    free(st_cell);
    if (atlas_ok) mcme_atlas_free(&atlas);
    mcme_subgrid_free(&grid);
    mcme_world_info_free(&info);
    if (!shown) {
        mcme_set_error("chunk submesh: out of memory");
        return MCME_EXIT_INTERNAL;
    }
    return rc2;
}

/*
 * `mcme chunk fill <world> <x1> <y1> <z1> <x2> <y2> <z2> <block>`
 * sets a box of blocks to one block — corners in any order, both
 * inside the box — for both kinds of world.
 *
 * Bedrock: the whole box is checked first, under the same LOCK the
 * edit then takes, so a subchunk the check cannot edit refuses the
 * whole fill with nothing written. Subchunks the world has not
 * stored are created — one payload write per subchunk — and the
 * positions the box only reaches into keep what they held.
 *
 * Java: the same two passes over the region files the box touches,
 * but a chunk the region file does not have refuses the whole fill
 * (exit 2): mcme will not invent a chunk's bookkeeping. Each
 * changed chunk is repacked once per section the box touches and
 * appended like `chunk set`; a fill that changes nothing rewrites
 * nothing. No LOCK — regions have no single store to hold.
 *
 * Success prints nothing, like `db put`.
 */
static int cmd_chunk_fill(int argc, char **argv)
{
    int32_t dimension = 0;
    if (!extract_dimension("chunk", &argc, argv, &dimension)) {
        return MCME_EXIT_USAGE;
    }
    if (argc != 8) {
        mcme_set_error("chunk: usage: chunk fill <world> <x1> <y1> "
                       "<z1> <x2> <y2> <z2> <block> "
                       "[--dimension <name|number>]");
        return MCME_EXIT_USAGE;
    }
    const char *path = argv[0];
    long c[6];
    for (int i = 0; i < 6; ++i) {
        if (!parse_coord("chunk", argv[1 + i], &c[i])) return MCME_EXIT_USAGE;
    }
    if (!box_y_range_fits(c)) return MCME_EXIT_USAGE;

    McmeWorldInfo info;
    if (!mcme_world_probe(&info, path)) {
        return mcme_fs_exists(path) ? MCME_EXIT_IO : MCME_EXIT_NOT_FOUND;
    }
    if ((info.kind == MCME_WORLD_BEDROCK && info.db_path == NULL)
            || (info.kind != MCME_WORLD_BEDROCK
                && info.kind != MCME_WORLD_JAVA)) {
        mcme_set_error("chunk: '%s' is not a Bedrock or Java world", path);
        mcme_world_info_free(&info);
        return MCME_EXIT_NOT_FOUND;
    }

    McmeArena *arena = mcme_arena_new();
    if (arena == NULL) {
        mcme_world_info_free(&info);
        mcme_set_error("chunk: out of memory");
        return MCME_EXIT_INTERNAL;
    }
    char name[256];
    const McmeTag *states = NULL;
    int rc = parse_block_spec("chunk", argv[7], arena, name, sizeof name,
                              &states);
    McmeNbtDoc entry;
    if (rc == MCME_OK && info.kind == MCME_WORLD_BEDROCK
            && !mcme_bedrock_block_entry(arena, name, states, &entry)) {
        rc = MCME_EXIT_INTERNAL;
    }

    if (rc == MCME_OK) {
        McmeBox box = { (int32_t)c[0], (int32_t)c[1], (int32_t)c[2],
                        (int32_t)c[3], (int32_t)c[4], (int32_t)c[5] };
        if (info.kind == MCME_WORLD_JAVA) {
            rc = java_fill(&info, dimension, &box, name, states);
        } else {
            McmeLevelDb *db = NULL;
            if (!mcme_leveldb_open_rw(&db, info.db_path)) {
                rc = MCME_EXIT_IO;
            } else {
                McmeWriteRefusal refusal = MCME_WRITE_REFUSAL_NONE;
                if (!mcme_bedrock_fill_check(db, &box, dimension,
                                             &refusal)) {
                    rc = refusal_exit(refusal);
                } else if (!mcme_bedrock_fill(db, &box, dimension,
                                              &entry, &refusal)) {
                    char why[256];
                    snprintf(why, sizeof why, "%s", mcme_last_error());
                    mcme_set_error("chunk: %s", why);
                    rc = refusal_exit(refusal);
                }
                mcme_leveldb_close(db);
            }
        }
    }

    mcme_arena_free(arena);
    mcme_world_info_free(&info);
    return rc;
}

/*
 * `mcme chunk prune <world> <x1> <y1> <z1> <x2> <y2> <z2>` deletes
 * whole chunks — the box names the chunk columns, decisions.md#D13 —
 * whichever world half the probe names. Y is parsed and ignored; the
 * library's contract says why (a chunk is all of its Y).
 *
 * Silent on success, like every write verb; `--json` prints the
 * counts: chunks deleted, keys tombstoned (Bedrock), region files
 * punched (Java).
 */
static int cmd_chunk_prune(int argc, char **argv)
{
    int32_t dimension = 0;
    if (!extract_dimension("chunk", &argc, argv, &dimension)) {
        return MCME_EXIT_USAGE;
    }
    if (argc != 7) {
        mcme_set_error("chunk: usage: chunk prune <world> <x1> <y1> "
                       "<z1> <x2> <y2> <z2> "
                       "[--dimension <name|number>]");
        return MCME_EXIT_USAGE;
    }
    const char *path = argv[0];
    long c[6];
    for (int i = 0; i < 6; ++i) {
        if (!parse_coord("chunk", argv[1 + i], &c[i])) {
            return MCME_EXIT_USAGE;
        }
    }

    /* No box_y_range_fits: a prune never names a byte in Y, so any
       Y a coordinate can hold is a Y the prune parses and ignores. */
    McmeBox box = { (int32_t)c[0], (int32_t)c[1], (int32_t)c[2],
                    (int32_t)c[3], (int32_t)c[4], (int32_t)c[5] };

    McmePruneReport report;
    McmeWriteRefusal refusal = MCME_WRITE_REFUSAL_NONE;
    if (!mcme_prune(path, &box, dimension, &report, &refusal)) {
        char why[512];
        snprintf(why, sizeof why, "%s", mcme_last_error());
        mcme_set_error("chunk: %s", why);
        return refusal_exit(refusal);
    }

    if (g_opts.json) {
        printf("{\"chunks\":%lld,\"keys\":%lld,"
               "\"region_files\":%lld}\n",
               (long long)report.chunks, (long long)report.keys,
               (long long)report.region_files);
    }
    return MCME_OK;
}

/*
 * `mcme chunk stats <world> <x1> <y1> <z1> <x2> <y2> <z2>` says
 * what a box is made of: one line per distinct block, most
 * frequent first, in the same spec text the other chunk verbs
 * speak — a line can be pasted back into `set` or `fill`.
 *
 * A subchunk the box touches that is not stored counts its
 * positions as minecraft:air and is reported on stderr:
 * reading a hole is answerable — it is empty — where writing
 * one is not. That is why this verb, alone in the chunk family,
 * does not exit 2 for absence. Only layer zero is counted, the
 * layer the editing verbs act on.
 *
 * Absence is data here; what fails is world data mcme cannot
 * read (exit 3) or the store itself (exit 4).
 */
static int cmd_chunk_stats(int argc, char **argv)
{
    int32_t dimension = 0;
    if (!extract_dimension("chunk", &argc, argv, &dimension)) {
        return MCME_EXIT_USAGE;
    }
    if (argc != 7) {
        mcme_set_error("chunk: usage: chunk stats <world> <x1> <y1> "
                       "<z1> <x2> <y2> <z2> "
                       "[--dimension <name|number>]");
        return MCME_EXIT_USAGE;
    }
    const char *path = argv[0];
    long c[6];
    for (int i = 0; i < 6; ++i) {
        if (!parse_coord("chunk", argv[1 + i], &c[i])) return MCME_EXIT_USAGE;
    }
    if (!box_y_range_fits(c)) return MCME_EXIT_USAGE;

    McmeWorldInfo info;
    if (!mcme_world_probe(&info, path)) {
        return mcme_fs_exists(path) ? MCME_EXIT_IO : MCME_EXIT_NOT_FOUND;
    }
    if (info.kind != MCME_WORLD_BEDROCK || info.db_path == NULL) {
        mcme_set_error("chunk: '%s' is not a Bedrock world", path);
        mcme_world_info_free(&info);
        return MCME_EXIT_NOT_FOUND;
    }

    McmeArena *arena = mcme_arena_new();
    if (arena == NULL) {
        mcme_world_info_free(&info);
        mcme_set_error("chunk: out of memory");
        return MCME_EXIT_INTERNAL;
    }

    int rc = MCME_OK;
    McmeBlockCount *tally = NULL;
    size_t tally_len = 0;
    int64_t missing = 0;
    McmeBox box = { (int32_t)c[0], (int32_t)c[1], (int32_t)c[2],
                    (int32_t)c[3], (int32_t)c[4], (int32_t)c[5] };
    McmeLevelDb *db = NULL;
    if (!mcme_leveldb_open(&db, info.db_path)) {
        rc = MCME_EXIT_IO;
    } else {
        bool counted = mcme_bedrock_count_box(db, &box, dimension,
                                              arena, &tally,
                                              &tally_len, &missing);
        mcme_leveldb_close(db);
        if (!counted) {
            char why[256];
            snprintf(why, sizeof why, "%s", mcme_last_error());
            mcme_set_error("chunk: %s", why);
            rc = MCME_EXIT_BAD_FORMAT;
        }
    }

    if (rc == MCME_OK) {
        if (missing > 0 && !g_opts.quiet) {
            fprintf(stderr,
                    "mcme: chunk stats: %lld subchunk%s not stored, "
                    "counted as air\n",
                    (long long)missing, missing == 1 ? "" : "s");
        }
        if (g_opts.json) {
            cJSON *root = cJSON_CreateObject();
            cJSON_AddNumberToObject(root, "missing_subchunks",
                                    (double)missing);
            cJSON *blocks = cJSON_CreateArray();
            for (size_t i = 0; i < tally_len; ++i) {
                cJSON *row = cJSON_CreateObject();
                cJSON_AddStringToObject(row, "block",
                                        tally[i].block);
                cJSON_AddNumberToObject(row, "count",
                                        (double)tally[i].count);
                cJSON_AddItemToArray(blocks, row);
            }
            cJSON_AddItemToObject(root, "blocks", blocks);
            char *text = cJSON_PrintUnformatted(root);
            if (text == NULL) {
                mcme_set_error("chunk: out of memory");
                rc = MCME_EXIT_INTERNAL;
            } else {
                printf("%s\n", text);
                cJSON_free(text);
            }
            cJSON_Delete(root);
        } else {
            for (size_t i = 0; i < tally_len; ++i) {
                printf("%9lld %s\n", (long long)tally[i].count,
                       tally[i].block);
            }
        }
    }

    mcme_arena_free(arena);
    mcme_world_info_free(&info);
    return rc;
}

/* ========================================================================
 * `mcme chunk height <world> <x> <z>`
 *
 * How high the ground is in one column: the topmost and bottommost
 * non-air block of the stack through (x, z), and the subchunks the
 * world stores for it. The answer is derived from the payloads
 * rather than read from a heightmap record, because the editing
 * verbs do not update a heightmap — a height that trusted one would
 * report the world before the last edit.
 *
 * Exit 2 when the column has no stored subchunk: there is nothing to
 * measure, which is a different thing from a column of air, which
 * answers and exits 0.
 * ======================================================================== */

static int cmd_chunk_height(int argc, char **argv)
{
    int32_t dimension = 0;
    if (!extract_dimension("chunk", &argc, argv, &dimension)) {
        return MCME_EXIT_USAGE;
    }
    if (argc != 3) {
        mcme_set_error("chunk: usage: chunk height <world> <x> <z> "
                       "[--dimension <name|number>]");
        return MCME_EXIT_USAGE;
    }
    const char *path = argv[0];
    long x, z;
    if (!parse_coord("chunk", argv[1], &x)) return MCME_EXIT_USAGE;
    if (!parse_coord("chunk", argv[2], &z)) return MCME_EXIT_USAGE;

    McmeWorldInfo info;
    if (!mcme_world_probe(&info, path)) {
        return mcme_fs_exists(path) ? MCME_EXIT_IO : MCME_EXIT_NOT_FOUND;
    }
    if (info.kind != MCME_WORLD_BEDROCK || info.db_path == NULL) {
        mcme_set_error("chunk: '%s' is not a Bedrock world", path);
        mcme_world_info_free(&info);
        return MCME_EXIT_NOT_FOUND;
    }

    McmeArena *arena = mcme_arena_new();
    if (arena == NULL) {
        mcme_world_info_free(&info);
        mcme_set_error("chunk: out of memory");
        return MCME_EXIT_INTERNAL;
    }

    int rc = MCME_OK;
    McmeColumnHeight col;
    memset(&col, 0, sizeof col);
    McmeLevelDb *db = NULL;
    if (!mcme_leveldb_open(&db, info.db_path)) {
        rc = MCME_EXIT_IO;
    } else {
        bool read = mcme_bedrock_column_height(db, (int32_t)x,
                                               (int32_t)z, dimension,
                                               arena, &col);
        mcme_leveldb_close(db);
        if (!read) {
            /* The store's own error, or a payload this build does
               not read — both are "the world said something mcme
               cannot answer", the same class `chunk stats` reports. */
            char why[256];
            snprintf(why, sizeof why, "%s", mcme_last_error());
            mcme_set_error("chunk: %s", why);
            rc = MCME_EXIT_BAD_FORMAT;
        } else if (col.subchunk_count == 0) {
            mcme_set_error("chunk: no subchunk is stored for the "
                           "column at block (%ld, %ld) in dimension "
                           "%d", x, z, (int)dimension);
            rc = MCME_EXIT_NOT_FOUND;
        }
    }

    if (rc == MCME_OK) {
        if (g_opts.json) {
            cJSON *root = cJSON_CreateObject();
            if (root == NULL) {
                mcme_set_error("chunk: out of memory");
                rc = MCME_EXIT_INTERNAL;
            } else {
                cJSON_AddStringToObject(root, "path", info.path);
                cJSON_AddNumberToObject(root, "dimension",
                                        (double)dimension);
                cJSON_AddNumberToObject(root, "x", (double)x);
                cJSON_AddNumberToObject(root, "z", (double)z);
                cJSON *subs = cJSON_AddArrayToObject(root,
                                                     "subchunks");
                for (size_t i = 0; i < col.subchunk_count; ++i) {
                    double cy = (double)col.subchunks[i];
                    cJSON_AddItemToArray(subs,
                                         cJSON_CreateNumber(cy));
                }
                /* A column of air is an answer, so the fields are
                   null rather than left out: absence is the shape
                   of the answer, not a missing one. */
                if (col.has_surface) {
                    cJSON *surface = cJSON_CreateObject();
                    cJSON_AddNumberToObject(surface, "y",
                                            (double)col.surface_y);
                    cJSON_AddStringToObject(surface, "block",
                                            col.surface_block);
                    cJSON_AddItemToObject(root, "surface", surface);
                } else {
                    cJSON_AddNullToObject(root, "surface");
                }
                if (col.has_floor) {
                    cJSON *floor = cJSON_CreateObject();
                    cJSON_AddNumberToObject(floor, "y",
                                            (double)col.floor_y);
                    cJSON_AddStringToObject(floor, "block",
                                            col.floor_block);
                    cJSON_AddItemToObject(root, "floor", floor);
                } else {
                    cJSON_AddNullToObject(root, "floor");
                }
                char *text = cJSON_PrintUnformatted(root);
                if (text == NULL) {
                    mcme_set_error("chunk: out of memory");
                    rc = MCME_EXIT_INTERNAL;
                } else {
                    printf("%s\n", text);
                    cJSON_free(text);
                }
                cJSON_Delete(root);
            }
        } else {
            printf("%-11s%s\n", "path:", info.path);
            printf("%-11s(%ld, %ld)\n", "column:", x, z);
            printf("%-11s%d\n", "dimension:", (int)dimension);
            printf("%-11s", "subchunks:");
            for (size_t i = 0; i < col.subchunk_count; ++i) {
                printf("%s%d", i == 0 ? "" : " ",
                       (int)col.subchunks[i]);
            }
            printf("\n");
            if (col.has_surface) {
                printf("%-11sy %d %s\n", "surface:",
                       (int)col.surface_y, col.surface_block);
            } else {
                printf("%-11snone\n", "surface:");
            }
            if (col.has_floor) {
                printf("%-11sy %d %s\n", "floor:",
                       (int)col.floor_y, col.floor_block);
            } else {
                printf("%-11snone\n", "floor:");
            }
        }
    }

    mcme_arena_free(arena);
    mcme_world_info_free(&info);
    return rc;
}

/* ========================================================================
 * `mcme chunk entity <world> <x> <y> <z>`
 *
 * The block entity stored at one block — the chest's inventory, the
 * sign's text — which lives in the chunk's own record, not in a
 * subchunk, so `chunk get` cannot see it. The document is printed as
 * the world holds it: nothing added, nothing renamed.
 * ======================================================================== */

static int cmd_chunk_entity(int argc, char **argv)
{
    int32_t dimension = 0;
    if (!extract_dimension("chunk", &argc, argv, &dimension)) {
        return MCME_EXIT_USAGE;
    }
    if (argc != 4) {
        mcme_set_error("chunk: usage: chunk entity <world> <x> <y> "
                       "<z> [--dimension <name|number>]");
        return MCME_EXIT_USAGE;
    }
    const char *path = argv[0];
    long x, y, z;
    if (!parse_coord("chunk", argv[1], &x)) return MCME_EXIT_USAGE;
    if (!parse_coord("chunk", argv[2], &y)) return MCME_EXIT_USAGE;
    if (!parse_coord("chunk", argv[3], &z)) return MCME_EXIT_USAGE;

    McmeWorldInfo info;
    if (!mcme_world_probe(&info, path)) {
        return mcme_fs_exists(path) ? MCME_EXIT_IO : MCME_EXIT_NOT_FOUND;
    }
    if (info.kind != MCME_WORLD_BEDROCK || info.db_path == NULL) {
        mcme_set_error("chunk: '%s' is not a Bedrock world", path);
        mcme_world_info_free(&info);
        return MCME_EXIT_NOT_FOUND;
    }

    McmeArena *arena = mcme_arena_new();
    if (arena == NULL) {
        mcme_world_info_free(&info);
        mcme_set_error("chunk: out of memory");
        return MCME_EXIT_INTERNAL;
    }

    int rc = MCME_OK;
    McmeNbtDoc doc;
    memset(&doc, 0, sizeof doc);

    /* Narrowed once, here: a chunk addresses 32-bit coordinates whatever
       width parse_coord held them in, and doing it once keeps the read
       below on a single line instead of a four-deep continuation. */
    int32_t bx = (int32_t)x, by = (int32_t)y, bz = (int32_t)z;

    McmeLevelDb *db = NULL;
    if (!mcme_leveldb_open(&db, info.db_path)) {
        mcme_arena_free(arena);
        mcme_world_info_free(&info);
        return MCME_EXIT_IO;
    }
    McmeEntityStatus status =
        mcme_bedrock_block_entity(db, bx, by, bz, dimension, arena, &doc);
    mcme_leveldb_close(db);

    switch (status) {
    case MCME_ENTITY_OK:
        break;
    case MCME_ENTITY_ABSENT:
        mcme_set_error("chunk: no block entity is stored at "
                       "block (%ld, %ld, %ld) in dimension %d",
                       x, y, z, (int)dimension);
        rc = MCME_EXIT_NOT_FOUND;
        break;
    case MCME_ENTITY_NOT_READABLE:
        rc = MCME_EXIT_BAD_FORMAT;  /* the error already says */
        break;
    case MCME_ENTITY_STORE:
        rc = MCME_EXIT_IO;          /* the store's own error  */
        break;
    case MCME_ENTITY_USAGE:
        rc = MCME_EXIT_USAGE;
        break;
    case MCME_ENTITY_INTERNAL:
    default:
        rc = MCME_EXIT_INTERNAL;
        break;
    }

    if (rc == MCME_OK) {
        /* The document verbatim: the same text `nbt dump` prints,
           and the same values under --json, which is a view of the
           NBT rather than a round trip. */
        McmeBuffer out;
        mcme_buffer_init(&out);
        bool ok = g_opts.json
            ? mcme_nbt_write_json(doc.root, &out, 0)
            : mcme_nbt_write_snbt(doc.root, &out, 0);
        if (!ok) {
            char why[256];
            snprintf(why, sizeof why, "%s", mcme_last_error());
            mcme_set_error("chunk: the entity at (%ld, %ld, %ld) "
                           "cannot be printed: %s", x, y, z, why);
            rc = MCME_EXIT_BAD_FORMAT;
        } else {
            fwrite(out.data, 1, out.len, stdout);
            fputc('\n', stdout);
        }
        mcme_buffer_free(&out);
    }

    mcme_arena_free(arena);
    mcme_world_info_free(&info);
    return rc;
}

/* ========================================================================
 * `mcme chunk entity set <world> <x> <y> <z> <snbt>` and
 * `mcme chunk entity del <world> <x> <y> <z>`
 *
 * One body: the store layer splices a record the same way for both,
 * and only the document argument differs. Nothing is printed on
 * success, the way `chunk set` and `chunk fill` answer — the exit
 * code is the answer.
 * ======================================================================== */

/*
 * The edit behind both verbs: `set` parses the document it brings
 * in SNBT and stores it at the block, `del` removes whatever is
 * there. Split out so the verbs' argument checking stays flat and
 * both map a refusal through refusal_exit() the same way. The
 * document is the SNBT `chunk entity` prints, so read, edit,
 * write back is one loop; one that will not parse is a bad
 * argument, not bad world data.
 */
static int entity_edit(const char *db_dir, int32_t bx, int32_t by,
                       int32_t bz, int32_t dimension, bool is_set,
                       const char *snbt, McmeArena *arena)
{
    McmeWriteRefusal refusal = MCME_WRITE_REFUSAL_NONE;
    bool wrote;

    if (is_set) {
        McmeTag *entity = NULL;
        if (!mcme_nbt_parse_snbt(arena, snbt, strlen(snbt), &entity)) {
            return MCME_EXIT_USAGE;
        }
        wrote = mcme_bedrock_put_block_entity(db_dir, bx, by, bz,
                                              dimension, entity, arena,
                                              &refusal);
    } else {
        wrote = mcme_bedrock_del_block_entity(db_dir, bx, by, bz,
                                              dimension, &refusal);
    }

    return wrote ? MCME_OK : refusal_exit(refusal);
}

static int cmd_chunk_entity_write(int argc, char **argv, bool is_set)
{
    const char *what = is_set ? "set" : "del";
    int32_t dimension = 0;
    if (!extract_dimension("chunk", &argc, argv, &dimension)) {
        return MCME_EXIT_USAGE;
    }
    int want = is_set ? 5 : 4;
    if (argc != want) {
        mcme_set_error("chunk: usage: chunk entity %s <world> <x> "
                       "<y> <z>%s", what,
                       is_set ? " <snbt-document>" : "");
        return MCME_EXIT_USAGE;
    }
    const char *path = argv[0];
    long x, y, z;
    if (!parse_coord("chunk", argv[1], &x)) return MCME_EXIT_USAGE;
    if (!parse_coord("chunk", argv[2], &y)) return MCME_EXIT_USAGE;
    if (!parse_coord("chunk", argv[3], &z)) return MCME_EXIT_USAGE;

    McmeWorldInfo info;
    if (!mcme_world_probe(&info, path)) {
        return mcme_fs_exists(path) ? MCME_EXIT_IO : MCME_EXIT_NOT_FOUND;
    }
    if (info.kind != MCME_WORLD_BEDROCK || info.db_path == NULL) {
        mcme_set_error("chunk: '%s' is not a Bedrock world", path);
        mcme_world_info_free(&info);
        return MCME_EXIT_NOT_FOUND;
    }

    McmeArena *arena = mcme_arena_new();
    if (arena == NULL) {
        mcme_world_info_free(&info);
        mcme_set_error("chunk: out of memory");
        return MCME_EXIT_INTERNAL;
    }

    /* `set` brings the document; `del` has none. */
    const char *snbt = is_set ? argv[4] : NULL;
    int32_t bx = (int32_t)x, by = (int32_t)y, bz = (int32_t)z;
    int rc = entity_edit(info.db_path, bx, by, bz, dimension,
                         is_set, snbt, arena);

    mcme_arena_free(arena);
    mcme_world_info_free(&info);
    return rc;
}

/* ========================================================================
 * db — raw key/value access to a Bedrock store
 * ======================================================================== */

/*
 * `mcme db <get|put|del> <world> <key-hex> [value-hex]` and
 * `mcme db compact <world>`.
 *
 * This exists so the write path is reachable from a script, per D3: anything
 * worth doing is a verb. It is deliberately raw — keys and values are hex
 * because Bedrock keys carry binary chunk coordinates — and deliberately
 * dangerous-looking: it writes to the store the way the game would, so it
 * takes the LOCK and refuses a store the game has open.
 *
 * get prints the value as hex, or nothing and exit 2 if the key is absent
 * (distinguishing "gone" from "broken" via mcme_last_error()).
 *
 * compact is the manual form of the flush mcme_leveldb_write does by itself
 * past the compaction trigger; it exists because a script should not have to
 * write a dummy key to flush a log, and because the observable result —
 * one table, no logs, same view — is worth asserting from the shell.
 */

static int hex_decode(const char *hex, uint8_t **out, size_t *out_len)
{
    size_t len = strlen(hex);
    if (len % 2 != 0) {
        mcme_set_error("db: hex string has an odd length");
        return 0;
    }
    uint8_t *buf = (uint8_t *)malloc(len / 2 + 1);
    if (buf == NULL) {
        mcme_set_error("db: out of memory");
        return 0;
    }
    for (size_t i = 0; i < len / 2; ++i) {
        unsigned v = 0;
        if (sscanf(hex + i * 2, "%2x", &v) != 1) {
            free(buf);
            mcme_set_error("db: '%s' is not hex", hex);
            return 0;
        }
        buf[i] = (uint8_t)v;
    }
    *out = buf;
    *out_len = len / 2;
    return 1;
}

static int cmd_db(int argc, char **argv)
{
    if (argc < 2) {
        mcme_set_error("db: usage: db <get|put|del> <world> <key-hex> "
                       "[value-hex] | db compact <world>");
        return MCME_EXIT_USAGE;
    }
    const char *op = argv[0];
    const char *path = argv[1];

    bool is_compact = strcmp(op, "compact") == 0;
    if (is_compact) {
        if (argc != 2) {
            mcme_set_error("db: compact takes a world and nothing else");
            return MCME_EXIT_USAGE;
        }
    } else {
        if (argc < 3) {
            mcme_set_error("db: usage: db <get|put|del> <world> <key-hex> "
                           "[value-hex] | db compact <world>");
            return MCME_EXIT_USAGE;
        }
    }

    const char *key_hex = is_compact ? NULL : argv[2];
    const char *val_hex = argc > 3 ? argv[3] : NULL;

    bool is_get = strcmp(op, "get") == 0;
    bool is_put = strcmp(op, "put") == 0;
    bool is_del = strcmp(op, "del") == 0;
    if (!is_get && !is_put && !is_del && !is_compact) {
        mcme_set_error("db: unknown operation '%s'", op);
        return MCME_EXIT_USAGE;
    }
    if ((!is_compact && (is_put && val_hex == NULL)) ||
        (!is_put && !is_compact && argc > 3)) {
        mcme_set_error("db: put takes a value; get and del do not");
        return MCME_EXIT_USAGE;
    }

    uint8_t *key = NULL, *val = NULL;
    size_t key_len = 0, val_len = 0;
    if (!is_compact && !hex_decode(key_hex, &key, &key_len)) return MCME_EXIT_USAGE;
    if (val_hex != NULL && !hex_decode(val_hex, &val, &val_len)) {
        free(key);
        return MCME_EXIT_USAGE;
    }

    McmeWorldInfo info;
    if (!mcme_world_probe(&info, path)) {
        free(key); free(val);
        return mcme_fs_exists(path) ? MCME_EXIT_IO : MCME_EXIT_NOT_FOUND;
    }
    if (info.kind != MCME_WORLD_BEDROCK || info.db_path == NULL) {
        mcme_set_error("db: '%s' is not a Bedrock world", path);
        free(key); free(val);
        mcme_world_info_free(&info);
        return MCME_EXIT_NOT_FOUND;
    }

    /* get reads through a read-only handle: no LOCK, so it works on a store
       the game has open. put, del and compact take the lock the game would. */
    McmeLevelDb *db = NULL;
    bool opened = is_get ? mcme_leveldb_open(&db, info.db_path)
                         : mcme_leveldb_open_rw(&db, info.db_path);
    mcme_world_info_free(&info);
    if (!opened) {
        free(key); free(val);
        return MCME_EXIT_IO;
    }

    int rc = MCME_OK;
    if (is_compact) {
        if (!mcme_leveldb_compact(db)) rc = MCME_EXIT_IO;
    } else if (is_get) {
        McmeBuffer buf;
        mcme_buffer_init(&buf);
        bool deleted = false;
        if (!mcme_leveldb_get(db, key, key_len, &buf, &deleted)) {
            rc = MCME_EXIT_NOT_FOUND;
        } else if (deleted) {
            rc = MCME_EXIT_NOT_FOUND;
        } else {
            for (size_t i = 0; i < buf.len; ++i) printf("%02x", buf.data[i]);
            printf("\n");
        }
        mcme_buffer_free(&buf);
    } else if (is_put) {
        if (!mcme_leveldb_put(db, key, key_len, val, val_len)) rc = MCME_EXIT_IO;
    } else {
        if (!mcme_leveldb_delete(db, key, key_len)) rc = MCME_EXIT_IO;
    }

    mcme_leveldb_close(db);
    free(key); free(val);
    return rc;
}

/* ========================================================================
 * convert — a whole world into another format
 * ======================================================================== */

/* The verdict on a --to target: what this build converts is accepted,
   anything else well-formed is the reserved answer, exit 5, so the
   surface stays testable while the pairs arrive one at a time. */
static int convert_target(const char *to)
{
    const char *slash = strchr(to, '/');
    if (slash == NULL || slash == to || slash[1] == '\0'
            || strchr(slash + 1, '/') != NULL) {
        mcme_set_error("convert: --to wants <platform>/<version>, "
                       "e.g. java/1.20 (got '%s')", to);
        return MCME_EXIT_USAGE;
    }
    if ((size_t)(slash - to) == 4 && strncmp(to, "java", 4) == 0
            && strcmp(slash + 1, "1.20") == 0) {
        return MCME_OK;
    }
    mcme_set_error("convert: %.*s/%s converts in a later entry; only "
                   "a Bedrock world to java/1.20 converts today",
                   (int)(slash - to), to, slash + 1);
    return MCME_EXIT_NOT_IMPLEMENTED;
}

/* The drop counts, one line, singular or plural as they stand — only
   printed when there is something to say. */
static void print_convert_drops(const McmeConvertReport *r)
{
    /* The sentence itself lives in core now (G19): one spelling for
       this line and the window's label, byte-identical to what this
       printer wrote — the table moved whole, this keeps only the
       line (and its newline). */
    char buf[1024];

    if (mcme_convert_drops_text(r, buf, sizeof buf) > 0)
        puts(buf);
}

static int cmd_convert(int argc, char **argv)
{
    const char *to = NULL;
    const char *pos[2];
    int npos = 0;
    for (int i = 0; i < argc; ++i) {
        if (strcmp(argv[i], "--to") == 0) {
            if (i + 1 >= argc) {
                mcme_set_error("convert: --to needs "
                               "<platform>/<version> (e.g. java/1.20)");
                return MCME_EXIT_USAGE;
            }
            if (to != NULL) {
                mcme_set_error("convert: --to given twice");
                return MCME_EXIT_USAGE;
            }
            to = argv[i + 1];
            ++i;
            continue;
        }
        if (npos >= 2) {
            mcme_set_error("convert: unexpected extra argument '%s'",
                           argv[i]);
            return MCME_EXIT_USAGE;
        }
        pos[npos++] = argv[i];
    }
    if (npos != 2) {
        mcme_set_error("convert: usage: convert <in> <out> --to "
                       "<platform>/<version>");
        return MCME_EXIT_USAGE;
    }
    if (to == NULL) {
        mcme_set_error("convert: missing --to <platform>/<version>");
        return MCME_EXIT_USAGE;
    }
    int rc = convert_target(to);
    if (rc != MCME_OK) return rc;

    const char *in = pos[0];
    const char *out = pos[1];
    size_t inlen = strlen(in);
    if (strncmp(out, in, inlen) == 0 && out[inlen] == '/') {
        mcme_set_error("convert: '%s' is inside '%s'; an output "
                       "world is written outside its input", out, in);
        return MCME_EXIT_USAGE;
    }

    McmeWorldInfo info;
    if (!mcme_world_probe(&info, in)) {
        return mcme_fs_exists(in) ? MCME_EXIT_IO : MCME_EXIT_NOT_FOUND;
    }
    if (info.kind == MCME_WORLD_UNKNOWN) {
        mcme_set_error("convert: '%s' is not a Bedrock or Java world",
                       in);
        mcme_world_info_free(&info);
        return MCME_EXIT_NOT_FOUND;
    }
    if (info.kind != MCME_WORLD_BEDROCK) {
        mcme_set_error("convert: '%s' is a %s world; only Bedrock "
                       "input converts today", in,
                       mcme_world_kind_name(info.kind));
        mcme_world_info_free(&info);
        return MCME_EXIT_NOT_IMPLEMENTED;
    }
    mcme_world_info_free(&info);   /* the library re-probes; this was
                                      the CLI's verdict on the pair */

    McmeConvertReport rep;
    McmeWriteRefusal refusal = MCME_WRITE_REFUSAL_NONE;
    if (!mcme_convert_bedrock_java(in, out, &rep, &refusal)) {
        char why[512];
        snprintf(why, sizeof why, "%s", mcme_last_error());
        mcme_set_error("convert: %s", why);
        return refusal_exit(refusal);
    }

    if (g_opts.json) {
        cJSON *root = cJSON_CreateObject();
        cJSON *skipped = cJSON_CreateObject();
        if (root == NULL || skipped == NULL) {
            cJSON_Delete(root);
            cJSON_Delete(skipped);
            return MCME_EXIT_INTERNAL;
        }
        cJSON_AddStringToObject(root, "from", "bedrock");
        cJSON_AddStringToObject(root, "to", "java/1.20");
        cJSON_AddNumberToObject(root, "chunks", (double)rep.chunks);
        cJSON_AddNumberToObject(root, "subchunks", (double)rep.subchunks);
        cJSON_AddNumberToObject(root, "blocks", (double)rep.blocks);
        cJSON_AddNumberToObject(root, "block_entities_carried",
                                (double)rep.block_entities_carried);
        cJSON_AddNumberToObject(root, "ticks_carried",
                                (double)rep.ticks_carried);
        cJSON_AddNumberToObject(root, "biomes_carried",
                                (double)rep.biomes_carried);
        cJSON_AddNumberToObject(root, "heightmaps_carried",
                                (double)rep.heightmaps_carried);
        cJSON_AddNumberToObject(skipped, "actors", (double)rep.actors);
        cJSON_AddNumberToObject(skipped, "biomes",
                                (double)rep.biomes);
        cJSON_AddNumberToObject(skipped, "block_entities",
                                (double)rep.block_entities);
        cJSON_AddNumberToObject(skipped, "chunk_data",
                                (double)rep.chunk_data);
        cJSON_AddNumberToObject(skipped, "entities",
                                (double)rep.entities);
        cJSON_AddNumberToObject(skipped, "heightmaps",
                                (double)rep.heightmaps);
        cJSON_AddNumberToObject(skipped, "items",
                                (double)rep.items);
        cJSON_AddNumberToObject(skipped, "legacy_subchunks",
                                (double)rep.legacy_subchunks);
        cJSON_AddNumberToObject(skipped, "other", (double)rep.other);
        cJSON_AddNumberToObject(skipped, "ticks", (double)rep.ticks);
        cJSON_AddNumberToObject(skipped, "water_positions",
                                (double)rep.water_positions);
        cJSON_AddItemToObject(root, "skipped", skipped);
        print_json(root);
    } else {
        printf("converted %zu chunks (%zu subchunks, %zu blocks) to "
               "java/1.20", rep.chunks, rep.subchunks, rep.blocks);
        if (rep.block_entities_carried > 0) {
            printf(" — %zu block entities carried",
                   rep.block_entities_carried);
        }
        if (rep.ticks_carried > 0) {
            printf(" — %zu tick entries carried", rep.ticks_carried);
        }
        if (rep.biomes_carried > 0) {
            printf(" — %zu biome cells carried", rep.biomes_carried);
        }
        if (rep.heightmaps_carried > 0) {
            printf(" — %zu heightmaps carried",
                   rep.heightmaps_carried);
        }
        putchar('\n');
        print_convert_drops(&rep);
    }
    return MCME_OK;
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

    if (strcmp(cmd, "pack") == 0) {
        if (rest > 0 && strcmp(args[0], "info") == 0) {
            return cmd_pack_info(rest > 1 ? args[1] : NULL);
        }
        mcme_set_error("pack: unknown subcommand '%s' (expected info)", rest > 0 ? args[0] : "");
        return MCME_EXIT_USAGE;
    }

    if (strcmp(cmd, "blockstate") == 0) {
        if (rest != 2) {
            mcme_set_error("blockstate: usage: mcme blockstate "
                           "<pack-dir> <block>");
            return MCME_EXIT_USAGE;
        }
        return cmd_blockstate(args[0], args[1]);
    }

    if (strcmp(cmd, "model") == 0) {
        if (rest != 2) {
            mcme_set_error("model: usage: mcme model "
                           "<pack-dir> <model-ref>");
            return MCME_EXIT_USAGE;
        }
        return cmd_model(args[0], args[1]);
    }

    if (strcmp(cmd, "mesh") == 0) {
        if (rest != 2) {
            mcme_set_error("mesh: usage: mcme mesh "
                           "<pack-dir> <model-ref>");
            return MCME_EXIT_USAGE;
        }
        return cmd_mesh(args[0], args[1]);
    }

    if (strcmp(cmd, "blockmesh") == 0) {
        if (rest != 2) {
            mcme_set_error("blockmesh: usage: mcme blockmesh "
                           "<pack-dir> <block>");
            return MCME_EXIT_USAGE;
        }
        return cmd_blockmesh(args[0], args[1]);
    }

    if (strcmp(cmd, "atlas") == 0) {
        /* `atlas <pack-dir>` or `atlas <pack-dir> --out <png>`. */
        if (rest != 1 && !(rest == 3 && strcmp(args[1], "--out") == 0)) {
            mcme_set_error("atlas: usage: mcme atlas "
                           "<pack-dir> [--out <png>]");
            return MCME_EXIT_USAGE;
        }
        return cmd_atlas(args[0], rest == 3 ? args[2] : NULL);
    }

    if (strcmp(cmd, "atlasmesh") == 0) {
        /* The trailing flags come in value pairs, either order,
           each once: --y <n>, --beside <block>. */
        const char *y_arg = NULL, *beside_arg = NULL;
        bool ok = rest >= 2 && (rest - 2) % 2 == 0;
        for (int i = 2; ok && i + 1 < rest; i += 2) {
            const char **slot = NULL;
            if (strcmp(args[i], "--y") == 0) slot = &y_arg;
            else if (strcmp(args[i], "--beside") == 0)
                slot = &beside_arg;
            if (!slot || *slot) ok = false;  /* unknown, or twice */
            else *slot = args[i + 1];
        }
        if (!ok) {
            mcme_set_error("atlasmesh: usage: mcme atlasmesh "
                           "<pack-dir> <block> [--y <n>] "
                           "[--beside <block>]");
            return MCME_EXIT_USAGE;
        }
        return cmd_atlasmesh(args[0], args[1], y_arg, beside_arg);
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
    if (strcmp(cmd, "convert") == 0) return cmd_convert(rest, args);
    if (strcmp(cmd, "chunks") == 0)  return cmd_chunks(rest, args);
    if (strcmp(cmd, "region") == 0) {
        /* `chunk` and `compact` are matched before the path, like
           `entity set` and `entity del`: a world directory named
           chunk or compact reads as ./chunk, ./compact. Inside chunk,
           `set` is matched the same way. */
        if (rest >= 1 && strcmp(args[0], "compact") == 0) {
            return cmd_region_compact(rest - 1, args + 1);
        }
        if (rest >= 1 && strcmp(args[0], "chunk") == 0) {
            if (rest >= 2 && strcmp(args[1], "set") == 0) {
                return cmd_region_chunk_write(rest - 2, args + 2);
            }
            return cmd_region_chunk(rest - 1, args + 1);
        }
        return cmd_region(rest, args);
    }
    if (strcmp(cmd, "chunk") == 0) {
        if (rest == 0) {
            mcme_set_error("chunk: missing subcommand "
                           "(expected set, get, mesh, submesh, fill, "
                           "prune, stats, height or entity)");
            return MCME_EXIT_USAGE;
        }
        if (strcmp(args[0], "set") == 0) {
            return cmd_chunk_set(rest - 1, args + 1);
        }
        if (strcmp(args[0], "get") == 0) {
            return cmd_chunk_get(rest - 1, args + 1);
        }
        if (strcmp(args[0], "mesh") == 0) {
            return cmd_chunk_mesh(rest - 1, args + 1);
        }
        if (strcmp(args[0], "submesh") == 0) {
            return cmd_chunk_submesh(rest - 1, args + 1);
        }
        if (strcmp(args[0], "fill") == 0) {
            return cmd_chunk_fill(rest - 1, args + 1);
        }
        if (strcmp(args[0], "prune") == 0) {
            return cmd_chunk_prune(rest - 1, args + 1);
        }
        if (strcmp(args[0], "stats") == 0) {
            return cmd_chunk_stats(rest - 1, args + 1);
        }
        if (strcmp(args[0], "height") == 0) {
            return cmd_chunk_height(rest - 1, args + 1);
        }
        if (strcmp(args[0], "entity") == 0) {
            /* set and del are subcommands of entity; a world
               directory that is itself named "set" is read as
               "./set", since the subcommand matches first. */
            if (rest >= 2 && strcmp(args[1], "set") == 0) {
                return cmd_chunk_entity_write(rest - 2, args + 2, true);
            }
            if (rest >= 2 && strcmp(args[1], "del") == 0) {
                return cmd_chunk_entity_write(rest - 2, args + 2, false);
            }
            return cmd_chunk_entity(rest - 1, args + 1);
        }
        mcme_set_error("chunk: unknown subcommand '%s' "
                       "(expected set, get, fill, prune, stats, "
                       "height or entity)", args[0]);
        return MCME_EXIT_USAGE;
    }
    if (strcmp(cmd, "db") == 0)      return cmd_db(rest, args);

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
