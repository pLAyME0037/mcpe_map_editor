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
    MCME_LEVELDAT_NONE      = 0,   /* no level.dat found */
    MCME_LEVELDAT_GZIP      = 1,   /* raw gzip, no header          -> Java */
    MCME_LEVELDAT_HEADER_PLAIN = 2, /* 8-byte header, bare NBT     -> Bedrock */
    MCME_LEVELDAT_HEADER_GZIP  = 3, /* 8-byte header, then gzip    -> Bedrock */
    MCME_LEVELDAT_BARE_NBT  = 4,   /* no header, no gzip; ambiguous */
} McmeLevelDatFrame;

typedef struct {
    McmeWorldKind    kind;
    McmeLevelDatFrame frame;
    char *path;

    char    *name;             /* LevelName, else levelname.txt; NULL if absent */
    char    *version;          /* human-readable game version, or NULL */
    int32_t  data_version;     /* Java DataVersion; 0 if absent */
    bool     has_spawn;
    int32_t  spawn_x, spawn_y, spawn_z;
    int32_t  game_type;
    bool     has_game_type;
    int64_t  last_played;      /* unix seconds; 0 if absent */

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

#endif /* MCME_WORLD_H */
