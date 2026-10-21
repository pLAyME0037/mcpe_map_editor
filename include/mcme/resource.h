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
   always a string. A store's own typed state (Bedrock, or a
   Java entry that carried one) arrives with a NULL value:
   present, and matching nothing — the reference compares a
   property's py_data against the variant's text, and a typed
   value is never equal to text (D43). */
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
 * What the index holds — the model index's enumeration, one layer
 * down (D53): how many blockstate files it parsed, and the
 * (namespace, name) of the i-th in its own walk order (per
 * namespace directory, readdir, the fs's own). The reference
 * exposes no enumeration surface, so nothing pins this order but
 * the walk itself. Null index: count 0, every `at` false; an
 * out-of-range `at` answers false and names NULL in both outs.
 */
size_t mcme_blockstate_count(const McmeBlockstateIndex *index);

bool mcme_blockstate_at(const McmeBlockstateIndex *index, size_t i,
                        const char **ns_out, const char **name_out);

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

/*
 * What a selection WALK answered (D33):
 *
 *   MCME_SELECT_MISSING — no file, no usable key, or variants that
 *     matched nothing or refused every match: the reference falls
 *     through to its missing block.
 *   MCME_SELECT_VARIANTS — one apply won: the callback accepted it
 *     and the walk stopped, the way the reference's variants loop
 *     returns at the first load that does not raise.
 *   MCME_SELECT_MULTIPART — every matching case went to the
 *     callback, refusals skipped, possibly none: the meshes merge
 *     (an empty merge is an answer here, not a missing block).
 */
typedef enum {
    MCME_SELECT_MISSING = 0,
    MCME_SELECT_VARIANTS,
    MCME_SELECT_MULTIPART
} McmeSelectKind;

/*
 * Hands one winning apply to the walk. False means "this apply did
 * not produce an answer" — the reference's per-apply try/except
 * catching a load or rotation that raised: variants carry on to the
 * next matching key, multipart skips the case. `ref` is borrowed
 * and valid only for the call; a NULL `ref->model` is the
 * reference's missing mesh, accepted as it stands.
 */
typedef bool (*McmeApplyFn)(const McmeModelRef *ref, void *ud);

/*
 * The reference's `_get_model` walk, one level above the lookup:
 * mcme_blockstate_lookup answers the first matching apply outright,
 * while block-level meshing must accept-or-skip each one in
 * selection order, because a first match whose model refuses to
 * mesh does not end the reference's search. The matching rules (and
 * their quirks) are the lookup's own — one implementation. Returns
 * false only on a null argument; `*kind` always answers.
 */
bool mcme_blockstate_select(const McmeBlockstateIndex *index,
                            const char *namespace, const char *base_name,
                            const McmeState *states, size_t state_count,
                            McmeApplyFn take, void *ud,
                            McmeSelectKind *kind);

/*
 * Does the reference's loader load ANYTHING from this directory?
 * Its whole load — textures, blockstates, models alike — sits
 * under one guard: the pack must be valid (the manifest parsed and
 * named a pack) and its format must be at least 2, the first
 * format that carried assets. A directory that fails either loads
 * nothing there, and this gate makes mcme's three indexes agree:
 * an empty index, every answer the empty one (mcme's manifest
 * reader is stricter than the reference's on malformed manifests,
 * recorded in D29; the gate uses mcme's own verdict).
 */
bool mcme_pack_loads_assets(const char *pack_dir);

/* ========================================================================
 * Models — one file, every parent folded in (D31)
 *
 * A blockstate's selection names a model; the model itself is one
 * JSON file under `assets/<ns>/models/<path>.json` that may name a
 * `parent` above it. This layer reads the file the reference's
 * `_recursive_load_block_model` returns: the parent chain folded
 * into the child. The fold is narrower than it looks — the
 * reference propagates only TWO keys up the chain, and the mirror
 * is exact (decisions.md#D31):
 *
 *   - `textures` merges per KEY: the parent's dict is the base,
 *     each child key overwrites that one key. Keys the child does
 *     not name survive from the parent.
 *   - `elements` overrides WHOLESALE: whichever model in the chain
 *     names it last (nearest the child) contributes the entire
 *     list; elements never merge element-by-element.
 *   - Every other key — `display`, `gui_light`,
 *     `ambientocclusion`, `parent` itself — is DISCARDED. A model
 *     whose only content is `display` folds to the empty object.
 *
 * A reference that cannot find the file (or a parent in the chain)
 * answers the empty object, not an error: the same file-missing →
 * `{}` rule applies at every level, so a child with an unknown
 * parent folds onto nothing rather than failing.
 *
 * A `#name` texture stays exactly as written. Substitution walks
 * the textures dict at mesh time in the reference, one layer below
 * this one; a merged model here is the file's own truth.
 */

/* The `struct cJSON` type is declared by third_party/cjson/cJSON.h.
   We only forward-declare it here so this header does not depend
   on cJSON. */
struct cJSON;

/* A pack's model index: every model JSON under `assets/<ns>/models/`
   (nested directories included), parsed at open, by (namespace,
   path). Opaque to callers. */
typedef struct McmeModelIndex McmeModelIndex;

/*
 * Parses every model file under `<pack_dir>/assets/<ns>/models/`,
 * nested paths included, the reference's recursive glob's own
 * reach. A file that does not parse is skipped with a warning, the
 * reference catching the JSON error per file the same way. A
 * directory that is absent or unreadable contributes nothing — the
 * empty index answers every query with the empty model, not a
 * refusal; the CLI names a missing pack directory itself. Returns
 * false only on a null argument or OOM, with mcme_last_error()
 * saying which.
 */
bool mcme_model_index_open(const char *pack_dir, McmeModelIndex **out);

/* Releases the index. NULL is a no-op. */
void mcme_model_index_close(McmeModelIndex *index);

/*
 * What the index holds: how many models it parsed, and the
 * (namespace, path) of the i-th — the index's own walk order (per
 * namespace directory, recursive readdir, the fs's own). The
 * reference exposes no enumeration surface, so nothing pins this
 * order but the walk itself; a caller that wants a display order
 * sorts the answers. Null index: count 0, every `at` false. An
 * out-of-range `at` answers false and names NULL in both outs.
 */
size_t mcme_model_count(const McmeModelIndex *index);

bool mcme_model_at(const McmeModelIndex *index, size_t i,
                   const char **ns_out, const char **path_out);

/*
 * Answers the merged model for `<namespace>:<path>`, or for a bare
 * `<path>` (the caller passes NULL for the namespace to take the
 * reference's default, `minecraft`). The result is a heap
 * `struct cJSON` object the caller releases with cJSON_Delete —
 * the empty object when neither the model nor any parent in its
 * chain is in the index, exactly the reference's answer. Returns
 * false only on a null argument or OOM; a model the index does not
 * know is the empty object, not a failure.
 */
bool mcme_model_load(const McmeModelIndex *index, const char *namespace,
                     const char *path, struct cJSON **out);

/* ========================================================================
 * Textures — the pack's images, resolved (D32)
 *
 * A mesh names its textures by reference ("#all", "block/stone"),
 * and the reference resolves them against its texture table: every
 * PNG under `assets/<ns>/textures/` keyed by the path under
 * textures/ with `.png` dropped, first directory groups excluded —
 * colormap, effect, environment, font, gui, map, mob_effect,
 * particle are not block textures and the loader skips them. A
 * reference the table does not hold resolves to the missing
 * image — mcme names its sentinel "missing_no" (the reference's
 * own key word for it; its file lives inside its package, and the
 * pins canonicalise the two namings to one token).
 *
 * Transparency is the loader's own test, quirks included: an image
 * is transparent only if it is a PNG of colour type RGBA with at
 * least one alpha value below 255. Every other image — RGB, gray,
 * palette — is opaque by definition, whatever it draws.
 */

typedef struct McmeTextureIndex McmeTextureIndex;

/*
 * Parses the texture table of `<pack_dir>`: every PNG under
 * `assets/<ns>/textures/`, nested directories included, the
 * useless image groups skipped, each image's transparency read
 * once. The loader's gate applies: a directory the manifest does
 * not name a pack of format 2 or above contributes nothing.
 * Returns false only on a null argument or OOM.
 */
bool mcme_texture_index_open(const char *pack_dir,
                             McmeTextureIndex **out);

/* Releases the index. NULL is a no-op. */
void mcme_texture_index_close(McmeTextureIndex *index);

/*
 * The pack's file for one texture — namespace and the path under
 * textures/ with `.png` dropped — or "missing_no" when the pack
 * holds no such image. The returned string is owned by the index.
 */
const char *mcme_texture_path(const McmeTextureIndex *index,
                              const char *namespace, const char *rel);

/*
 * Is the image at this path partly transparent? `path` is what
 * mcme_texture_path returned. false for "missing_no" — and for
 * an image the index holds as opaque. Only an RGBA PNG can answer
 * true, the reference's own rule.
 */
bool mcme_texture_transparent(const McmeTextureIndex *index,
                              const char *path);

/*
 * The table's files, one by one — the index walks them in
 * directory order, which is the filesystem's own. `mcme_texture_at`
 * answers the file's absolute path, owned by the index. The atlas
 * layer is the first consumer: it sorts what it is handed, because
 * its layout depends on input order (decisions.md#D34).
 */
size_t mcme_texture_count(const McmeTextureIndex *index);
const char *mcme_texture_at(const McmeTextureIndex *index, size_t i);

/* ========================================================================
 * Images and the atlas — many small files on one page (D34)
 *
 * The editor's own textureatlas module packs every texture a pack
 * ships into one square RGBA page and answers where each one landed
 * as texture coordinates, which is what the renderer wants back.
 * mcme mirrors it rule for rule (decisions.md#D34):
 *
 *   - Every frame loads whole; the textures sort by perimeter
 *     (2w + 2h) in non-increasing order, STABLY — equal perimeters
 *     keep the input order, and a pack of 16x16 textures is one
 *     long tie, so the input order decides the layout.
 *   - The page starts at max(max width, max height, the smallest
 *     power of two at least ceil(sqrt(total pixels))) and DOUBLES
 *     until the packer takes everything.
 *   - The packer is a binary-split region tree: the first free
 *     region that holds an item takes it at its top-left, and the
 *     region splits into the strip below and the strip right.
 *   - Each texture's bounds are (x/size, y/size, (x+w)/size,
 *     (y + min(h, w))/size) — the top edge using the SMALLER of
 *     the frame's sides, the module's own quirk, not an invention.
 */

typedef struct {
    int      width, height;
    uint8_t *rgba;  /* width*height*4 bytes, row-major, owned */
} McmeImage;

/* Loads a PNG as RGBA (grey, palette and RGB images expand the way
   Pillow's paste into an RGBA canvas does). Refuses by name on a
   file stb cannot read — where the reference's Pillow raise kills
   the whole atlas call, recorded in D34. */
bool mcme_image_load(const char *path, McmeImage *out);

/* Releases an image's pixels; NULL is a no-op. */
void mcme_image_free(McmeImage *image);

/* Writes an image back out as a PNG. */
bool mcme_image_write_png(const char *path, const McmeImage *image);

typedef struct {
    int32_t   size;      /* the page is size x size                */
    size_t    count;
    char    **names;     /* count entries, the input paths         */
    double   *bounds;    /* count*4: u0 v0 u1 v1 per name          */
    McmeImage page;      /* the assembled page                     */
} McmeAtlas;

/*
 * Packs `count` image paths into one square page, the editor's own
 * page: the missing image is packed FIRST (the manager seeds its
 * table with it before the pack's files, so the app's page carries
 * it too — D35), then `paths` in the order given (a tie in the
 * perimeter sort keeps it); the answer's `names` mirrors that back,
 * the missing image under the word "missing_no". The images all
 * load up front, exactly as the reference's frames do, and one
 * that will not load refuses the whole atlas by name.
 *
 * Answers `*out` (released with mcme_atlas_free); false only on a
 * null argument, an unloadable image or an exhausted heap.
 */
bool mcme_atlas_build(const char *const *paths, size_t count,
                      McmeAtlas *out);

/* Releases an atlas and everything it owns. NULL is a no-op. */
void mcme_atlas_free(McmeAtlas *atlas);

/* ========================================================================
 * Meshes — one model, faced and verticed (D32)
 *
 * The reference's `_load_block_model` turns a folded model into a
 * BlockMesh: for each element box, for each named face, four
 * vertices with their texture coordinates, grouped by CULL
 * direction (the face's `cullface`, or the shared "none" bucket
 * when it has none), a triangle index table, a per-face texture
 * index, the resolved texture list, and the block's transparency.
 * This layer mirrors that builder rule for rule (decisions.md#D32):
 *
 *   - A model with textures but no elements answers the missing
 *     BLOCK — a unit cube of the missing image — not an empty
 *     mesh; an empty `elements` list counts as no elements (the
 *     reference tests truthiness). A model with neither answers
 *     the empty mesh.
 *   - Face textures resolve through the `#key` chain with a loop
 *     check; a `{"sprite": ...}` value passes through; a
 *     reference the textures dict does not hold answers itself.
 *   - Texture entries deduplicate by their path under textures/
 *     NAMESPACE-BLIND: `block/x` and `other:block/x` share one
 *     slot, the first face to use them winning.
 *   - Face UV defaults per direction from the element box when
 *     the face gives none; a face `rotation` remaps the UV
 *     corners; element `rotation` (origin, axis, angle, the
 *     multi-axis overrides, rescale) turns the vertices.
 *   - A full-size unrotated element with six or more faces turns
 *     the block translucent and its faces are checked for
 *     opacity; six opaque faces make the block opaque. Here the
 *     reference reads its transparency table — and its read of a
 *     texture the table does not hold RAISES, which mcme mirrors
 *     as a refusal: a full block whose texture the pack lacks
 *     does not mesh.
 *
 * Rotation of the whole mesh by the blockstate selection's x/y
 * (and the reference's ignored uvlock), the merge of multipart's
 * many meshes and the block-level loader that composes them all
 * are layer D33's; see the declarations below.
 */

typedef enum {
    MCME_TRANSPARENCY_OPAQUE      = 0, /* full block, opaque textures  */
    MCME_TRANSPARENCY_TRANSLUCENT = 1, /* full block, see-through     */
    MCME_TRANSPARENCY_PARTIAL     = 2  /* not a full block            */
} McmeTransparency;

/* The seven buckets a face can land in — the six cull directions
   plus "none" for faces the game culls nowhere. */
typedef enum {
    MCME_CULL_DOWN = 0, MCME_CULL_UP, MCME_CULL_NORTH, MCME_CULL_EAST,
    MCME_CULL_SOUTH,    MCME_CULL_WEST, MCME_CULL_NONE
} McmeCullDir;

/* One bucket's flat arrays, the reference's own layout: three
   numbers per vertex for positions, texture coordinates and tint;
   six indices per face (two triangles into the vertex stream);
   two indices per face into the texture list. */
typedef struct {
    double  *verts;
    double  *tverts;
    double  *tint_verts;
    uint32_t *faces;
    uint32_t *texture_indexes;
    size_t    vert_count; /* verts holds 3 per vertex            */
    size_t    face_count; /* faces holds 6, texture_indexes 2    */
} McmeMeshBucket;

typedef struct {
    McmeMeshBucket   buckets[7];       /* indexed by McmeCullDir   */
    bool             present[7];       /* the reference omits empty
                                          buckets; so does mcme    */
    char           **textures;         /* resolved paths, first-use
                                          order, owned            */
    size_t           texture_count;
    McmeTransparency transparency;
} McmeMesh;

/*
 * Builds the mesh for `<namespace>:<path>` (NULL namespace takes
 * the minecraft default), filling `*out` — released with
 * mcme_mesh_free. Returns false, with mcme_last_error() saying
 * which, only where the reference itself raises and its callers
 * degrade the whole selection: a full block whose texture the pack
 * does not hold, a face whose texture is neither a string nor a
 * `{sprite: ...}` object, a self-referencing `#key` chain, a
 * malformed UV, or an element whose box, rotation or faces are
 * shapes numpy would refuse. A model the pack does not know is
 * the empty mesh — an answer, not a failure.
 */
bool mcme_mesh_build(const McmeModelIndex *models,
                     const McmeTextureIndex *textures,
                     const char *namespace, const char *path,
                     McmeMesh *out);

/*
 * mcme_mesh_build with the block's tint in hand (D44): a face
 * whose model JSON declares `tintindex` — presence, whatever the
 * value — takes the tint on all four of its vertices, every other
 * face (1, 1, 1), exactly the reference's per-face rule. `tint`
 * NULL reads as (1, 1, 1); mcme_mesh_build is this call without
 * one.
 */
bool mcme_mesh_build_tinted(const McmeModelIndex *models,
                            const McmeTextureIndex *textures,
                            const char *namespace, const char *path,
                            const double tint[3], McmeMesh *out);

/*
 * The block tint (D44): the colour the reference's own lookup
 * answers for one block state, before any fold. The three
 * sources are the reference's tables, keyed its way: the block's
 * own namespaced name (`BlockTints`), redstone_wire's `power`,
 * pumpkin and melon stem's `age` — the state-keyed branches only
 * inside the minecraft namespace, and only where the state is
 * present and a string: a missing or typed state RAISES in the
 * reference (KeyError, AttributeError), so false here means
 * exactly that, and the caller answers what the reference's
 * caught exception leaves behind. Every other block answers its
 * colour, or (1, 1, 1) when no table names it. A `namespace` NULL
 * reads as "minecraft".
 */
bool mcme_block_tint(const char *namespace, const char *base_name,
                     const McmeState *states, size_t state_count,
                     double out[3]);

/*
 * The reference's `get_missing_block`: the unit cube wearing the
 * missing image, opaque, one bucket per cull direction. It answers
 * a block the pack's blockstates do not know, and the block-level
 * loader's winning applies whose `apply` carried no model at all.
 */
bool mcme_mesh_missing_block(McmeMesh *out);

/*
 * Turns a whole mesh by the selection's x/y — the reference's
 * `BlockMesh.rotate`, quirks included (decisions.md#D33):
 *
 *   - `rotx`/`roty` are quarter-turn steps, degrees floored the
 *     way Python floors division: x -45 is step -1, not 0.
 *   - The reference's guard reads `if rotx or roty and (roty,
 *     rotx) in cull_remap_all:` — a NONZERO rotx skips the table
 *     test, so an out-of-range step (magnitude above 3) then
 *     raises on the lookup; rotx zero with an out-of-range roty
 *     returns the mesh untouched; (0, 0) short-circuits. This
 *     function mirrors that exactly: false with mcme_last_error()
 *     for the raising case, true and untouched for the others.
 *   - Vertices turn about (0.5, 0.5, 0.5) by -90*rotx about X then
 *     -90*roty about Y; texture coordinates do not turn (uvlock's
 *     TODO in the reference has stood unimplemented since it was
 *     written, and mcme follows the reference); culling buckets
 *     are remapped by the same turns, "none" to "none".
 *
 * The mesh is rewritten in place. On the raising refusal nothing
 * moves.
 */
bool mcme_mesh_rotate(McmeMesh *mesh, int32_t rotx, int32_t roty);

/*
 * Merges `count` meshes into `*out` (released with
 * mcme_mesh_free) — the reference's `BlockMesh.merge`: per cull
 * direction the buckets concatenate with faces offset by the
 * running vertex count, the texture list becomes the sorted set of
 * every part's textures with each part's indexes remapped through
 * it, and transparency is the most transparent part. Zero parts
 * merge to the empty mesh. The parts stay owned by the caller.
 *
 * Returns false only on a null argument or an exhausted heap.
 */
bool mcme_mesh_merge(const McmeMesh *const *parts, size_t count,
                     McmeMesh *out);

/*
 * The reference's `_get_model` as one call (D33): the block
 * state's selection walked, each winning apply meshed and turned
 * by its x/y — variants answering the first apply that meshes and
 * multipart merging every one that does, refusals skipped, the
 * missing block standing in for applies without a model. The
 * block-level answer is total: the reference catches every load
 * failure, so false here is a null argument or an exhausted heap,
 * never a bad pack.
 */
bool mcme_mesh_for_block(const McmeBlockstateIndex *blockstates,
                         const McmeModelIndex *models,
                         const McmeTextureIndex *textures,
                         const char *namespace, const char *base_name,
                         const McmeState *states, size_t state_count,
                         McmeMesh *out);

/* Releases a mesh and everything it owns. NULL is a no-op. */
void mcme_mesh_free(McmeMesh *mesh);

/* ========================================================================
 * Texture coordinates in atlas space (D35)
 *
 * The mesh's own coordinates never reach the sampler: the editor's
 * chunk builder gives every triangle the rectangle its texture
 * landed on, and the fragment stage maps the mesh's coordinates
 * into that rectangle at draw time. The pieces, as the reference
 * has them:
 *
 *   - The vertex table is one row of twelve floats per vertex of
 *     the face stream — position, texture coordinate, the
 *     rectangle, tint — built per cull bucket as
 *     `verts[faces]`, `tverts[faces]`, the rectangle written once
 *     per THREE rows (one triangle) from the bucket's per-triangle
 *     texture index, and the tint folded twice — the per-direction
 *     brightness, then the band shade — which the table's `tints`
 *     answers (D36); the rows' rectangles are here.
 *   - The rectangle for a texture path comes from one rule: the
 *     page's rectangle under that exact name, or the missing
 *     image's own rectangle when the name is not on the page. The
 *     reference's texture_bounds, quirks included (D35).
 *   - The mapping is the fragment stage's, per axis:
 *     mix(b0, b1, mod(t, 1.0)) — b0 + (b1 - b0) * the coordinate
 *     wrapped into [0, 1) the GLSL way (floor-based, so a
 *     negative coordinate wraps up rather than clamps). Rendered
 *     live in the shader; the renderer-to-be wants the numbers,
 *     so mcme answers them.
 *
 * The atlas this layer reads is the editor's own: the page always
 * carries the missing image first — the manager seeds its table
 * with it before the pack's files, so the app's page has it too
 * (D34 recorded the strip while mcme had no image of its own; D35
 * gives it one, the reader's exact 16x16 magenta-and-black
 * checker, and the strip is superseded).
 */

/* The word for the missing image, the reference's own key word for
   it (its file lives inside its reader's package). The pins
   canonicalise the reference's path to this token; the texture
   index answers it for a texture the pack does not hold. */
#define MCME_MISSING_TEXTURE "missing_no"

/* The bounds a lookup answers for `name` — the reference's
   texture_bounds: the rectangle `name` landed on, or the missing
   image's own rectangle where `name` is not on the page. `out`
   takes u0 v0 u1 v1. False only on a null argument or an atlas
   that somehow carries no missing image. */
bool mcme_atlas_bounds(const McmeAtlas *atlas, const char *name,
                       double out[4]);

/* ========================================================================
 * The tint column (D36)
 *
 * The builder's table's last group is the vertex's colour, and it
 * is the mesh's tint folded twice before the rows are done. First
 * with a per-direction brightness the builder owns: a step of 0.15
 * per side — down three steps, the east/west pair two, the
 * north/south pair one, up and the cullless none bucket none — one
 * grey on all three components. Then with a band shade the vertex's
 * WORLD height reads out of a triangular wave: (y / 32) % 2,
 * folded back over 1 where it passes the top, and scaled — 0.9 +
 * 0.2 of it. The light runs 0.9 at the trough to 1.1 at the peak,
 * 0.1 either side of flat, and back every 64 blocks: the trough
 * belongs to t == 0 (y a multiple of 64) and the peak to t == 1
 * (y == 32, 96, ...) because the reference folds `shade > 1`, not
 * `>=`.
 *
 * The wave reads the block's world height — the builder adds the
 * chunk offset to the vertex before shading — so the table's
 * answer takes the block's own y. The modulo in that read is
 * numpy's: its remainder carries the DIVISOR's sign, so a block
 * below zero walks the wave like any other. C's fmod would answer
 * negative and fold to a light below the trough; mcme adds the
 * period back.
 *
 * Under the fold the mesh's tint is D32's answer: the default
 * (1, 1, 1) for every block the reference does not colour. The
 * block tints themselves — grass, foliage — stay out of this
 * layer; the pins skip the blocks the reference tints, by the
 * reference's own tint function.
 */

/* ========================================================================
 * The cull clauses (D37)
 *
 * The table's rows are the faces that SURVIVE the builder's cull,
 * and the cull is two clauses over transparency alone
 * (chunk_builder_cy.pyx 279-289): a face whose direction has a
 * cullface drops when the NEIGHBOUR's transparency is opaque, or
 * when neighbour and self are both translucent — the chained
 * `== 1 == 1`. A face with no cullface (the none bucket) never
 * culls at all: the test runs under `if cull_id:`, so a full cube
 * without cullface keeps every face even embedded in stone. Air,
 * partial on both counts, fires neither clause.
 *
 * The quirk to hold on to: the reference culls on the
 * TRANSPARENCY FLAG alone — there is no full-block or shape
 * condition, so a partial block with opaque textures culls its
 * neighbours' faces exactly like a full one would. The game's own
 * rule asks whether the neighbour's shape covers the face; the
 * map follows the reference, and the reference does not ask.
 *
 * The neighbour is one block state standing on all six sides —
 * what the block-level verb can honestly carry, the same answer
 * in every direction. A neighbour the pack does not have is the
 * missing BLOCK: opaque, and it culls. The mesh's own
 * transparency is D32's classification; `beside` is the
 * neighbour's, the same enum.
 */

/* One cull bucket's rows in atlas space — one row per vertex of
   the face stream (six per face: the two triangles the builder
   draws), in stream order, so a row's rectangle is its
   triangle's. */
typedef struct {
    double *bounds;   /* 4 per row: u0 v0 u1 v1                  */
    double *auvs;     /* 2 per row: the coordinates mapped into
                         that rectangle                          */
    double *tints;    /* 3 per row: r g b under the two folds     */
    size_t  row_count;
} McmeAtlasBucket;

typedef struct {
    McmeAtlasBucket buckets[7]; /* indexed by McmeCullDir        */
    bool            present[7]; /* as the mesh has them          */
} McmeAtlasMesh;

/* Answers the mesh's SURVIVING texture coordinates in atlas space
   and its colour: `bounds` naming the rectangle every row's
   triangle draws from (its texture looked up by the rule above),
   `auvs` the mesh's own coordinates mapped through it — the
   fragment stage's arithmetic done here — and `tints` the mesh's
   tint folded the builder's way, brightness and band shade (D36),
   the shade reading the block's world height as `block_y` plus
   the vertex's own. The rows are the faces the cull clauses keep
   (D37): `beside` is the transparency of the one block state
   standing on all six sides — the neighbour's own, so a bucket
   with a cullface answers no rows when that neighbour is opaque
   or when both are translucent, and the none bucket answers its
   rows under any neighbour. The uniform neighbour is the
   block-level verb's one question asked of all six sides; six
   real ones are mcme_atlas_mesh_dirs (D38). Answers `*out`
   (released with mcme_atlas_mesh_free); false only on a null
   argument or an exhausted heap. */
bool mcme_atlas_mesh(const McmeAtlas *atlas, const McmeMesh *mesh,
                     long block_y, McmeTransparency beside,
                     McmeAtlasMesh *out);

/* ========================================================================
 * The six neighbours (D38)
 *
 * The builder's cull has always been PER-FACE: each face reads
 * the neighbour standing in ITS own direction — CULL_MAP
 * (chunk_builder_cy.pyx 49-55) answers up, down, north, east,
 * south, west, one lookup per face — so six faces can six
 * answers. The uniform `beside` (D37) asks the one question of
 * all six sides; the world the table is drawn from has six real
 * neighbours standing, and `chunk mesh` reads them out of the
 * chunk itself, one transparency each, the clauses unchanged:
 * a bucket with a cullface answers no rows when ITS side's
 * neighbour is opaque, or when that neighbour and the mesh are
 * both translucent; the none bucket answers its rows under any
 * neighbour. A side outside the stored world is air — partial,
 * firing neither clause.
 */

/* Answers like mcme_atlas_mesh, but with one neighbour per
   direction: `beside` is indexed by McmeCullDir, DOWN through
   WEST, six entries (MCME_CULL_NONE of them), each the
   transparency that side's own neighbour answers. Same
   allocation, same free, same refusals. */
bool mcme_atlas_mesh_dirs(const McmeAtlas *atlas, const McmeMesh *mesh,
                          long block_y,
                          const McmeTransparency beside[MCME_CULL_NONE],
                          McmeAtlasMesh *out);

/* Releases an atlas-space mesh and everything it owns. NULL is a
   no-op. */
void mcme_atlas_mesh_free(McmeAtlasMesh *m);

#endif /* MCME_RESOURCE_H */
