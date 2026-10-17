#include "mcme/resource.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mcme/core.h"
#include "cJSON.h"

/*
 * Blockstate selection: which model(s) a block state picks out of a
 * pack, mirroring the reference reader's `_get_model` rule for rule
 * (decisions.md#D30). The quirks below are the reference's own
 * answers, not inventions — the map follows the reference.
 *
 * Selection stops at the model STRING. The parent-merge, the
 * rotations applied at mesh time and the atlas come in the next
 * drop; here an answer is the winning variant's or matching
 * multipart cases' apply, in selection order, with the game's
 * defaults (x 0, y 0, uvlock false) filled in.
 *
 * The two branches differ on purpose, and both differences are
 * mirrored:
 *
 *   variants — a pair whose property the block lacks MATCHES (the
 *     reference defaults the missing property to the pair's own
 *     value), a key no regex pair can be found in matches
 *     everything, and the first key to match wins outright.
 *   multipart — a property the block lacks NEVER matches (the
 *     reference looks it up as None), every matching case
 *     contributes, and a `when` value the reference cannot parse
 *     (a number, a nested object) skips that whole case.
 *
 * A JSON shape the reference would crash on (a non-object `when`, a
 * non-object `apply`, an empty apply list) is a skip here too — the
 * reference's own try/except skips it. Where the reference instead
 * degrades to its missing mesh (a winning apply without a `model`),
 * the selection still happened and is reported: model NULL.
 */

/* What the index holds: one parsed blockstate file. */
typedef struct {
    char        *ns;
    char        *name;
    struct cJSON *doc;
} BlockstateEntry;

struct McmeBlockstateIndex {
    BlockstateEntry *entries;
    size_t           count;
};

/* ========================================================================
 * Index
 * ======================================================================== */

/* Copy `n` bytes of `s` into a fresh NUL-terminated string, or NULL
   on OOM (which refuses the whole open: a half-built index would
   answer some blocks and not others). */
static char *ndup(const char *s, size_t n)
{
    char *out = malloc(n + 1);
    if (!out) return NULL;
    memcpy(out, s, n);
    out[n] = '\0';
    return out;
}

/* Parses one blockstate file into the index. A file that does not
   parse is skipped with a warning, the reference skipping it the
   same way (its load catches JSONDecodeError per file). */
static bool index_add_file(McmeBlockstateIndex *index,
                           const char *ns, const char *path)
{
    char *text = mcme_fs_read_text(path);
    if (!text) {
        MCME_WARN("blockstate: cannot read '%s': %s", path,
                  mcme_last_error());
        return true;
    }
    struct cJSON *doc = cJSON_Parse(text);
    free(text);
    if (!doc) {
        MCME_WARN("blockstate: '%s' is not valid JSON; skipped", path);
        return true;
    }

    /* The name is the file's own stem: <name>.json. */
    const char *stem = strrchr(path, '/');
    stem = stem ? stem + 1 : path;
    size_t stem_len = strlen(stem);
    if (stem_len <= 5) {
        cJSON_Delete(doc);
        return true;
    }

    BlockstateEntry *grown = realloc(
        index->entries, (index->count + 1) * sizeof *grown);
    if (!grown) {
        cJSON_Delete(doc);
        return false;
    }
    index->entries = grown;

    BlockstateEntry *e = &index->entries[index->count];
    e->ns   = ndup(ns, strlen(ns));
    e->name = ndup(stem, stem_len - 5); /* drop ".json" */
    e->doc  = doc;
    if (!e->ns || !e->name) {
        free(e->ns);
        free(e->name);
        cJSON_Delete(doc);
        return false;
    }
    index->count++;
    return true;
}

/* Every *.json directly inside `<pack>/assets/<ns>/blockstates`. A
   directory that is absent or unreadable contributes nothing — the
   reference's glob returns what it can and never raises, so an
   empty index, not a refusal. */
static bool index_scan_namespace(McmeBlockstateIndex *index,
                                 const char *pack_dir, const char *ns)
{
    char dir[1024];
    int n = snprintf(dir, sizeof dir, "%s/assets/%s/blockstates",
                     pack_dir, ns);
    if (n < 0 || (size_t)n >= sizeof dir) {
        mcme_set_error("blockstate: path too long under '%.200s'",
                       pack_dir);
        return false;
    }

    DIR *d = opendir(dir);
    if (!d) return true; /* no blockstates here; not a failure */

    bool ok = true;
    struct dirent *ent;
    while (ok && (ent = readdir(d)) != NULL) {
        size_t len = strlen(ent->d_name);
        if (len <= 5 || strcmp(ent->d_name + len - 5, ".json") != 0)
            continue;
        char path[2048];
        int m = snprintf(path, sizeof path, "%s/%s", dir, ent->d_name);
        if (m < 0 || (size_t)m >= sizeof path) continue; /* too long: skip */
        ok = index_add_file(index, ns, path);
    }
    closedir(d);
    return ok;
}

bool mcme_blockstate_index_open(const char *pack_dir,
                                McmeBlockstateIndex **out)
{
    if (!pack_dir || !out) {
        mcme_set_error("blockstate index: null argument");
        return false;
    }

    McmeBlockstateIndex *index = calloc(1, sizeof *index);
    if (!index) {
        mcme_set_error("blockstate index: out of memory");
        return false;
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
                ok = index_scan_namespace(index, pack_dir, ent->d_name);
            }
            closedir(d);
        }
        /* No assets/ at all: an empty index, every lookup missing —
           the reference's glob finds nothing either. */
    }

    if (!ok) {
        mcme_set_error("blockstate index: out of memory");
        mcme_blockstate_index_close(index);
        return false;
    }
    *out = index;
    return true;
}

void mcme_blockstate_index_close(McmeBlockstateIndex *index)
{
    if (!index) return;
    for (size_t i = 0; i < index->count; ++i) {
        free(index->entries[i].ns);
        free(index->entries[i].name);
        cJSON_Delete(index->entries[i].doc);
    }
    free(index->entries);
    free(index);
}

/* ========================================================================
 * Selection
 * ======================================================================== */

/* The block's state by name, or NULL when it does not carry one. */
static const McmeState *find_state(const McmeState *states, size_t n,
                                   const char *name)
{
    for (size_t i = 0; i < n; ++i)
        if (strcmp(states[i].name, name) == 0) return &states[i];
    return NULL;
}

/* The reference's variant-key regex, `(,name=value)` over
   "[a-z0-9_]+", scanned left to right over ",<key>" — so a pair may
   start at the key's first character (the synthetic comma) or at
   any comma inside it. Text the regex does not pair — junk between
   commas, a `|`-separated second value — is never seen. `cb`
   receives each pair by length (the key stays borrowed); false
   from it stops the scan. */
static bool scan_variant_pairs(const char *key,
                               bool (*cb)(const char *name, size_t nlen,
                                          const char *value, size_t vlen,
                                          void *ctx),
                               void *ctx)
{
    size_t len = strlen(key);
    size_t i = 0;
    while (i < len) {
        if (i > 0 && key[i] != ',') { ++i; continue; }

        size_t p = (i == 0) ? 0 : i + 1; /* past the comma */
        size_t name_start = p;
        while (p < len && ((key[p] >= 'a' && key[p] <= 'z') ||
                           (key[p] >= '0' && key[p] <= '9') ||
                           key[p] == '_')) ++p;
        if (p == name_start || p >= len || key[p] != '=') { ++i; continue; }
        size_t name_len = p - name_start; /* p moves on; the name ends here */

        size_t value_start = ++p;
        while (p < len && ((key[p] >= 'a' && key[p] <= 'z') ||
                           (key[p] >= '0' && key[p] <= '9') ||
                           key[p] == '_')) ++p;
        if (p == value_start) { ++i; continue; }

        if (!cb(key + name_start, name_len,
                key + value_start, p - value_start, ctx))
            return false;
        i = p;
    }
    return true;
}

/* One variant pair against the block's states. A property the block
   lacks matches — the reference defaults it to the pair's value. */
struct PairCtx {
    const McmeState *states;
    size_t           n;
    bool             all_match;
};
static bool variant_pair(const char *name, size_t nlen,
                         const char *value, size_t vlen, void *ctxv)
{
    struct PairCtx *ctx = ctxv;
    char buf[128];
    if (nlen >= sizeof buf) return true; /* not a game property name */
    memcpy(buf, name, nlen);
    buf[nlen] = '\0';

    const McmeState *st = find_state(ctx->states, ctx->n, buf);
    if (st && (strlen(st->value) != vlen ||
               strncmp(st->value, value, vlen) != 0))
        ctx->all_match = false;
    return true; /* keep scanning; all() collects every pair */
}

/* Emits one `apply` value into the answer. Returns false when the
   value is a shape the reference's `_load_blockstate_model` would
   raise on (empty list, non-object, non-string model, non-number
   x/y) — the caller then skips that variant or case, as the
   reference's catch does. */
static bool emit_apply(const cJSON *value, McmeBlockstate *out)
{
    if (cJSON_IsArray(value)) {
        value = value->child; /* first element only */
        if (!value) return false;
    }
    if (!cJSON_IsObject(value)) return false;

    McmeModelRef *grown = realloc(out->models,
                                  (out->count + 1) * sizeof *grown);
    if (!grown) return false;
    out->models = grown;
    McmeModelRef *ref = &out->models[out->count];
    *ref = (McmeModelRef){ NULL, 0, 0, false };

    const cJSON *model = cJSON_GetObjectItemCaseSensitive(value, "model");
    if (model) {
        if (!cJSON_IsString(model)) { /* reference: later .split raises */
            /* undo the reservation: this apply is not emitted */
            return false;
        }
        ref->model = ndup(model->valuestring, strlen(model->valuestring));
        if (!ref->model) return false;
    }
    /* No model: the reference answers its missing mesh; the
       selection still happened, so the entry is kept with model
       NULL. */

    const cJSON *x = cJSON_GetObjectItemCaseSensitive(value, "x");
    if (x) {
        if (!cJSON_IsNumber(x)) { free(ref->model); return false; }
        ref->x = (int32_t)x->valuedouble;
    }
    const cJSON *y = cJSON_GetObjectItemCaseSensitive(value, "y");
    if (y) {
        if (!cJSON_IsNumber(y)) { free(ref->model); return false; }
        ref->y = (int32_t)y->valuedouble;
    }
    ref->uvlock = cJSON_IsTrue(
        cJSON_GetObjectItemCaseSensitive(value, "uvlock"));

    out->count++;
    return true;
}

/* The `variants` branch: first match in file order wins outright. */
static void resolve_variants(const cJSON *variants,
                             const McmeState *states, size_t n,
                             McmeBlockstate *out)
{
    for (const cJSON *item = variants->child; item; item = item->next) {
        const char *key = item->string ? item->string : "";
        bool match;

        if (key[0] == '\0') {
            match = true; /* the "" key matches unconditionally */
        } else {
            struct PairCtx ctx = { states, n, true };
            scan_variant_pairs(key, variant_pair, &ctx);
            match = ctx.all_match;
        }

        if (!match) continue;
        if (!emit_apply(item, out)) continue; /* reference: catch, next */
        out->missing = false;
        return;
    }
    /* No key matched: the reference falls through to its missing
       mesh. multipart is never consulted once variants exists. */
    out->missing = true;
}

/* Multipart `when` evaluation. The reference compares membership:
   the block's property tag must equal one of the value's `|`-split
   parts, and a property the block lacks is None, in no list. A
   value that is neither string nor boolean raises there, which
   skips the whole case — reported as ABORT. */
enum WhenResult { WHEN_MATCH, WHEN_NOMATCH, WHEN_ABORT };

static enum WhenResult one_arm(const cJSON *arm, const McmeState *states,
                               size_t n)
{
    if (!cJSON_IsObject(arm)) return WHEN_ABORT; /* .items() raises */
    for (const cJSON *pair = arm->child; pair; pair = pair->next) {
        const McmeState *st = find_state(states, n, pair->string);
        bool hit = false;

        if (cJSON_IsString(pair)) {
            if (st) {
                const char *v = pair->valuestring;
                for (;;) {
                    const char *bar = strchr(v, '|');
                    size_t part = bar ? (size_t)(bar - v) : strlen(v);
                    if (part == strlen(st->value) &&
                        strncmp(v, st->value, part) == 0) { hit = true; break; }
                    if (!bar) break;
                    v = bar + 1;
                }
            }
            /* absent property: None is in no list */
        } else if (cJSON_IsBool(pair)) {
            const char *want = cJSON_IsTrue(pair) ? "true" : "false";
            hit = st && strcmp(st->value, want) == 0;
        } else {
            return WHEN_ABORT; /* parse_state_val raises */
        }
        if (!hit) return WHEN_NOMATCH;
    }
    return WHEN_MATCH; /* an empty arm matches, all([]) is true */
}

/* Evaluates a case's `when`, or says the case has none. */
static enum WhenResult eval_when(const cJSON *when,
                                 const McmeState *states, size_t n)
{
    if (!cJSON_IsObject(when)) return WHEN_ABORT; /* .items() raises */

    const cJSON *or_ = cJSON_GetObjectItemCaseSensitive(when, "OR");
    if (or_) {
        if (!cJSON_IsArray(or_)) return WHEN_ABORT;
        for (const cJSON *arm = or_->child; arm; arm = arm->next) {
            enum WhenResult r = one_arm(arm, states, n);
            if (r == WHEN_MATCH) return WHEN_MATCH;  /* any() stops */
            if (r == WHEN_ABORT) return WHEN_ABORT;
        }
        return WHEN_NOMATCH;
    }

    const cJSON *and_ = cJSON_GetObjectItemCaseSensitive(when, "AND");
    if (and_) {
        if (!cJSON_IsArray(and_)) return WHEN_ABORT;
        for (const cJSON *arm = and_->child; arm; arm = arm->next) {
            enum WhenResult r = one_arm(arm, states, n);
            if (r != WHEN_MATCH) return r; /* all() stops on false;
                                              an abort raises through */
        }
        return WHEN_MATCH;
    }

    return one_arm(when, states, n); /* the plain prop=value form */
}

/* The `multipart` branch: every matching case contributes, in file
   order; all failing is an empty selection, NOT missing. */
static void resolve_multipart(const cJSON *cases, const McmeState *states,
                              size_t n, McmeBlockstate *out)
{
    out->missing = false;
    if (!cJSON_IsArray(cases)) return; /* garbage: contributes nothing */

    for (const cJSON *item = cases->child; item; item = item->next) {
        if (!cJSON_IsObject(item)) continue; /* case not an object */

        const cJSON *when = cJSON_GetObjectItemCaseSensitive(item, "when");
        if (when) {
            enum WhenResult r = eval_when(when, states, n);
            if (r != WHEN_MATCH) continue; /* NOMATCH or the reference's
                                              caught exception */
        }

        const cJSON *apply = cJSON_GetObjectItemCaseSensitive(item,
                                                              "apply");
        if (!apply) continue; /* a matched case with nothing to add */
        if (!emit_apply(apply, out)) continue; /* reference: catch */
    }
}

bool mcme_blockstate_lookup(const McmeBlockstateIndex *index,
                            const char *namespace, const char *base_name,
                            const McmeState *states, size_t state_count,
                            McmeBlockstate *out)
{
    if (!index || !namespace || !base_name || !out) {
        mcme_set_error("blockstate lookup: null argument");
        return false;
    }

    *out = (McmeBlockstate){ NULL, 0, true };

    const struct cJSON *doc = NULL;
    for (size_t i = 0; i < index->count; ++i) {
        const BlockstateEntry *e = &index->entries[i];
        if (strcmp(e->ns, namespace) == 0 &&
            strcmp(e->name, base_name) == 0) {
            doc = e->doc;
            break;
        }
    }
    if (!doc) return true; /* no file: missing, the renderer's cue */

    const struct cJSON *variants =
        cJSON_GetObjectItemCaseSensitive(doc, "variants");
    if (variants && cJSON_IsObject(variants)) {
        resolve_variants(variants, states, state_count, out);
        return true;
    }

    const struct cJSON *multipart =
        cJSON_GetObjectItemCaseSensitive(doc, "multipart");
    if (multipart) {
        resolve_multipart(multipart, states, state_count, out);
        return true;
    }

    /* A file holding neither key is the reference's missing mesh. */
    return true;
}

void mcme_blockstate_free(McmeBlockstate *state)
{
    if (!state) return;
    for (size_t i = 0; i < state->count; ++i)
        free(state->models[i].model);
    free(state->models);
    *state = (McmeBlockstate){ NULL, 0, false };
}
