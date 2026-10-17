#include "mcme/resource.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "mcme/core.h"
#include "stb_image.h"

/*
 * The texture table — every PNG under a pack's `assets/<ns>/`,
 * keyed the way the game references it, mirroring the reference
 * loader's own scan (decisions.md#D32).
 *
 * The keys are (namespace, path under textures/ with `.png`
 * dropped). Directories that are not block textures — colormap,
 * effect, environment, font, gui, map, mob_effect, particle — are
 * skipped whole, the reference excluding those image groups from
 * its glob's results. A reference the table does not hold
 * resolves to the missing image; mcme names its sentinel
 * "missing_no", the same word the reference seeds into its own
 * table, and the pins canonicalise the reference's absolute path
 * (which points into its package) to that token.
 *
 * Transparency is the loader's test, quirks included: an image is
 * transparent only when it is a PNG of colour type RGBA carrying
 * at least one alpha value below 255. Gray-plus-alpha, palette
 * and plain RGB images are opaque by definition — the reference
 * reads the Pillow mode and only "RGBA" earns an alpha scan.
 * An image stb cannot read is opaque with a warning here, where
 * the reference's Pillow would raise and its whole load die; the
 * difference is recorded in D32.
 */

/* The image directories that are not block textures. */
static const char *const k_useless_groups[] = {
    "colormap", "effect", "environment", "font",
    "gui", "map", "mob_effect", "particle",
};

typedef struct {
    char *ns;
    char *rel;  /* the path under textures/, ".png" dropped */
    char *path; /* the absolute file path                    */
    bool  transparent;
} TextureEntry;

struct McmeTextureIndex {
    TextureEntry *entries;
    size_t        count;
};

/* Copy `n` bytes of `s` into a fresh NUL-terminated string, or NULL
   on OOM (which refuses the whole open: a half-built table would
   resolve some textures and not others). */
static char *ndup(const char *s, size_t n)
{
    char *out = malloc(n + 1);
    if (!out) return NULL;
    memcpy(out, s, n);
    out[n] = '\0';
    return out;
}

static bool group_is_useless(const char *name)
{
    for (size_t i = 0; i < sizeof k_useless_groups / sizeof *k_useless_groups;
         ++i)
        if (strcmp(name, k_useless_groups[i]) == 0) return true;
    return false;
}

/* The reference's transparency test, file for file: only an RGBA
   PNG can be transparent, and only if some alpha is below 255. */
static bool image_is_transparent(const char *path)
{
    int w = 0, h = 0, comp = 0;
    if (!stbi_info(path, &w, &h, &comp)) {
        MCME_WARN("texture: cannot read '%s' as an image; treated as "
                  "opaque", path);
        return false;
    }
    if (comp != 4) return false; /* not RGBA: opaque by definition */

    int out_w = 0, out_h = 0, out_comp = 0;
    unsigned char *pixels =
        stbi_load(path, &out_w, &out_h, &out_comp, 4);
    if (!pixels) {
        MCME_WARN("texture: cannot decode '%s'; treated as opaque", path);
        return false;
    }
    bool transparent = false;
    size_t count = (size_t)out_w * (size_t)out_h;
    for (size_t i = 0; i < count; ++i)
        if (pixels[i * 4 + 3] != 255) { transparent = true; break; }
    stbi_image_free(pixels);
    return transparent;
}

/* Parses one PNG into the table. */
static bool index_add_file(McmeTextureIndex *index, const char *ns,
                           const char *rel, size_t rel_len,
                           const char *stem, size_t stem_len,
                           const char *path)
{
    TextureEntry *grown = realloc(
        index->entries, (index->count + 1) * sizeof *grown);
    if (!grown) return false;
    index->entries = grown;

    TextureEntry *e = &index->entries[index->count];
    e->ns   = ndup(ns, strlen(ns));
    e->rel  = ndup(rel, rel_len);
    e->path = ndup(path, strlen(path));
    if (!e->ns || !e->rel || !e->path) {
        free(e->ns);
        free(e->rel);
        free(e->path);
        return false;
    }
    if (stem_len) { /* append the file's own name to the key */
        char *full = realloc(e->rel, rel_len + stem_len + 1);
        if (!full) {
            free(e->ns);
            free(e->rel);
            free(e->path);
            return false;
        }
        memcpy(full + rel_len, stem, stem_len);
        full[rel_len + stem_len] = '\0';
        e->rel = full;
    }
    e->transparent = image_is_transparent(path);
    index->count++;
    return true;
}

/* Every *.png under one textures directory, nested directories
   included, skipping the useless image groups whole. */
static bool index_scan_dir(McmeTextureIndex *index, const char *ns,
                           const char *dir, const char *rel)
{
    DIR *d = opendir(dir);
    if (!d) return true; /* no textures here; not a failure */

    bool ok = true;
    struct dirent *ent;
    while (ok && (ent = readdir(d)) != NULL) {
        if (ent->d_name[0] == '.') continue;

        char path[2048];
        int n = snprintf(path, sizeof path, "%s/%s", dir, ent->d_name);
        if (n < 0 || (size_t)n >= sizeof path) continue; /* too long: skip */

        struct stat st;
        if (stat(path, &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) {
            /* The reference keys each image by its FIRST directory
               under textures/ and drops the useless groups; a
               skipped group is a skipped subtree. */
            if (rel[0] == '\0' && group_is_useless(ent->d_name)) continue;
            char child_rel[1024];
            int m = snprintf(child_rel, sizeof child_rel, "%s%s/",
                             rel, ent->d_name);
            if (m < 0 || (size_t)m >= sizeof child_rel) continue;
            ok = index_scan_dir(index, ns, path, child_rel);
            continue;
        }

        size_t len = strlen(ent->d_name);
        if (len <= 4 || strcmp(ent->d_name + len - 4, ".png") != 0)
            continue;
        ok = index_add_file(index, ns, rel, strlen(rel),
                            ent->d_name, len - 4, path);
    }
    closedir(d);
    return ok;
}

bool mcme_texture_index_open(const char *pack_dir, McmeTextureIndex **out)
{
    if (!pack_dir || !out) {
        mcme_set_error("texture index: null argument");
        return false;
    }

    McmeTextureIndex *index = calloc(1, sizeof *index);
    if (!index) {
        mcme_set_error("texture index: out of memory");
        return false;
    }

    /* The reference's loader gate: a directory the manifest does
       not name a pack of format 2 or above holds nothing here. */
    if (!mcme_pack_loads_assets(pack_dir)) {
        *out = index;
        return true;
    }

    /* Every directory under assets/ is a namespace. */
    char assets[1024];
    int n = snprintf(assets, sizeof assets, "%s/assets", pack_dir);
    bool ok = n > 0 && (size_t)n < sizeof assets;
    if (ok) {
        DIR *d = opendir(assets);
        if (d) {
            struct dirent *ent;
            while (ok && (ent = readdir(d)) != NULL) {
                if (ent->d_name[0] == '.') continue;
                char ns_dir[2048];
                int m = snprintf(ns_dir, sizeof ns_dir, "%s/%s/textures",
                                 assets, ent->d_name);
                if (m < 0 || (size_t)m >= sizeof ns_dir) continue;
                ok = index_scan_dir(index, ent->d_name, ns_dir, "");
            }
            closedir(d);
        }
        /* No assets/ at all: an empty table, every reference the
           missing image — the reference's glob finds nothing. */
    }

    if (!ok) {
        mcme_set_error("texture index: out of memory");
        mcme_texture_index_close(index);
        return false;
    }
    *out = index;
    return true;
}

void mcme_texture_index_close(McmeTextureIndex *index)
{
    if (!index) return;
    for (size_t i = 0; i < index->count; ++i) {
        free(index->entries[i].ns);
        free(index->entries[i].rel);
        free(index->entries[i].path);
    }
    free(index->entries);
    free(index);
}

const char *mcme_texture_path(const McmeTextureIndex *index,
                              const char *namespace, const char *rel)
{
    if (index && namespace && rel) {
        for (size_t i = 0; i < index->count; ++i) {
            const TextureEntry *e = &index->entries[i];
            if (strcmp(e->ns, namespace) == 0 &&
                strcmp(e->rel, rel) == 0)
                return e->path;
        }
    }
    /* The reference's table carries the same sentinel under
       ("minecraft", "missing_no"); an unknown reference lands on
       the missing image either way. */
    return "missing_no";
}

bool mcme_texture_transparent(const McmeTextureIndex *index,
                              const char *path)
{
    if (!index || !path) return false;
    for (size_t i = 0; i < index->count; ++i)
        if (strcmp(index->entries[i].path, path) == 0)
            return index->entries[i].transparent;
    return false; /* the missing image is opaque here */
}

size_t mcme_texture_count(const McmeTextureIndex *index)
{
    return index ? index->count : 0;
}

const char *mcme_texture_at(const McmeTextureIndex *index, size_t i)
{
    if (!index || i >= index->count) return NULL;
    return index->entries[i].path;
}
