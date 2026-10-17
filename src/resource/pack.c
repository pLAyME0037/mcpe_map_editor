#include "mcme/resource.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mcme/core.h"
#include "cJSON.h"

/*
 * A Java resource pack's manifest.
 *
 * The reference the project translates against decides a directory is a pack
 * by one test — `pack.mcmeta` is a file — and reads three things from it: the
 * integer `pack.pack_format`, the string `pack.description`, and whether the
 * file parsed. This layer reads exactly those and stops (decisions.md#D29).
 *
 * A pack whose manifest is absent, unparseable, not the game's object shape,
 * or lacks an integer `pack.pack_format` is refused by name. That mirrors the
 * reference: it only sets its valid flag when `pack_format` parsed as an int,
 * so a manifest without one is not a pack. A missing `description` is the
 * empty string, not a refusal — the reference defaults it and keeps going.
 *
 * The caller owns the returned record and every string inside it; one
 * mcme_pack_free releases all of it.
 */

/* Join `dir` and `leaf` into a fixed buffer. Returns false if the path would
   not fit — a refusal, not a truncation, because a truncated manifest path
   would silently read the wrong file. */
static bool join_path(char *out, size_t cap, const char *dir, const char *leaf)
{
    int n = snprintf(out, cap, "%s/%s", dir, leaf);
    if (n < 0 || (size_t)n >= cap) {
        mcme_set_error("pack: path too long under '%.200s'", dir);
        return false;
    }
    return true;
}

/* Heap copy of a cJSON string, or NULL when the item is absent or not a
   string. `fallback` is used for an absent-or-wrong-type value the reference
   tolerates (the description). */
static char *dup_string(const struct cJSON *item, const char *fallback)
{
    const char *s = (item && cJSON_IsString(item) && item->valuestring)
                        ? item->valuestring : fallback;
    size_t n = strlen(s) + 1;
    char *out = malloc(n);
    if (!out) {
        mcme_set_error("pack: out of memory");
        return NULL;
    }
    memcpy(out, s, n);
    return out;
}

bool mcme_pack_open(const char *dir, McmePack **out)
{
    if (!dir || !out) {
        mcme_set_error("pack open: null argument");
        return false;
    }

    char manifest[1024];
    if (!join_path(manifest, sizeof(manifest), dir, "pack.mcmeta")) return false;

    char *text = mcme_fs_read_text(manifest);
    if (!text) {
        /* mcme_fs_read_text already set the error; name the condition. */
        mcme_set_error("pack: no readable manifest at '%s'", manifest);
        return false;
    }

    struct cJSON *root = cJSON_Parse(text);
    free(text);
    if (!root) {
        mcme_set_error("pack: '%s' is not valid JSON", manifest);
        return false;
    }

    /* The manifest is `{ "pack": { ... } }`. Anything else is not the game's
       shape, so it is not a pack. */
    const struct cJSON *pack = cJSON_GetObjectItemCaseSensitive(root, "pack");
    if (!cJSON_IsObject(pack)) {
        mcme_set_error("pack: '%s' has no 'pack' object", manifest);
        cJSON_Delete(root);
        return false;
    }

    /* `pack_format` is the lynchpin: an integer, present. The reference only
       marks a pack valid once this parses, so its absence is a refusal. */
    const struct cJSON *fmt = cJSON_GetObjectItemCaseSensitive(pack, "pack_format");
    if (!cJSON_IsNumber(fmt)) {
        mcme_set_error("pack: '%s' has no integer pack.pack_format", manifest);
        cJSON_Delete(root);
        return false;
    }
    double raw = fmt->valuedouble;
    int32_t format = (int32_t)raw;

    McmePack *p = calloc(1, sizeof(*p));
    if (!p) {
        mcme_set_error("pack: out of memory");
        cJSON_Delete(root);
        return false;
    }
    p->format = format;

    p->description = dup_string(
        cJSON_GetObjectItemCaseSensitive(pack, "description"), "");
    if (!p->description) {
        free(p);
        cJSON_Delete(root);
        return false;
    }

    {
        size_t n = strlen(dir) + 1;
        char *r = malloc(n);
        if (!r) {
            mcme_set_error("pack: out of memory");
            free(p->description);
            free(p);
            cJSON_Delete(root);
            return false;
        }
        memcpy(r, dir, n);
        p->root = r;
    }

    /* The icon is optional; its presence or absence never refuses a pack. */
    char icon[1024];
    p->has_icon = join_path(icon, sizeof(icon), dir, "pack.png")
                  && mcme_fs_exists(icon);

    cJSON_Delete(root);
    *out = p;
    return true;
}

void mcme_pack_free(McmePack *pack)
{
    if (!pack) return;
    free(pack->description);
    free(pack->root);
    free(pack);
}
