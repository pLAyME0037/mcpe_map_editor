/*
 * mcme world detection and metadata.
 *
 * Given a directory, work out which edition of Minecraft it holds and summarise
 * it. This is what `mcme info` prints and what the GUI's world picker uses, so
 * it must be cheap, must never write, and must survive being pointed at
 * something that is not a world at all.
 *
 * How the two editions are told apart:
 *
 *   Bedrock   a db/ directory (a LevelDB store) alongside level.dat. level.dat
 *             itself is an 8-byte little-endian header — [storage version]
 *             [payload length] — followed by the NBT, which may or may not then
 *             be gzipped. Minecraft gzips it; amulet's writer does not, so both
 *             are accepted. The NBT is little-endian and its root is the level
 *             compound directly, with no `Data` wrapper.
 *
 *   Java      level.dat with no db/ beside it, plus one of region/, session.lock
 *             or a DIM-n directory. level.dat is plain gzip with no header,
 *             wrapping big-endian NBT whose root compound holds a `Data`
 *             child that in turn holds LevelName, SpawnX/Y/Z, GameType and
 *             friends.
 *
 * The marker test comes first because it is cheap and unambiguous; the level.dat
 * framing is only consulted when no marker is present, since a bare level.dat
 * is a thing people do copy around.
 */

#ifndef MCME_WORLD_H
#define MCME_WORLD_H

#include "mcme/core.h"
#include "mcme/nbt.h"    /* McmeNbtDoc: what mcme_level_dat_parse fills */

typedef enum {
    MCME_WORLD_UNKNOWN  = 0,
    MCME_WORLD_JAVA     = 1,
    MCME_WORLD_BEDROCK  = 2,
} McmeWorldKind;

/* NBT frame a level.dat was wrapped in. Reported because it is the single most
   reliable Java/Bedrock discriminator and callers debugging a mis-detected
   world want to see it. */
typedef enum {
    MCME_LEVELDAT_NONE         = 0, /* no level.dat found */
    MCME_LEVELDAT_GZIP         = 1, /* raw gzip, no header       -> Java */
    MCME_LEVELDAT_HEADER_PLAIN = 2, /* 8-byte header, bare NBT   -> Bedrock */
    MCME_LEVELDAT_HEADER_GZIP  = 3, /* 8-byte header, then gzip  -> Bedrock */
    MCME_LEVELDAT_BARE_NBT     = 4, /* no header, no gzip; ambiguous */
} McmeLevelDatFrame;

typedef struct {
    McmeWorldKind    kind;
    McmeLevelDatFrame frame;
    char *path;

    char    *name;         /* LevelName, else levelname.txt; NULL if absent */
    char    *version;      /* human-readable game version, or NULL */
    int32_t  data_version; /* Java DataVersion; 0 if absent */
    bool     has_spawn;
    int32_t  spawn_x, spawn_y, spawn_z;
    int32_t  game_type;
    bool     has_game_type;
    int64_t  last_played;  /* unix seconds; 0 if absent */

    /* Java inventory, from scanning the overworld region/ directory. */
    size_t   region_files;
    size_t   chunks;

    /* Bedrock inventory. `db_path` is the db/ directory itself, which is what
       mcme_leveldb_open expects — not the world folder above it, which is the
       mistake callers make most often and which this module has already
       resolved so they do not have to. NULL when there is no store. */
    bool     has_db;
    char    *db_path;
    bool     has_level_dat;
} McmeWorldInfo;

const char *mcme_world_kind_name(McmeWorldKind k);
const char *mcme_level_dat_frame_name(McmeLevelDatFrame f);

/* ========================================================================
 * One block out of the store (G9)
 *
 * The block position's own answer — what `chunk get` prints and
 * `chunk mesh` reads its seven specs from — moved from the CLI into
 * the core whole (G9): the same walks, the same "chunk: ..." messages,
 * the same answers, and the same road the GUI's worker now asks. The
 * verbs keep their exit codes by mapping this status; a non-OK answer
 * always has mcme_last_error() standing.
 * ======================================================================== */

typedef enum {
    MCME_WORLDBLOCK_OK = 0,
    MCME_WORLDBLOCK_NOT_FOUND,   /* not stored, deleted, or this world
                                    holds no such dimension          */
    MCME_WORLDBLOCK_BAD_FORMAT,  /* the store's bytes are not the
                                    format mcme reads                */
    MCME_WORLDBLOCK_IO,          /* the store would not open or read */
    MCME_WORLDBLOCK_USAGE,       /* the coordinates or the dimension
                                    cannot name a block              */
    MCME_WORLDBLOCK_INTERNAL     /* the path too long, or the heap   */
} McmeWorldBlockStatus;

/*
 * The world's own answer for one block position: the spec text the
 * store spells there — "minecraft:stone", with {states} when the
 * entry carries them, the dialect `chunk set` accepts — appended to
 * `out` (a fresh buffer's own). Dispatches on `info->kind`; a world
 * that is neither Bedrock nor Java refuses usage with the CLI's own
 * message. Every non-OK answer sets mcme_last_error() with the
 * "chunk: ..." text the verbs have always printed, moved whole.
 */
McmeWorldBlockStatus mcme_world_block_spec(const McmeWorldInfo *info,
                                           int32_t dimension,
                                           long x, long y, long z,
                                           McmeBuffer *out);

/* ========================================================================
 * The sub-chunk grid the builder walks (D40, G11)
 *
 * One sub-chunk with one cell of neighbours on every side — 18³
 * spec cells, the sub-chunk's own at [1..16] — read from the world
 * once: the seven sub-chunks' layers (the target's, then DOWN
 * through WEST, the CULL_MAP order the sides' offsets come in),
 * each format's own reader behind the same answer. `chunk submesh`
 * walked this grid from the CLI; it moves here whole (G11) so the
 * GUI's section view asks the same road. The status is the block
 * reader's own — the verbs map it to their exit codes, and a
 * non-OK answer always has mcme_last_error() standing.
 *
 * The padding is the world's own edge rule, the same one `chunk
 * mesh` reads a neighbour chunk by: a neighbour's sub-chunk the
 * store has never held is air — the edge stands open. Only the
 * TARGET's read refuses: Bedrock a sub-chunk the store has never
 * held, Java a chunk the region file does not have. A Java
 * section the chunk does not list is the format's own all-air
 * answer, not a refusal.
 *
 * Everything the grid holds lives in its arenas — the specs'
 * text and the index arrays both — until mcme_subgrid_free. The
 * specs are the dialect `chunk set` accepts, the same text
 * mcme_world_block_spec answers with.
 * ======================================================================== */

#define MCME_SUB_EDGE  16
#define MCME_SUB_WIDTH 18   /* the sub-chunk with one cell of
                               neighbours on every side */
#define MCME_SUB_CELLS (MCME_SUB_WIDTH * MCME_SUB_WIDTH \
                        * MCME_SUB_WIDTH)
#define MCME_SUB_LAYERS 7
#define MCME_SUB_TARGET 0   /* the layers order: the sub-chunk
                               itself, then DOWN through WEST     */

/* One sub-chunk's block layer as the grid keeps it: the palette's
   specs and the 4096 indices pointing at them, X-then-Z-then-Y. A
   layer the world has none for is air in every cell. */
typedef struct {
    const char   **specs;     /* spec_count specs                  */
    size_t         spec_count;
    const uint16_t *indices;  /* 4096, XZY — NULL when all air     */
} McmeSubchunkLayer;

typedef struct {
    McmeSubchunkLayer layers[MCME_SUB_LAYERS];
    const char       *cells[MCME_SUB_CELLS]; /* the padded grid:
                                                sub-chunk [1..16],
                                                air corners       */
    McmeArena        *arenas[MCME_SUB_LAYERS];
    int               arena_count;
} McmeSubgrid;

/*
 * Compose the grid around the sub-chunk holding block (x, y, z):
 * seven layers read through the world's own format, the padded
 * grid built from them. `*grid` owns everything on success — free
 * it with mcme_subgrid_free. A refused compose owns nothing.
 */
McmeWorldBlockStatus mcme_world_subgrid(const McmeWorldInfo *info,
                                        int32_t dimension,
                                        long x, long y, long z,
                                        McmeSubgrid *grid);

void mcme_subgrid_free(McmeSubgrid *grid);

/*
 * The spec at the grid's local position: -1..MCME_SUB_EDGE on each
 * axis, the sub-chunk's cells at [0..15] and the padding around
 * them. Total — any position outside the padded box is air, so a
 * caller walking the sub-chunk's own cells may read every
 * neighbour without a bounds check.
 */
const char *mcme_subgrid_at(const McmeSubgrid *grid, int lx, int ly,
                            int lz);

/*
 * Inspect `path`. Returns false only when `path` is not a readable directory or
 * `out` is NULL; a directory that simply is not a world is a success with
 * kind == MCME_WORLD_UNKNOWN, so callers can report what they found rather than
 * an I/O error.
 *
 * A false return leaves `*out` zeroed and owns nothing, so callers need not
 * call mcme_world_info_free on the error path. On success the result must be
 * released with mcme_world_info_free.
 */
bool mcme_world_probe(McmeWorldInfo *out, const char *path);
void mcme_world_info_free(McmeWorldInfo *info);

/*
 * Reads `<path>` — one level.dat file, not a directory — in whatever
 * frame it wears (raw gzip, the Bedrock 8-byte header plain or with a
 * gzipped payload, bare NBT) and parses it into `doc` from `arena`.
 * `*frame` says which wrapper was found, and is set even when the NBT
 * does not parse. The parse copies into `arena`, so the file's bytes
 * never outlive the call.
 */
bool mcme_level_dat_parse(McmeNbtDoc *doc, McmeArena *arena,
                          const char *path, McmeLevelDatFrame *frame);

/*
 * The directory a Java world keeps a dimension in: the overworld at the
 * world's own root (*out_dir = NULL, the caller joins "region" itself),
 * the nether in DIM-1, the end in DIM1 — the game's own layout, and all
 * of it Java keeps: any other number is usage rather than a directory
 * mcme would invent. The number is the one the CLI parses: 0, 1, 2.
 */
bool mcme_java_dimension_dir(int32_t dimension, const char **out_dir);

#endif /* MCME_WORLD_H */
