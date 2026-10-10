#include "mcme/core.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"

/*
 * Config.
 *
 * Plain JSON on disk. The original project gzips a Python pickle to
 * $CONFIG_DIR/<id>.config — a format no other language can read and an unsafe
 * thing to load from disk. See wiki/decisions.md#D4.
 *
 * cJSON owns the tree. We never free subtrees we hand back from get(); they
 * stay valid until the caller cJSON_Delete()s the root.
 */

struct cJSON *mcme_config_load(const char *path)
{
    char *text = mcme_fs_read_text(path);
    if (!text) return NULL;

    struct cJSON *cfg = cJSON_Parse(text);

    if (!cfg) {
        /* cJSON_GetErrorPtr() points INTO the buffer we passed to Parse, so it
           must be copied out before that buffer is freed. Getting this wrong is
           a use-after-free that ASan catches immediately. */
        const char *where = cJSON_GetErrorPtr();
        char snippet[128];
        snippet[0] = '\0';
        if (where && where >= text && where < text + strlen(text)) {
            /* Bound the copy so a pathological file cannot run us off the end. */
            snprintf(snippet, sizeof(snippet), "near: %.80s", where);
        }
        mcme_set_error("config '%s' is not valid JSON%s", path, snippet);
        free(text);
        return NULL;
    }

    free(text);
    return cfg;
}

bool mcme_config_save(const char *path, const struct cJSON *cfg)
{
    if (!path || !cfg) {
        mcme_set_error("config save: null argument");
        return false;
    }

    char *text = cJSON_Print(cfg);
    if (!text) {
        mcme_set_error("config save: could not serialize");
        return false;
    }

    bool ok = mcme_fs_write(path, text, strlen(text));
    cJSON_free(text);
    return ok;
}

bool mcme_config_pretty(const struct cJSON *cfg, char **out_text)
{
    if (!cfg || !out_text) return false;
    char *text = cJSON_Print(cfg);
    if (!text) {
        mcme_set_error("config: could not serialize");
        return false;
    }
    *out_text = text;   /* cJSON_free() releases this */
    return true;
}

/* Walk a dotted path like "options.fov". Returns the container reached so far,
   or NULL if a step was not an object. */
static struct cJSON *walk(struct cJSON *cfg, const char *dotted, bool create)
{
    if (!cfg || !dotted || !*dotted) return cfg;

    const char *p = dotted;
    struct cJSON *cur = cfg;

    for (;;) {
        const char *dot = strchr(p, '.');
        size_t len = dot ? (size_t)(dot - p) : strlen(p);
        if (len == 0) return cur;   /* tolerate "a..b" by stopping cleanly */

        char key[256];
        if (len >= sizeof(key)) return NULL;
        memcpy(key, p, len);
        key[len] = '\0';

        struct cJSON *next = cJSON_GetObjectItemCaseSensitive(cur, key);
        if (!next && create) {
            next = cJSON_CreateObject();
            if (!next) return NULL;
            if (!cJSON_AddItemToObject(cur, key, next)) {
                cJSON_Delete(next);
                return NULL;
            }
        }
        if (!next) return NULL;
        cur = next;

        if (!dot) return cur;
        p = dot + 1;
    }
}

struct cJSON *mcme_config_get(struct cJSON *cfg, const char *dotted)
{
    return walk(cfg, dotted, false);
}

bool mcme_config_set(struct cJSON *cfg, const char *dotted, const char *json_value)
{
    if (!cfg || !dotted || !json_value) {
        mcme_set_error("config set: null argument");
        return false;
    }

    /* Split the path into parent + leaf so we replace the leaf in place. */
    char path_copy[512];
    if (strlen(dotted) >= sizeof(path_copy)) {
        mcme_set_error("config set: path too long");
        return false;
    }
    strcpy(path_copy, dotted);

    char *leaf = strrchr(path_copy, '.');
    struct cJSON *parent = cfg;
    const char *leaf_name = path_copy;

    if (leaf) {
        *leaf = '\0';
        leaf_name = leaf + 1;
        parent = walk(cfg, path_copy, true);
        if (!parent || !cJSON_IsObject(parent)) {
            mcme_set_error("config set: no object at '%s'", path_copy);
            return false;
        }
    }

    struct cJSON *value = cJSON_Parse(json_value);
    if (!value) {
        mcme_set_error("config set: '%s' is not valid JSON", json_value);
        return false;
    }

    /* cJSON_AddItemToObject APPENDS; it does not replace. Without this delete,
       setting the same key twice produces a duplicate JSON member, which
       parses back as whichever one cJSON happens to find first. */
    if (cJSON_GetObjectItemCaseSensitive(parent, leaf_name)) {
        cJSON_DeleteItemFromObjectCaseSensitive(parent, leaf_name);
    }

    if (!cJSON_AddItemToObject(parent, leaf_name, value)) {
        cJSON_Delete(value);
        mcme_set_error("config set: could not attach '%s'", leaf_name);
        return false;
    }
    return true;
}
