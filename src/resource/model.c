#include "mcme/resource.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "mcme/core.h"
#include "cJSON.h"

/*
 * Model folding — one model file with every parent folded in,
 * mirroring the reference reader's `_recursive_load_block_model`
 * rule for rule (decisions.md#D31). The fold is narrower than it
 * looks, and the mirror is exact: only TWO keys travel up the
 * parent chain. `textures` merges per key — the parent's dict is
 * the base, each child key overwrites that one key. `elements`
 * overrides wholesale — whichever model in the chain names it
 * last (nearest the child) contributes the whole list; elements
 * never merge element by element. Every other key — `display`,
 * `gui_light`, `ambientocclusion`, `parent` itself — is
 * discarded, so a model whose only content is `display` folds to
 * the empty object.
 *
 * A file the index does not have folds onto the empty object at
 * ANY level of the chain: the reference's answer for a missing
 * file is `{}`, not an error, so an unknown parent contributes
 * nothing and the child still answers.
 *
 * `#name` texture references stay exactly as written. Substituting
 * them is the mesh layer's job, one below this one; a folded model
 * here is the file's own truth.
 *
 * Where the reference would crash, mcme answers instead, and the
 * differences are recorded in D31: a `parent` that is not a
 * string would raise on its `.split` — mcme reads it as no parent;
 * a chain so deep Python's recursion limit fires — mcme refuses
 * by name past MODEL_DEPTH_CAP, a depth real packs come nowhere
 * near (their chains run a handful deep).
 */

/* How deep one parent chain may run before mcme calls it a cycle.
   The reference's reach is Python's recursion limit (~1000); this
   cap turns that crash into a clean refusal. */
#define MODEL_DEPTH_CAP 64

/* What the index holds: one parsed model file, keyed by the path
   the game writes — everything under models/ minus ".json". */
typedef struct {
    char        *ns;
    char        *path;
    struct cJSON *doc;
} ModelEntry;

struct McmeModelIndex {
    ModelEntry *entries;
    size_t      count;
};

/* ========================================================================
 * Index
 * ======================================================================== */

/* Copy `n` bytes of `s` into a fresh NUL-terminated string, or NULL
   on OOM (which refuses the whole open: a half-built index would
   answer some models and not others). */
static char *ndup(const char *s, size_t n)
{
    char *out = malloc(n + 1);
    if (!out) return NULL;
    memcpy(out, s, n);
    out[n] = '\0';
    return out;
}

/* Parses one model file into the index. A file that does not parse
   is skipped with a warning, the reference skipping it the same way
   (its load catches JSONDecodeError per file). */
static bool index_add_file(McmeModelIndex *index, const char *ns,
                           const char *rel, size_t rel_len,
                           const char *stem, size_t stem_len,
                           const char *path)
{
    char *text = mcme_fs_read_text(path);
    if (!text) {
        MCME_WARN("model: cannot read '%s': %s", path, mcme_last_error());
        return true;
    }
    struct cJSON *doc = cJSON_Parse(text);
    free(text);
    if (!doc) {
        MCME_WARN("model: '%s' is not valid JSON; skipped", path);
        return true;
    }

    ModelEntry *grown = realloc(
        index->entries, (index->count + 1) * sizeof *grown);
    if (!grown) {
        cJSON_Delete(doc);
        return false;
    }
    index->entries = grown;

    /* The key is the path under models/ with ".json" dropped:
       block/banners/blue_2 for models/block/banners/blue_2.json. */
    ModelEntry *e = &index->entries[index->count];
    e->ns   = ndup(ns, strlen(ns));
    e->path = ndup(rel, rel_len);
    e->doc  = doc;
    if (!e->ns || !e->path) {
        free(e->ns);
        free(e->path);
        cJSON_Delete(doc);
        return false;
    }
    if (stem_len) { /* append the file's own name to the key */
        char *full = realloc(e->path, rel_len + stem_len + 1);
        if (!full) {
            free(e->ns);
            free(e->path);
            cJSON_Delete(doc);
            return false;
        }
        memcpy(full + rel_len, stem, stem_len);
        full[rel_len + stem_len] = '\0';
        e->path = full;
    }
    index->count++;
    return true;
}

/* Every *.json under one models directory, nested directories
   included — models live at block/banners/blue_2.json depth, so
   this walk is the reference's recursive glob's own reach. A
   directory that is absent or unreadable contributes nothing —
   the reference's glob returns what it can and never raises, so
   an empty index, not a refusal. */
static bool index_scan_dir(McmeModelIndex *index, const char *ns,
                           const char *dir, const char *rel)
{
    DIR *d = opendir(dir);
    if (!d) return true; /* no models here; not a failure */

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
            char child_rel[1024];
            int m = snprintf(child_rel, sizeof child_rel, "%s%s/",
                             rel, ent->d_name);
            if (m < 0 || (size_t)m >= sizeof child_rel) continue;
            ok = index_scan_dir(index, ns, path, child_rel);
            continue;
        }

        size_t len = strlen(ent->d_name);
        if (len <= 5 || strcmp(ent->d_name + len - 5, ".json") != 0)
            continue;
        ok = index_add_file(index, ns, rel, strlen(rel),
                            ent->d_name, len - 5, path);
    }
    closedir(d);
    return ok;
}

bool mcme_model_index_open(const char *pack_dir, McmeModelIndex **out)
{
    if (!pack_dir || !out) {
        mcme_set_error("model index: null argument");
        return false;
    }

    McmeModelIndex *index = calloc(1, sizeof *index);
    if (!index) {
        mcme_set_error("model index: out of memory");
        return false;
    }

    /* The reference's loader loads nothing from a directory whose
       manifest does not name a pack of format 2 or above; such a
       pack has no models here, every fold the empty object. */
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
                int m = snprintf(ns_dir, sizeof ns_dir, "%s/%s/models",
                                 assets, ent->d_name);
                if (m < 0 || (size_t)m >= sizeof ns_dir) continue;
                ok = index_scan_dir(index, ent->d_name, ns_dir, "");
            }
            closedir(d);
        }
        /* No assets/ at all: an empty index, every load the empty
           model — the reference's glob finds nothing either. */
    }

    if (!ok) {
        mcme_set_error("model index: out of memory");
        mcme_model_index_close(index);
        return false;
    }
    *out = index;
    return true;
}

void mcme_model_index_close(McmeModelIndex *index)
{
    if (!index) return;
    for (size_t i = 0; i < index->count; ++i) {
        free(index->entries[i].ns);
        free(index->entries[i].path);
        cJSON_Delete(index->entries[i].doc);
    }
    free(index->entries);
    free(index);
}

size_t mcme_model_count(const McmeModelIndex *index)
{
    return index ? index->count : 0;
}

bool mcme_model_at(const McmeModelIndex *index, size_t i,
                   const char **ns_out, const char **path_out)
{
    if (!index || i >= index->count) {
        if (ns_out) *ns_out = NULL;
        if (path_out) *path_out = NULL;
        return false;
    }
    if (ns_out) *ns_out = index->entries[i].ns;
    if (path_out) *path_out = index->entries[i].path;
    return true;
}

/* ========================================================================
 * Folding
 * ======================================================================== */

/* The index's file for (ns, path), or NULL — a file the pack does
   not have, which folds onto the empty object. */
static const struct cJSON *find_doc(const McmeModelIndex *index,
                                    const char *ns, const char *path)
{
    for (size_t i = 0; i < index->count; ++i) {
        const ModelEntry *e = &index->entries[i];
        if (strcmp(e->ns, ns) == 0 && strcmp(e->path, path) == 0)
            return e->doc;
    }
    return NULL;
}

/* Sets `key` in `obj` to `val`, replacing any current entry — the
   way a Python dict assignment behaves (a bare AddItemToObject
   would append a SECOND entry under the same key). Takes
   ownership of `val`; on false the caller still owns it. */
static bool set_value(struct cJSON *obj, const char *key,
                      struct cJSON *val)
{
    if (cJSON_GetObjectItemCaseSensitive(obj, key)) {
        if (!cJSON_ReplaceItemInObjectCaseSensitive(obj, key, val))
            return false;
    } else if (!cJSON_AddItemToObject(obj, key, val)) {
        return false;
    }
    return true;
}

/* Folds the model (ns, path) and its whole parent chain into
   `result`, which the caller created empty. The order is the
   reference's own: the parent chain folds FIRST, then this
   model's textures merge per key over it, then this model's
   elements replace the accumulated list wholesale. Every value is
   a deep copy — the result outlives the index and stands alone. */
static bool fold_model(const McmeModelIndex *index, const char *ns,
                       const char *path, struct cJSON *result, int depth)
{
    const struct cJSON *doc = find_doc(index, ns, path);
    if (!doc) return true; /* the reference answers {} here */

    const struct cJSON *parent =
        cJSON_GetObjectItemCaseSensitive(doc, "parent");
    if (cJSON_IsString(parent)) {
        if (depth >= MODEL_DEPTH_CAP) {
            mcme_set_error("model: parent chain for '%.100s:%.100s' is "
                           "deeper than %d (a cycle?)", ns, path,
                           MODEL_DEPTH_CAP);
            return false;
        }
        /* The parent ref is a full model reference: an optional
           `namespace:` prefix, defaulting to minecraft the way the
           reference's own split does. */
        const char *ref  = parent->valuestring;
        const char *colon = strchr(ref, ':');
        char pns[256];
        const char *ppath;
        if (colon) {
            size_t n = (size_t)(colon - ref);
            if (n >= sizeof pns)
                return true; /* an absurd namespace: no such file */
            memcpy(pns, ref, n);
            pns[n] = '\0';
            ppath = colon + 1;
        } else {
            snprintf(pns, sizeof pns, "minecraft");
            ppath = ref;
        }
        if (!fold_model(index, pns, ppath, result, depth + 1))
            return false;
    }
    /* A `parent` that is not a string: the reference's .split
       would raise; mcme reads it as no parent (recorded in D31). */

    /* textures: the parent's dict is the base; each key of this
       model's dict overwrites that one key and only that one. */
    const struct cJSON *textures =
        cJSON_GetObjectItemCaseSensitive(doc, "textures");
    if (cJSON_IsObject(textures)) {
        struct cJSON *have =
            cJSON_GetObjectItemCaseSensitive(result, "textures");
        if (!have) {
            have = cJSON_CreateObject();
            if (!have || !cJSON_AddItemToObject(result, "textures", have)) {
                if (have) cJSON_Delete(have);
                mcme_set_error("model: out of memory");
                return false;
            }
        }
        for (const struct cJSON *kv = textures->child; kv;
             kv = kv->next) {
            struct cJSON *dup = cJSON_Duplicate(kv, true);
            if (!dup) {
                mcme_set_error("model: out of memory");
                return false;
            }
            if (!set_value(have, kv->string ? kv->string : "", dup)) {
                cJSON_Delete(dup);
                mcme_set_error("model: out of memory");
                return false;
            }
        }
    }

    /* elements: wholesale. Whichever model in the chain names it
       last wins the ENTIRE list; an empty list wipes it too, and
       the value is taken at face value — any JSON type, no
       validation at this layer. */
    const struct cJSON *elements =
        cJSON_GetObjectItemCaseSensitive(doc, "elements");
    if (elements) {
        struct cJSON *dup = cJSON_Duplicate(elements, true);
        if (!dup) {
            mcme_set_error("model: out of memory");
            return false;
        }
        if (!set_value(result, "elements", dup)) {
            cJSON_Delete(dup);
            mcme_set_error("model: out of memory");
            return false;
        }
    }

    /* Everything else in the file — display, gui_light,
       ambientocclusion, parent — is discarded: the reference's
       fold carries only textures and elements upward. */
    return true;
}

bool mcme_model_load(const McmeModelIndex *index, const char *namespace,
                     const char *path, struct cJSON **out)
{
    if (!index || !path || !out) {
        mcme_set_error("model load: null argument");
        return false;
    }

    struct cJSON *result = cJSON_CreateObject();
    if (!result) {
        mcme_set_error("model load: out of memory");
        return false;
    }

    if (!fold_model(index, namespace ? namespace : "minecraft", path,
                    result, 0)) {
        cJSON_Delete(result);
        return false; /* the depth cap; mcme_last_error() names it */
    }
    *out = result;
    return true;
}
