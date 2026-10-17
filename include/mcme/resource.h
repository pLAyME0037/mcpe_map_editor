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

#endif /* MCME_RESOURCE_H */
