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
#include "mcme/region.h"
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
        "  chunk fill <world> <x1> <y1> <z1> <x2> <y2> <z2> <block>\n"
        "                    Set a box of blocks (corners in any\n"
        "                    order), Bedrock or Java. Bedrock:\n"
        "                    subchunks are created, one payload\n"
        "                    write each. Java: one repack per\n"
        "                    section; a chunk not stored refuses\n"
        "                    the whole fill, nothing written.\n"
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

/* ========================================================================
 * Bedrock chunk inventory
 * ======================================================================== */

typedef struct {
    int32_t dimension;
    int32_t x, z;
} ChunkCoord;

typedef struct {
    int32_t dimension;
    size_t  chunks;
} DimensionCount;

typedef struct {
    bool    opened;             /* the store was readable at all */
    size_t  keys;               /* every live key               */
    size_t  chunk_keys;         /* keys that marked a chunk     */
    size_t  chunks;             /* distinct (dimension, x, z)   */
    ChunkCoord     *coords;     /* stb_ds, sorted then deduped  */
    DimensionCount *by_dim;     /* stb_ds, one row per dimension */
} BedrockInventory;

static void bedrock_inventory_free(BedrockInventory *inv)
{
    arrfree(inv->coords);
    arrfree(inv->by_dim);
    memset(inv, 0, sizeof *inv);
}

static int chunk_coord_cmp(const void *pa, const void *pb)
{
    const ChunkCoord *a = (const ChunkCoord *)pa;
    const ChunkCoord *b = (const ChunkCoord *)pb;
    if (a->dimension != b->dimension) return a->dimension < b->dimension ? -1 : 1;
    if (a->x != b->x) return a->x < b->x ? -1 : 1;
    if (a->z != b->z) return a->z < b->z ? -1 : 1;
    return 0;
}

static bool on_bedrock_key(void *ud, const McmeLevelEntry *e)
{
    BedrockInventory *inv = (BedrockInventory *)ud;
    inv->keys++;

    McmeBedrockKey k;
    if (!mcme_bedrock_key_parse(e->key, e->key_len, &k) || !k.is_chunk) {
        /* Level metadata, actors, portallists, and chunk data whose tag does
           not identify a chunk. All normal, all counted so the total adds up. */
        return true;
    }

    inv->chunk_keys++;
    ChunkCoord c = { k.dimension, k.x, k.z };
    arrput(inv->coords, c);
    return true;
}

/*
 * Opens a Bedrock store and counts what is in it.
 *
 * "How many chunks" is not a number the store keeps; it has to be derived, and
 * the derivation is: a chunk is present if any key marks it. Several keys can
 * mark one chunk — a subchunk, a version record — so the coordinates are
 * collected and counted distinct rather than tallied per key.
 *
 * Returns false only when the store cannot be opened. A store that opens is a
 * success even if it holds no chunks, because an empty world is a real thing.
 */
static bool bedrock_inventory(BedrockInventory *inv, const char *db_path)
{
    memset(inv, 0, sizeof *inv);

    McmeLevelDb *db = NULL;
    if (!mcme_leveldb_open(&db, db_path)) return false;
    inv->opened = true;

    mcme_leveldb_iterate(db, on_bedrock_key, inv);
    mcme_leveldb_close(db);

    size_t n = (size_t)arrlen(inv->coords);
    if (n > 1) qsort(inv->coords, n, sizeof *inv->coords, chunk_coord_cmp);

    for (size_t i = 0; i < n; ++i) {
        if (i > 0 && chunk_coord_cmp(&inv->coords[i - 1], &inv->coords[i]) == 0) {
            continue;   /* the same chunk under another tag */
        }
        inv->chunks++;

        size_t rows = (size_t)arrlen(inv->by_dim);
        if (rows > 0 && inv->by_dim[rows - 1].dimension == inv->coords[i].dimension) {
            inv->by_dim[rows - 1].chunks++;
        } else {
            DimensionCount row = { inv->coords[i].dimension, 1 };
            arrput(inv->by_dim, row);
        }
    }

    /* The raw coordinate list is only needed to count; dropping it keeps the
       peak footprint proportional to the store rather than keeping it. */
    arrfree(inv->coords);
    inv->coords = NULL;
    return true;
}

static void print_info_text(const McmeWorldInfo *w, const BedrockInventory *inv)
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

static void print_info_json(const McmeWorldInfo *w, const BedrockInventory *inv)
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
            if (inv != NULL && inv->opened) {
                cJSON_AddNumberToObject(root, "keys", (double)inv->keys);
                cJSON_AddNumberToObject(root, "chunks", (double)inv->chunks);
                cJSON *dims = cJSON_AddArrayToObject(root, "chunks_by_dimension");
                for (size_t i = 0; i < (size_t)arrlen(inv->by_dim); ++i) {
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

    /* Only opened for a store that is actually there. Probing is cheap and
       opening is not, and a Java world has no store to open. */
    BedrockInventory inv;
    memset(&inv, 0, sizeof inv);
    if (info.kind == MCME_WORLD_BEDROCK && info.db_path != NULL) {
        if (!bedrock_inventory(&inv, info.db_path)) {
            /* Keep the exit code at "found": the world was identified, only
               its contents could not be read, and a script that wants to
               distinguish those can ask for --json and read db_error. */
            MCME_WARN("info: cannot read %s: %s", info.db_path, mcme_last_error());
            mcme_clear_error();
        }
    }

    if (g_opts.json) print_info_json(&info, &inv);
    else print_info_text(&info, &inv);

    bedrock_inventory_free(&inv);
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
        chunks = info.chunks;
        if (g_opts.json) {
            cJSON *root = cJSON_CreateObject();
            if (root != NULL) {
                cJSON_AddStringToObject(root, "path", info.path);
                cJSON_AddStringToObject(root, "format", "java");
                cJSON_AddNumberToObject(root, "region_files",
                                        (double)info.region_files);
                cJSON_AddNumberToObject(root, "chunks", (double)chunks);
                print_json(root);
            }
        } else if (!count_only) {
            printf("format:   java (anvil)\n");
            printf("regions:  %zu\n", info.region_files);
            printf("chunks:   %zu\n", chunks);
        }
    } else if (info.kind == MCME_WORLD_BEDROCK && info.db_path != NULL) {
        BedrockInventory inv;
        if (!bedrock_inventory(&inv, info.db_path)) {
            mcme_set_error("chunks: cannot read %s: %s", info.db_path,
                           mcme_last_error());
            bedrock_inventory_free(&inv);
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
                for (size_t i = 0; i < (size_t)arrlen(inv.by_dim); ++i) {
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
            for (size_t i = 0; i < (size_t)arrlen(inv.by_dim); ++i) {
                /* Dimension 0 is the overworld and is normally written as a
                   bare 0; naming it costs one line and saves a lookup. */
                const char *name = inv.by_dim[i].dimension == 0 ? "overworld"
                                  : inv.by_dim[i].dimension == 1 ? "nether"
                                  : inv.by_dim[i].dimension == 2 ? "end"
                                  : NULL;
                if (name != NULL) {
                    printf("  %-10s %zu\n", name, inv.by_dim[i].chunks);
                } else {
                    printf("  %-10d %zu\n", inv.by_dim[i].dimension,
                           inv.by_dim[i].chunks);
                }
            }
        }
        bedrock_inventory_free(&inv);
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
   borrowed for the call that uses it. */
static int parse_block_spec(const char *spec, McmeArena *arena,
                            char *name, size_t name_cap,
                            const McmeTag **states)
{
    const char *open = strchr(spec, '{');
    size_t spec_len = strlen(spec);
    size_t name_len;

    if (open == NULL) {
        name_len = spec_len;
        *states = NULL;
    } else {
        name_len = (size_t)(open - spec);
        if (spec_len == 0 || spec[spec_len - 1] != '}') {
            mcme_set_error("chunk: block '%s' has '{' without a "
                           "closing '}'", spec);
            return MCME_EXIT_USAGE;
        }
        McmeTag *tag = NULL;
        if (!mcme_nbt_parse_snbt(arena, open,
                                 spec_len - name_len, &tag)) {
            char why[256];
            snprintf(why, sizeof why, "%s", mcme_last_error());
            mcme_set_error("chunk: states of block '%s' do not "
                           "parse: %s", spec, why);
            return MCME_EXIT_USAGE;
        }
        if (tag == NULL || tag->type != MCME_TAG_Compound) {
            mcme_set_error("chunk: states of block '%s' are not "
                           "a compound", spec);
            return MCME_EXIT_USAGE;
        }
        *states = tag;
    }

    if (name_len == 0) {
        mcme_set_error("chunk: block '%s' has an empty name", spec);
        return MCME_EXIT_USAGE;
    }
    if (name_len >= name_cap) {
        mcme_set_error("chunk: block name '%s' is too long", spec);
        return MCME_EXIT_USAGE;
    }
    memcpy(name, spec, name_len);
    name[name_len] = '\0';
    return MCME_OK;
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
        long n;
        char *end = NULL;
        if (strcmp(v, "overworld") == 0) {
            n = 0;
        } else if (strcmp(v, "nether") == 0) {
            n = 1;
        } else if (strcmp(v, "end") == 0) {
            n = 2;
        } else {
            n = strtol(v, &end, 10);
            if (end == v || *end != '\0') {
                mcme_set_error("%s: '%s' is not a dimension "
                               "(overworld, nether, end, or a "
                               "number)", verb, v);
                return false;
            }
        }
        if (n < INT32_MIN || n > INT32_MAX) {
            mcme_set_error("%s: dimension '%s' is outside "
                           "the range", verb, v);
            return false;
        }
        *dim = (int32_t)n;
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

/* The Java directory a dimension is stored in: the overworld keeps its
   chunks in plain region/, the nether in DIM-1, the end in DIM1 — the
   game's own layout. Java stores nothing else, so any other number is
   usage rather than a directory mcme would invent. */
static bool java_dimension_dir(int32_t dimension, const char **out_dir)
{
    *out_dir = NULL;
    if (dimension == 0) return true;
    if (dimension == 1) { *out_dir = "DIM-1"; return true; }
    if (dimension == 2) { *out_dir = "DIM1"; return true; }
    mcme_set_error("region: Java keeps the nether in DIM-1 and "
                   "the end in DIM1; there is no directory for "
                   "dimension %d", (int)dimension);
    return false;
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
    if (!java_dimension_dir(dimension, &dim_dir)) {
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
    if (!java_dimension_dir(dimension, &dim_dir)) {
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
    if (!java_dimension_dir(dimension, &dim_dir)) {
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
    if (!java_dimension_dir(dimension, &dim_dir)) {
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
static int java_chunk_doc(const McmeWorldInfo *info, int32_t dimension,
                          long x, long y, long z, McmeArena **arena,
                          McmeNbtDoc *doc, char *file, size_t file_cap)
{
    const char *dim_dir = NULL;
    if (!java_dimension_dir(dimension, &dim_dir)) {
        return MCME_EXIT_USAGE;
    }
    int cx = (int)(x >> 4), cz = (int)(z >> 4);
    int rx, rz;
    mcme_region_of(cx, cz, &rx, &rz);

    if (mcme_region_path(file, file_cap, info->path, dim_dir,
                         rx, rz) == NULL) {
        mcme_set_error("chunk: the path to the chunk of block "
                       "(%ld, %ld, %ld) is too long", x, y, z);
        return MCME_EXIT_INTERNAL;
    }

    if (!mcme_fs_exists(file)) {
        mcme_set_error("chunk: the chunk of block (%ld, %ld, %ld) "
                       "is not stored in this world", x, y, z);
        return MCME_EXIT_NOT_FOUND;
    }
    McmeRegion reg;
    if (!mcme_region_open(&reg, file)) {
        return MCME_EXIT_BAD_FORMAT;   /* open said what and where */
    }
    if (!mcme_region_chunk_present(&reg, cx, cz)) {
        mcme_set_error("chunk: the chunk of block (%ld, %ld, %ld) "
                       "is not stored in this world", x, y, z);
        mcme_region_close(&reg);
        return MCME_EXIT_NOT_FOUND;
    }

    McmeBuffer nbt;
    mcme_buffer_init(&nbt);
    bool read = mcme_region_read_chunk(&reg, cx, cz, NULL, &nbt);
    mcme_region_close(&reg);
    if (!read) {
        char why[256];
        snprintf(why, sizeof why, "%s", mcme_last_error());
        mcme_set_error("chunk: the chunk of block (%ld, %ld, %ld) "
                       "does not read: %s", x, y, z, why);
        mcme_buffer_free(&nbt);
        return MCME_EXIT_BAD_FORMAT;
    }

    *arena = mcme_arena_new();
    if (*arena == NULL) {
        mcme_set_error("chunk: out of memory");
        mcme_buffer_free(&nbt);
        return MCME_EXIT_INTERNAL;
    }
    bool parsed = mcme_nbt_parse(doc, *arena, nbt.data, nbt.len, false);
    mcme_buffer_free(&nbt);
    if (!parsed) {
        char why[256];
        snprintf(why, sizeof why, "%s", mcme_last_error());
        mcme_set_error("chunk: the chunk of block (%ld, %ld, %ld) "
                       "does not decode: %s", x, y, z, why);
        mcme_arena_free(*arena);
        *arena = NULL;
        return MCME_EXIT_BAD_FORMAT;
    }
    return MCME_OK;
}

static int java_get_block(const McmeWorldInfo *info, int32_t dimension,
                          long x, long y, long z, McmeBuffer *spec)
{
    McmeArena *arena = NULL;
    McmeNbtDoc doc;
    char file[MCME_REGION_PATH_MAX];
    int rc = java_chunk_doc(info, dimension, x, y, z, &arena, &doc,
                            file, sizeof file);
    if (rc == MCME_OK
            && !mcme_java_block_spec(doc.root, (int)x, (int)y, (int)z,
                                     spec)) {
        char why[256];
        snprintf(why, sizeof why, "%s", mcme_last_error());
        mcme_set_error("chunk: the chunk of block (%ld, %ld, %ld): %s",
                       x, y, z, why);
        rc = MCME_EXIT_BAD_FORMAT;
    }
    if (arena != NULL) mcme_arena_free(arena);
    return rc;
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
    int rc = java_chunk_doc(info, dimension, x, y, z, &arena, &doc,
                            file, sizeof file);
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
 * One pass over the region files the box's chunks land in: `apply`
 * false runs mcme_java_fill_check on every file and writes
 * nothing, true edits and writes every changed chunk. Two passes
 * in java_fill — check all, then write all — so a chunk the region
 * file does not have refuses the whole fill before any file has
 * changed.
 *
 * The per-file chunk span is the box's chunks crossed with the
 * region's 32x32. Refusals come back as exit codes; the check's
 * own message stands, the fill's is wrapped in "chunk: " the way
 * Bedrock's fill is.
 */
static int java_fill_files(const McmeWorldInfo *info,
                           const char *dim_dir,
                           const McmeBox *box, const char *name,
                           const McmeTag *states, bool apply)
{
    /* The box's chunks: floors of the corners — `>>` floors in C,
       which is what a negative coordinate needs. */
    int x1 = box->x1 < box->x2 ? box->x1 : box->x2;
    int x2 = box->x1 < box->x2 ? box->x2 : box->x1;
    int z1 = box->z1 < box->z2 ? box->z1 : box->z2;
    int z2 = box->z1 < box->z2 ? box->z2 : box->z1;
    int cx1 = x1 >> 4, cx2 = x2 >> 4;
    int cz1 = z1 >> 4, cz2 = z2 >> 4;
    int rx1 = cx1 >> 5, rx2 = cx2 >> 5;
    int rz1 = cz1 >> 5, rz2 = cz2 >> 5;

    for (int rz = rz1; rz <= rz2; ++rz) {
        for (int rx = rx1; rx <= rx2; ++rx) {
            char file[MCME_REGION_PATH_MAX];
            if (mcme_region_path(file, sizeof file, info->path, dim_dir,
                                 rx, rz) == NULL) {
                mcme_set_error("chunk: the path to r.%d.%d is too long",
                               rx, rz);
                return MCME_EXIT_INTERNAL;
            }
            int fx1 = cx1 > rx * 32 ? cx1 : rx * 32;
            int fx2 = cx2 < rx * 32 + 31 ? cx2 : rx * 32 + 31;
            int fz1 = cz1 > rz * 32 ? cz1 : rz * 32;
            int fz2 = cz2 < rz * 32 + 31 ? cz2 : rz * 32 + 31;
            if (fx1 > fx2 || fz1 > fz2) continue;

            McmeWriteRefusal refusal = MCME_WRITE_REFUSAL_NONE;
            if (!apply) {
                if (!mcme_java_fill_check(file, fx1, fx2, fz1, fz2, box,
                                          name, states, &refusal)) {
                    return refusal_exit(refusal);
                }
            } else {
                bool changed = false;
                if (!mcme_java_fill(file, fx1, fx2, fz1, fz2, box, name,
                                    states, (uint32_t)time(NULL),
                                    &changed, &refusal)) {
                    char why[256];
                    snprintf(why, sizeof why, "%s", mcme_last_error());
                    mcme_set_error("chunk: %s", why);
                    return refusal_exit(refusal);
                }
            }
        }
    }
    return MCME_OK;
}

/*
 * The Java half of `chunk fill`: check every region file the box
 * touches first, then edit and write — a chunk the region file
 * does not have refuses the whole fill with nothing written,
 * where Bedrock creates the subchunks it needs. No LOCK: a Java
 * world is region files, no single store to take one on.
 */
static int java_fill(const McmeWorldInfo *info, int32_t dimension,
                     const McmeBox *box, const char *name,
                     const McmeTag *states)
{
    const char *dim_dir = NULL;
    if (!java_dimension_dir(dimension, &dim_dir)) {
        return MCME_EXIT_USAGE;
    }
    int rc = java_fill_files(info, dim_dir, box, name, states, false);
    if (rc == MCME_OK) {
        rc = java_fill_files(info, dim_dir, box, name, states, true);
    }
    return rc;
}

/*
 * The Bedrock half of `chunk get`, the body this verb grew: the
 * subchunk key, the store's read, the layer's palette, the block
 * spec. Reading takes no LOCK — the game can hold the world open
 * while a block is read.
 */
static int bedrock_get_block(const McmeWorldInfo *info,
                             int32_t dimension,
                             long x, long y, long z, McmeBuffer *spec)
{
    uint8_t key[14];
    size_t key_len = 0;
    if (!mcme_bedrock_subchunk_key((int32_t)x, (int32_t)y,
                                   (int32_t)z, dimension,
                                   key, sizeof key, &key_len)) {
        return MCME_EXIT_USAGE;
    }

    McmeBuffer payload;
    mcme_buffer_init(&payload);
    bool deleted = false;
    int rc;
    McmeLevelDb *db = NULL;
    if (!mcme_leveldb_open(&db, info->db_path)) {
        rc = MCME_EXIT_IO;
    } else if (!mcme_leveldb_has(db, key, key_len)) {
        mcme_leveldb_close(db);
        mcme_set_error("chunk: the subchunk of block (%ld, %ld, %ld) "
                       "is not stored in this world", x, y, z);
        rc = MCME_EXIT_NOT_FOUND;
    } else {
        bool got = mcme_leveldb_get(db, key, key_len, &payload, &deleted);
        mcme_leveldb_close(db);
        if (!got) {
            rc = MCME_EXIT_IO;       /* the store's own error stands */
        } else if (deleted) {
            mcme_set_error("chunk: the subchunk of block "
                           "(%ld, %ld, %ld) is deleted in this world",
                           x, y, z);
            rc = MCME_EXIT_NOT_FOUND;
        } else {
            rc = MCME_OK;
        }
    }

    McmeArena *arena = NULL;
    if (rc == MCME_OK) {
        arena = mcme_arena_new();
        if (arena == NULL) {
            mcme_set_error("chunk: out of memory");
            rc = MCME_EXIT_INTERNAL;
        }
    }
    if (rc == MCME_OK) {
        McmeBedrockSubchunk sc;
        if (!mcme_bedrock_subchunk_decode(&sc, arena,
                                          payload.data, payload.len)) {
            char why[256];
            snprintf(why, sizeof why, "%s", mcme_last_error());
            mcme_set_error("chunk: subchunk of block (%ld, %ld, %ld) "
                           "does not decode: %s", x, y, z, why);
            rc = MCME_EXIT_BAD_FORMAT;
        } else if (sc.layer_count == 0) {
            /* The all-air marker: the position holds air. */
            if (!mcme_buffer_append_cstr(spec, "minecraft:air")) {
                rc = MCME_EXIT_INTERNAL;
            }
        } else {
            long lx = x % 16, ly = y % 16, lz = z % 16;
            if (lx < 0) lx += 16;
            if (ly < 0) ly += 16;
            if (lz < 0) lz += 16;
            size_t u = (size_t)lx * 256 + (size_t)lz * 16 + (size_t)ly;
            const McmeBedrockLayer *layer = &sc.layers[0];
            uint16_t idx = layer->indices[u];
            if (idx >= layer->palette_len
                    || layer->palette[idx].root == NULL) {
                mcme_set_error("chunk: the index at block (%ld, %ld, "
                               "%ld) is %u, outside its palette of "
                               "%zu entries", x, y, z, idx,
                               layer->palette_len);
                rc = MCME_EXIT_BAD_FORMAT;
            } else {
                const McmeTag *entry = layer->palette[idx].root;
                if (!mcme_bedrock_block_spec(entry, spec)) {
                    char why[256];
                    snprintf(why, sizeof why, "%s",
                             mcme_last_error());
                    mcme_set_error("chunk: the palette entry at "
                                   "block (%ld, %ld, %ld): %s",
                                   x, y, z, why);
                    rc = MCME_EXIT_BAD_FORMAT;
                }
            }
        }
    }

    if (arena != NULL) mcme_arena_free(arena);
    mcme_buffer_free(&payload);
    return rc;
}

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
    int rc = parse_block_spec(argv[4], arena, name, sizeof name,
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
    int rc;
    if (info.kind == MCME_WORLD_JAVA) {
        /* The chunk document, its palettes, one position. */
        rc = java_get_block(&info, dimension, x, y, z, &spec);
    } else {
        rc = bedrock_get_block(&info, dimension, x, y, z, &spec);
    }

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
    int rc = parse_block_spec(argv[7], arena, name, sizeof name,
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
    const struct { size_t n; const char *one; const char *many; } d[] = {
        { r->legacy_subchunks, "subchunk mcme cannot read",
                               "subchunks mcme cannot read" },
        { r->water_layers,     "water layer",    "water layers" },
        { r->block_entities,   "block entity",   "block entities" },
        { r->entities,         "entity list",    "entity lists" },
        { r->chunk_data,       "other chunk key", "other chunk keys" },
        { r->other,            "other key",      "other keys" },
    };
    bool any = false;
    for (size_t i = 0; i < sizeof d / sizeof d[0]; ++i) {
        if (d[i].n == 0) continue;
        printf("%s%zu %s", any ? ", " : "dropped: ", d[i].n,
               d[i].n == 1 ? d[i].one : d[i].many);
        any = true;
    }
    if (any) putchar('\n');
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
        cJSON_AddNumberToObject(skipped, "block_entities",
                                (double)rep.block_entities);
        cJSON_AddNumberToObject(skipped, "chunk_data",
                                (double)rep.chunk_data);
        cJSON_AddNumberToObject(skipped, "entities",
                                (double)rep.entities);
        cJSON_AddNumberToObject(skipped, "legacy_subchunks",
                                (double)rep.legacy_subchunks);
        cJSON_AddNumberToObject(skipped, "other", (double)rep.other);
        cJSON_AddNumberToObject(skipped, "water_layers",
                                (double)rep.water_layers);
        cJSON_AddItemToObject(root, "skipped", skipped);
        print_json(root);
    } else {
        printf("converted %zu chunks (%zu subchunks, %zu blocks) to "
               "java/1.20\n", rep.chunks, rep.subchunks, rep.blocks);
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
                           "(expected set, get, fill, stats, "
                           "height or entity)");
            return MCME_EXIT_USAGE;
        }
        if (strcmp(args[0], "set") == 0) {
            return cmd_chunk_set(rest - 1, args + 1);
        }
        if (strcmp(args[0], "get") == 0) {
            return cmd_chunk_get(rest - 1, args + 1);
        }
        if (strcmp(args[0], "fill") == 0) {
            return cmd_chunk_fill(rest - 1, args + 1);
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
                       "(expected set, get, fill, stats, height "
                       "or entity)", args[0]);
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
