/*
 * `mcme pack`: a Java resource pack's identity, read from its manifest.
 *
 * Phase 5 opens on the thing every later layer stands on: a directory of
 * `assets/` is a resource pack, and a pack names itself. The reference the
 * project translates against (the Java `JavaResourcePack` reader) decides a
 * pack is valid by one test — the manifest `pack.mcmeta` is a file — and
 * reads three things from it: the integer `pack_format`, the free-text
 * `description`, and whether the parse held. Nothing else about a pack is
 * needed until the model and atlas layers arrive, so this layer reads exactly
 * that and stops (decisions.md#D29).
 *
 * The manifest is the game's own JSON. A pack whose manifest is absent,
 * unparseable, or lacks the `pack.pack_format` integer is not a pack: the
 * loader refuses it by name rather than shipping a half-read record the
 * caller cannot trust.
 */

#ifndef MCME_RESOURCE_H
#define MCME_RESOURCE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * What one pack's manifest held. Every string is a heap copy owned by the
 * record; `mcme_pack_free` releases the record and them together. A valid
 * pack always has `format >= 0`; `description` is empty (never NULL) when the
 * manifest omits it, because the reference defaults a missing description to
 * the empty string rather than refusing the pack.
 */
typedef struct {
    int32_t format;       /* pack.mcmeta pack.pack_format, or -1 if absent  */
    char   *description;  /* pack.pack.description, "" when not given       */
    bool    has_icon;     /* pack.png sits beside the manifest              */
    char   *root;         /* the directory the manifest was read from       */
} McmePack;

/*
 * Reads `<dir>/pack.mcmeta` and fills `*out` with a pack the caller must
 * release with `mcme_pack_free`. Returns false and leaves `*out` untouched
 * when the directory holds no readable manifest, when the manifest is not
 * the game's JSON object, or when `pack.pack_format` is absent or not an
 * integer — the three conditions under which the reference calls a
 * directory not-a-pack. On false, mcme_last_error() says which.
 */
bool mcme_pack_open(const char *dir, McmePack **out);

/* Releases a pack and every string it owns. NULL is a no-op. */
void mcme_pack_free(McmePack *pack);

/* ========================================================================
 * Blockstates — which model a block state selects (D30)
 *
 * Every pack holds `assets/<ns>/blockstates/<name>.json`: for one
 * block and its states, which model the renderer draws and how it
 * is turned. This layer is the selection half — it reports exactly
 * what the winning variant or matching multipart case carried (the
 * `model` string as written, `x`, `y`, `uvlock`) and stops before
 * any model file is opened; the parent-merge and the atlas come in
 * the next drop.
 *
 * The selection rules mirror the reference reader's, quirks
 * included, because the map follows the reference:
 *
 *   - `variants` are consulted in file order and the FIRST match
 *     wins. A variant key is a list of `name=value` pairs of
 *     `[a-z0-9_]+` characters, each pair needing a comma before it
 *     (the reference's regex, `,(name)=(value)`); text the regex
 *     does not pair — a `|`-separated second value, a key with no
 *     comma — is never seen. A key of "" matches unconditionally at
 *     its position in the order. A pair whose property the block
 *     does NOT have matches anyway (the reference defaults the
 *     missing property to the pair's own value); a malformed key
 *     the regex pairs nothing from matches everything.
 *   - `multipart` cases are consulted in order; every matching
 *     case contributes its `apply`. A `when` property the block
 *     does NOT have never matches (the opposite of the variants
 *     rule). `OR` needs one arm to match, `AND` needs all of them;
 *     an empty arm matches; a `when` value is a `|`-separated list
 *     or a JSON boolean; a value of any other type skips the whole
 *     case, as does a `when`/arm/apply of the wrong JSON type.
 *   - An `apply` that is a list takes its FIRST element, and one
 *     without a `model` answers a null model — the reference
 *     degrades such a selection to its missing mesh rather than
 *     carrying on to the next variant or case.
 *   - No blockstate file, a file holding neither key, or a
 *     `variants` block whose keys all fail to match answers
 *     `missing` — the renderer's job then is the missing texture.
 *     A `multipart` block whose cases all fail is NOT missing: it
 *     selects nothing, an empty model list.
 *
 * Selection is an answer, never a refusal: a block the pack has no
 * state file for is `missing: true` with an empty list, and the
 * caller decides what that means.
 */

/* One property of the queried block — a Java blockstate property,
   always a string. Non-string properties are out of the Java
   domain and refused by the CLI before they reach a lookup. */
typedef struct {
    const char *name;
    const char *value;
} McmeState;

/* One selected model: exactly what the winning apply carried,
   with the game's defaults filled in. `model` is the file's own
   string, namespace and all, and is NULL when the apply carried
   none; it is a heap copy owned by the answer. */
typedef struct {
    char    *model;
    int32_t  x;      /* degrees as written, 0 when absent        */
    int32_t  y;      /* degrees as written, 0 when absent        */
    bool     uvlock; /* false when absent                        */
} McmeModelRef;

/* The pack's answer for one block state. */
typedef struct {
    McmeModelRef *models; /* in variant- or case-selection order */
    size_t        count;
    bool          missing; /* no selection was possible          */
} McmeBlockstate;

/* A pack's blockstate index: every blockstate JSON under
   `assets/<ns>/blockstates/`, parsed at open, by (namespace, name).
   Opaque to callers. */
typedef struct McmeBlockstateIndex McmeBlockstateIndex;

/*
 * Parses every blockstate file under `<pack_dir>/assets/<ns>/`. A
 * file that does not parse is skipped with a warning, the reference
 * skipping it too (its loader catches the JSON error per file). A
 * directory that is absent or unreadable contributes nothing — the
 * reference's glob never raises, so an empty index, not a refusal;
 * the CLI names a missing pack directory itself. Returns false only
 * on a null argument or OOM, with mcme_last_error() saying which.
 */
bool mcme_blockstate_index_open(const char *pack_dir,
                                McmeBlockstateIndex **out);

/* Releases the index. NULL is a no-op. */
void mcme_blockstate_index_close(McmeBlockstateIndex *index);

/*
 * Answers which model(s) `<namespace>:<base_name>` selects under
 * `states`, filling `*out` (which the caller releases with
 * mcme_blockstate_free). Returns false only on a null argument; a
 * block the index does not know is `missing`, not a failure.
 */
bool mcme_blockstate_lookup(const McmeBlockstateIndex *index,
                            const char *namespace, const char *base_name,
                            const McmeState *states, size_t state_count,
                            McmeBlockstate *out);

/* Releases an answer and every string it owns. NULL is a no-op. */
void mcme_blockstate_free(McmeBlockstate *state);

#endif /* MCME_RESOURCE_H */
