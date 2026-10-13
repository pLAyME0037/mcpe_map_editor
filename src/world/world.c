/*
 * mcme world detection and metadata. See include/mcme/world.h.
 */

#include "mcme/world.h"

#include "mcme/compress.h"
#include "mcme/nbt.h"
#include "mcme/region.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

/* ========================================================================
 * Small helpers
 * ======================================================================== */

const char *mcme_world_kind_name(McmeWorldKind k)
{
    switch (k) {
    case MCME_WORLD_JAVA:    return "java";
    case MCME_WORLD_BEDROCK: return "bedrock";
    case MCME_WORLD_UNKNOWN: break;
    }
    return "unknown";
}

const char *mcme_level_dat_frame_name(McmeLevelDatFrame f)
{
    switch (f) {
    case MCME_LEVELDAT_NONE:          return "none";
    case MCME_LEVELDAT_GZIP:          return "gzip";
    case MCME_LEVELDAT_HEADER_PLAIN:  return "header+plain";
    case MCME_LEVELDAT_HEADER_GZIP:   return "header+gzip";
    case MCME_LEVELDAT_BARE_NBT:      return "bare-nbt";
    }
    return "none";
}

static bool path_is_dir(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

static bool path_is_file(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

/* Joins a directory and one child with a fixed-size stack buffer. Returns NULL
   if the result would not fit, so callers can treat a long path as "skip this"
   rather than silently truncating it. */
static const char *join2(char *buf, size_t cap, const char *a, const char *b)
{
    int n = snprintf(buf, cap, "%s/%s", a, b);
    if (n < 0 || (size_t)n >= cap) return NULL;
    return buf;
}

static char *dup_range(const char *data, size_t len)
{
    char *s = (char *)malloc(len + 1);
    if (s == NULL) return NULL;
    if (len > 0) memcpy(s, data, len);
    s[len] = '\0';
    return s;
}

/* ========================================================================
 * NBT navigation
 * ======================================================================== */

static const McmeTag *child_of(const McmeTag *compound, const char *name)
{
    if (compound == NULL || compound->type != MCME_TAG_Compound) return NULL;
    for (size_t i = 0; i < compound->compound.len; ++i) {
        const McmeStr *k = &compound->compound.entries[i].name;
        if (k->len == strlen(name) && memcmp(k->data, name, k->len) == 0) {
            return compound->compound.entries[i].value;
        }
    }
    return NULL;
}

/* Fetches a string child into *out (newly allocated), if it is there and is a
   string. Returns whether it was found; *out stays NULL when it was not. */
static bool get_string(const McmeTag *c, const char *name, char **out)
{
    const McmeTag *v = child_of(c, name);
    if (v == NULL || v->type != MCME_TAG_String) return false;
    *out = dup_range(v->string.data, v->string.len);
    return *out != NULL;
}

static bool get_int(const McmeTag *c, const char *name, int32_t *out)
{
    const McmeTag *v = child_of(c, name);
    if (v == NULL) return false;
    switch (v->type) {
    case MCME_TAG_Int:   *out = v->as_int;   return true;
    case MCME_TAG_Short: *out = v->as_short; return true;
    case MCME_TAG_Byte:  *out = v->as_byte;  return true;
    default: return false;
    }
}

static bool get_long(const McmeTag *c, const char *name, int64_t *out)
{
    const McmeTag *v = child_of(c, name);
    if (v == NULL) return false;
    switch (v->type) {
    case MCME_TAG_Long:  *out = v->as_long;  return true;
    case MCME_TAG_Int:   *out = v->as_int;   return true;
    default: return false;
    }
}

/* ========================================================================
 * level.dat
 * ======================================================================== */

/*
 * Reads `<dir>/level.dat` and fills in the frame plus every field it can find.
 *
 * Returns false only on a real I/O failure. A level.dat that exists but cannot
 * be understood is reported as MCME_LEVELDAT_NONE with the error left in
 * mcme_last_error for the caller to log — the world may still be usable.
 */
/*
 * Reads `<path>` in whatever frame it wears and parses it into `doc`
 * from `arena` — the framing rules, extracted so every reader of a
 * level.dat agrees on them (the frames are named on McmeLevelDatFrame).
 * `*frame` says which wrapper was found, and is set even when the NBT
 * itself does not parse, because a caller reporting a mis-detected
 * world wants the wrapper either way.
 */
bool mcme_level_dat_parse(McmeNbtDoc *doc, McmeArena *arena,
                          const char *path, McmeLevelDatFrame *frame)
{
    if (doc == NULL || arena == NULL || path == NULL || frame == NULL) {
        mcme_set_error("level.dat: null argument");
        return false;
    }
    *frame = MCME_LEVELDAT_NONE;

    void *raw = NULL;
    size_t raw_len = 0;
    if (!mcme_fs_read(path, &raw, &raw_len)) {
        return false;
    }

    const uint8_t *bytes = (const uint8_t *)raw;
    const uint8_t *body = NULL;
    size_t body_len = 0;
    bool little_endian = false;
    bool framed = false;

    if (raw_len >= 2 && bytes[0] == 0x1f && bytes[1] == 0x8b) {
        /* No header, gzip straight away. Only Java does this. */
        *frame = MCME_LEVELDAT_GZIP;
        little_endian = false;
    } else if (raw_len >= 8) {
        /* Bedrock: [storage version, 4 LE][payload length, 4 LE]. The length
           field is advisory and is often wrong in the wild, so it is not
           trusted — the remaining bytes are simply the payload. */
        *frame = MCME_LEVELDAT_HEADER_PLAIN;
        body = bytes + 8;
        body_len = raw_len - 8;
        little_endian = true;
        framed = true;
    } else {
        /* Too short for either container. Treat the whole thing as NBT and let
           the parser say what is wrong with it. */
        *frame = MCME_LEVELDAT_BARE_NBT;
        body = bytes;
        body_len = raw_len;
        little_endian = false;
    }

    McmeBuffer inflated;
    mcme_buffer_init(&inflated);

    if (*frame == MCME_LEVELDAT_GZIP) {
        if (!mcme_decompress(MCME_COMPRESS_GZIP, bytes, raw_len, &inflated,
                             MCME_DECOMPRESS_MAX)) {
            free(raw);
            return false;
        }
        body = inflated.data;
        body_len = inflated.len;
    } else if (framed && body_len >= 2 && body[0] == 0x1f && body[1] == 0x8b) {
        /* Bedrock gzips its payload sometimes (Minecraft does, amulet does
           not), so sniff inside the header-framed body too. */
        if (!mcme_decompress(MCME_COMPRESS_GZIP, body, body_len, &inflated,
                             MCME_DECOMPRESS_MAX)) {
            mcme_buffer_free(&inflated);
            free(raw);
            return false;
        }
        *frame = MCME_LEVELDAT_HEADER_GZIP;
        body = inflated.data;
        body_len = inflated.len;
    }

    bool parsed = mcme_nbt_parse(doc, arena, body, body_len, little_endian);
    mcme_buffer_free(&inflated);
    free(raw);
    return parsed;
}

static bool read_level_dat(McmeWorldInfo *info, const char *dir)
{
    char path[4096];
    if (join2(path, sizeof path, dir, "level.dat") == NULL) return false;

    McmeArena *arena = mcme_arena_new();
    if (arena == NULL) return false;

    McmeNbtDoc doc;
    bool parsed = mcme_level_dat_parse(&doc, arena, path, &info->frame);
    if (parsed) {
        /* Java nests everything under a `Data` compound; Bedrock normally does
           too, but a freshly generated Bedrock level.dat (and amulet's own
           writer) puts the fields at the root. Look in both. */
        const McmeTag *root = doc.root;
        const McmeTag *data = child_of(root, "Data");
        const McmeTag *scope = (data != NULL && data->type == MCME_TAG_Compound)
                             ? data : root;

        get_string(scope, "LevelName", &info->name);
        get_int(scope, "DataVersion", &info->data_version);
        get_int(scope, "GameType", &info->game_type);
        info->has_game_type = child_of(scope, "GameType") != NULL;

        if (get_int(scope, "SpawnX", &info->spawn_x)) {
            get_int(scope, "SpawnY", &info->spawn_y);
            get_int(scope, "SpawnZ", &info->spawn_z);
            info->has_spawn = true;
        }

        int64_t played = 0;
        if (get_long(scope, "LastPlayed", &played)) {
            /* Java stores milliseconds; Bedrock stores seconds. Anything past
               year 5138 in seconds is milliseconds. */
            info->last_played = (played > 100000000000LL) ? played / 1000 : played;
        }

        /* Version reporting differs wildly between editions, so each gets its
           own shape and neither pretends to be the other. */
        const McmeTag *v = child_of(scope, "Version");
        if (v != NULL && v->type == MCME_TAG_String) {
            info->version = dup_range(v->string.data, v->string.len);
        } else if (v != NULL && v->type == MCME_TAG_Compound) {
            get_string(v, "Name", &info->version);
        } else {
            /* Bedrock: lastOpenedWithVersion is a list of ints, most
               significant first. Print every element rather than guessing at a
               depth: [1, 10, 0, 0, 0] is the raw tuple the world recorded, and
               truncating it to "1.10.0.0" would silently invent precision. */
            const McmeTag *lv = child_of(scope, "lastOpenedWithVersion");
            if (lv == NULL) lv = child_of(scope, "LastOpenedWithVersion");
            if (lv != NULL && lv->type == MCME_TAG_List && lv->list.len > 0) {
                char buf[128];
                size_t at = 0;
                /* Eight components is far past any real game version; the cap
                   keeps a hostile list from running off the end of `buf`. */
                size_t parts = lv->list.len < 8 ? lv->list.len : 8;
                for (size_t i = 0; i < parts; ++i) {
                    const McmeTag *n = &lv->list.items[i];
                    int32_t part = 0;
                    if (n->type == MCME_TAG_Int) part = n->as_int;
                    else if (n->type == MCME_TAG_Byte) part = n->as_byte;
                    int w = snprintf(buf + at, sizeof buf - at,
                                     i == 0 ? "%d" : ".%d", part);
                    if (w < 0 || (size_t)w >= sizeof buf - at) break;
                    at += (size_t)w;
                }
                if (at > 0) info->version = dup_range(buf, at);
            } else {
                int32_t dv = 0;
                if (get_int(scope, "StorageVersion", &dv)) {
                    char buf[32];
                    int w = snprintf(buf, sizeof buf, "storage %d", dv);
                    if (w > 0) info->version = dup_range(buf, (size_t)w);
                }
            }
        }
    }

    mcme_arena_free(arena);
    return parsed;
}

/* ========================================================================
 * Java region inventory
 * ======================================================================== */

/* Counts .mca files in `<dir>/region` and the chunks inside them. */
static void scan_regions(McmeWorldInfo *info, const char *dir)
{
    char region_dir[4096];
    if (join2(region_dir, sizeof region_dir, dir, "region") == NULL) return;

    DIR *d = opendir(region_dir);
    if (d == NULL) return;

    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        size_t len = strlen(e->d_name);
        if (len <= 5 || strcmp(e->d_name + len - 4, ".mca") != 0) continue;

        char path[4096];
        int n = snprintf(path, sizeof path, "%s/%s", region_dir, e->d_name);
        if (n < 0 || (size_t)n >= sizeof path) continue;
        if (!path_is_file(path)) continue;

        McmeRegion reg;
        if (!mcme_region_open(&reg, path)) {
            /* A corrupt region must not abort the whole scan: report it and
               keep going, which is what a map editor's user expects when one
               file out of hundreds is damaged. */
            MCME_WARN("world: %s: %s", e->d_name, mcme_last_error());
            mcme_clear_error();
            continue;
        }

        info->region_files++;
        info->chunks += mcme_region_chunk_count(&reg);
        mcme_region_close(&reg);
    }
    closedir(d);
}

/* ========================================================================
 * Probe
 * ======================================================================== */

void mcme_world_info_free(McmeWorldInfo *info)
{
    if (info == NULL) return;
    free(info->path);
    free(info->name);
    free(info->version);
    free(info->db_path);
    memset(info, 0, sizeof *info);
}

bool mcme_world_probe(McmeWorldInfo *out, const char *path)
{
    if (out == NULL) {
        mcme_set_error("world: NULL output");
        return false;
    }
    memset(out, 0, sizeof *out);
    if (path == NULL) {
        mcme_set_error("world: NULL path");
        return false;
    }
    out->path = strdup(path);
    if (out->path == NULL) {
        mcme_set_error("world: out of memory");
        return false;
    }

    if (!path_is_dir(path)) {
        mcme_set_error("world: '%s' is not a directory", path);
        /* Release before returning false so the contract is unambiguous: a
           failed probe leaves nothing to free, and callers cannot leak by
           forgetting to call mcme_world_info_free on the error path. */
        mcme_world_info_free(out);
        return false;
    }

    char marker[4096];
    char level_dat[4096];
    bool has_db = false;
    bool has_region = false;
    bool has_session_lock = false;

    if (join2(marker, sizeof marker, path, "db") != NULL) {
        has_db = path_is_dir(marker);
    }
    if (join2(marker, sizeof marker, path, "region") != NULL) {
        has_region = path_is_dir(marker);
    }
    if (join2(marker, sizeof marker, path, "session.lock") != NULL) {
        has_session_lock = path_is_file(marker);
    }
    out->has_db = has_db;
    if (has_db) {
        /* Re-join rather than reuse `marker`, which the session.lock probe
           above has already overwritten. */
        if (join2(marker, sizeof marker, path, "db") != NULL) {
            out->db_path = strdup(marker);
            if (out->db_path == NULL) {
                mcme_set_error("world: out of memory");
                mcme_world_info_free(out);
                return false;
            }
        }
    }
    if (join2(level_dat, sizeof level_dat, path, "level.dat") != NULL) {
        out->has_level_dat = path_is_file(level_dat);
    }

    /* Markers decide. db/ wins outright because a LevelDB store is unambiguous
       and Java has nothing like it. */
    if (has_db) {
        out->kind = MCME_WORLD_BEDROCK;
    } else if (has_region || has_session_lock) {
        out->kind = MCME_WORLD_JAVA;
    } else if (out->has_level_dat) {
        /* No markers, so fall back to how level.dat is framed. */
        if (!read_level_dat(out, path)) {
            MCME_WARN("world: %s: cannot read level.dat: %s",
                      path, mcme_last_error());
            mcme_clear_error();
        }
        if (out->frame == MCME_LEVELDAT_HEADER_PLAIN ||
            out->frame == MCME_LEVELDAT_HEADER_GZIP) {
            out->kind = MCME_WORLD_BEDROCK;
        } else if (out->frame == MCME_LEVELDAT_GZIP) {
            out->kind = MCME_WORLD_JAVA;
        }
    }

    if (out->kind == MCME_WORLD_UNKNOWN) {
        return true;   /* not an error: the caller reports "not a world" */
    }

    if (out->has_level_dat && out->name == NULL && out->frame == MCME_LEVELDAT_NONE) {
        if (!read_level_dat(out, path)) {
            MCME_WARN("world: %s: cannot read level.dat: %s",
                      path, mcme_last_error());
            mcme_clear_error();
        }
    }

    /* levelname.txt is a Bedrock file and is what the game shows in its world
       list. Prefer the NBT LevelName, which is what the world itself records,
       and fall back to the text file. */
    if (out->name == NULL) {
        char txt[4096];
        if (join2(txt, sizeof txt, path, "levelname.txt") != NULL) {
            char *s = mcme_fs_read_text(txt);
            if (s != NULL) {
                size_t len = strlen(s);
                while (len > 0 && (s[len - 1] == '\n' || s[len - 1] == '\r')) {
                    s[--len] = '\0';
                }
                out->name = (len > 0) ? s : NULL;
                if (out->name == NULL) free(s);
            } else {
                mcme_clear_error();
            }
        }
    }

    if (out->kind == MCME_WORLD_JAVA) {
        scan_regions(out, path);
    }

    return true;
}

/* The Java directory a dimension is stored in: the overworld keeps its
   chunks in plain region/, the nether in DIM-1, the end in DIM1 — the
   game's own layout. Java stores nothing else, so any other number is
   usage rather than a directory mcme would invent. The rule lives here
   beside the probe that decides the world is Java's, so every verb —
   fill, get, set, prune — speaks one layout. */
bool mcme_java_dimension_dir(int32_t dimension, const char **out_dir)
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
