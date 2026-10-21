/*
 * One block out of the store — the CLI's java_chunk_doc,
 * java_get_block and bedrock_get_block moved into the core whole
 * (G9): the same walks, the same "chunk: ..." messages, the same
 * answers. The verbs dispatch here now through
 * mcme_world_block_spec, mapping the status to their exit codes,
 * and the GUI's chunk worker asks the same road. See world.h and
 * java_chunk.h.
 */

#include "mcme/world.h"

#include <stdio.h>
#include <string.h>

#include "mcme/bedrock_chunk.h"
#include "mcme/bedrock_write.h" /* the sub-chunk key, the spec text */
#include "mcme/java_chunk.h"
#include "mcme/leveldb.h"
#include "mcme/region.h"

McmeWorldBlockStatus mcme_java_chunk_doc(const char *world_dir,
                                         int32_t dimension,
                                         long x, long y, long z,
                                         McmeArena **arena,
                                         McmeNbtDoc *doc,
                                         char *out_file,
                                         size_t out_file_cap)
{
    const char *dim_dir = NULL;
    int cx, cz, rx, rz;
    McmeRegion reg;
    McmeBuffer nbt;
    bool read;

    if (world_dir == NULL || arena == NULL || doc == NULL)
        return MCME_WORLDBLOCK_INTERNAL;
    if (!mcme_java_dimension_dir(dimension, &dim_dir)) {
        return MCME_WORLDBLOCK_USAGE;
    }
    cx = (int)(x >> 4);
    cz = (int)(z >> 4);
    mcme_region_of(cx, cz, &rx, &rz);

    if (mcme_region_path(out_file, out_file_cap, world_dir, dim_dir,
                         rx, rz) == NULL) {
        mcme_set_error("chunk: the path to the chunk of block "
                       "(%ld, %ld, %ld) is too long", x, y, z);
        return MCME_WORLDBLOCK_INTERNAL;
    }

    if (!mcme_fs_exists(out_file)) {
        mcme_set_error("chunk: the chunk of block (%ld, %ld, %ld) "
                       "is not stored in this world", x, y, z);
        return MCME_WORLDBLOCK_NOT_FOUND;
    }
    if (!mcme_region_open(&reg, out_file)) {
        return MCME_WORLDBLOCK_BAD_FORMAT; /* open said what and where */
    }
    if (!mcme_region_chunk_present(&reg, cx, cz)) {
        mcme_set_error("chunk: the chunk of block (%ld, %ld, %ld) "
                       "is not stored in this world", x, y, z);
        mcme_region_close(&reg);
        return MCME_WORLDBLOCK_NOT_FOUND;
    }

    mcme_buffer_init(&nbt);
    read = mcme_region_read_chunk(&reg, cx, cz, NULL, &nbt);
    mcme_region_close(&reg);
    if (!read) {
        char why[256];
        snprintf(why, sizeof why, "%s", mcme_last_error());
        mcme_set_error("chunk: the chunk of block (%ld, %ld, %ld) "
                       "does not read: %s", x, y, z, why);
        mcme_buffer_free(&nbt);
        return MCME_WORLDBLOCK_BAD_FORMAT;
    }

    *arena = mcme_arena_new();
    if (*arena == NULL) {
        mcme_set_error("chunk: out of memory");
        mcme_buffer_free(&nbt);
        return MCME_WORLDBLOCK_INTERNAL;
    }
    if (!mcme_nbt_parse(doc, *arena, nbt.data, nbt.len, false)) {
        char why[256];
        snprintf(why, sizeof why, "%s", mcme_last_error());
        mcme_set_error("chunk: the chunk of block (%ld, %ld, %ld) "
                       "does not decode: %s", x, y, z, why);
        mcme_buffer_free(&nbt);
        mcme_arena_free(*arena);
        *arena = NULL;
        return MCME_WORLDBLOCK_BAD_FORMAT;
    }
    mcme_buffer_free(&nbt);
    return MCME_WORLDBLOCK_OK;
}

/* The Java half: the document, then its palettes, one position. */
static McmeWorldBlockStatus java_get_block(const McmeWorldInfo *info,
                                           int32_t dimension,
                                           long x, long y, long z,
                                           McmeBuffer *spec)
{
    McmeArena *arena = NULL;
    McmeNbtDoc doc;
    char file[MCME_REGION_PATH_MAX];
    McmeWorldBlockStatus st = mcme_java_chunk_doc(
        info->path, dimension, x, y, z, &arena, &doc,
        file, sizeof file);

    if (st == MCME_WORLDBLOCK_OK
            && !mcme_java_block_spec(doc.root, (int)x, (int)y, (int)z,
                                     spec)) {
        char why[256];
        snprintf(why, sizeof why, "%s", mcme_last_error());
        mcme_set_error("chunk: the chunk of block (%ld, %ld, %ld): %s",
                       x, y, z, why);
        st = MCME_WORLDBLOCK_BAD_FORMAT;
    }
    if (arena != NULL) mcme_arena_free(arena);
    return st;
}

/* The Bedrock half: the sub-chunk's payload, then its layer. */
static McmeWorldBlockStatus bedrock_get_block(
    const McmeWorldInfo *info, int32_t dimension,
    long x, long y, long z, McmeBuffer *spec)
{
    uint8_t key[14];
    size_t key_len = 0;
    McmeWorldBlockStatus st = MCME_WORLDBLOCK_OK;
    McmeBuffer payload;
    bool deleted = false;
    McmeLevelDb *db = NULL;
    McmeArena *arena = NULL;

    if (!mcme_bedrock_subchunk_key((int32_t)x, (int32_t)y,
                                   (int32_t)z, dimension,
                                   key, sizeof key, &key_len)) {
        return MCME_WORLDBLOCK_USAGE;
    }

    mcme_buffer_init(&payload);
    if (!mcme_leveldb_open(&db, info->db_path)) {
        st = MCME_WORLDBLOCK_IO;
    } else if (!mcme_leveldb_has(db, key, key_len)) {
        mcme_leveldb_close(db);
        mcme_set_error("chunk: the subchunk of block (%ld, %ld, %ld) "
                       "is not stored in this world", x, y, z);
        st = MCME_WORLDBLOCK_NOT_FOUND;
    } else {
        bool got = mcme_leveldb_get(db, key, key_len, &payload,
                                    &deleted);
        mcme_leveldb_close(db);
        if (!got) {
            st = MCME_WORLDBLOCK_IO; /* the store's own error stands */
        } else if (deleted) {
            mcme_set_error("chunk: the subchunk of block "
                           "(%ld, %ld, %ld) is deleted in this world",
                           x, y, z);
            st = MCME_WORLDBLOCK_NOT_FOUND;
        }
    }

    if (st == MCME_WORLDBLOCK_OK) {
        arena = mcme_arena_new();
        if (arena == NULL) {
            mcme_set_error("chunk: out of memory");
            st = MCME_WORLDBLOCK_INTERNAL;
        }
    }
    if (st == MCME_WORLDBLOCK_OK) {
        McmeBedrockSubchunk sc;
        if (!mcme_bedrock_subchunk_decode(&sc, arena,
                                          payload.data, payload.len)) {
            char why[256];
            snprintf(why, sizeof why, "%s", mcme_last_error());
            mcme_set_error("chunk: subchunk of block (%ld, %ld, %ld) "
                           "does not decode: %s", x, y, z, why);
            st = MCME_WORLDBLOCK_BAD_FORMAT;
        } else if (sc.layer_count == 0) {
            /* The all-air marker: the position holds air. */
            if (!mcme_buffer_append_cstr(spec, "minecraft:air")) {
                st = MCME_WORLDBLOCK_INTERNAL;
            }
        } else {
            long lx = x % 16, ly = y % 16, lz = z % 16;
            if (lx < 0) lx += 16;
            if (ly < 0) ly += 16;
            if (lz < 0) lz += 16;
            size_t u = (size_t)lx * 256 + (size_t)lz * 16 +
                       (size_t)ly;
            const McmeBedrockLayer *layer = &sc.layers[0];
            uint16_t idx = layer->indices[u];
            if (idx >= layer->palette_len
                    || layer->palette[idx].root == NULL) {
                mcme_set_error("chunk: the index at block (%ld, %ld, "
                               "%ld) is %u, outside its palette of "
                               "%zu entries", x, y, z, idx,
                               layer->palette_len);
                st = MCME_WORLDBLOCK_BAD_FORMAT;
            } else {
                const McmeTag *entry = layer->palette[idx].root;
                if (!mcme_bedrock_block_spec(entry, spec)) {
                    char why[256];
                    snprintf(why, sizeof why, "%s",
                             mcme_last_error());
                    mcme_set_error("chunk: the palette entry at "
                                   "block (%ld, %ld, %ld): %s",
                                   x, y, z, why);
                    st = MCME_WORLDBLOCK_BAD_FORMAT;
                }
            }
        }
    }

    if (arena != NULL) mcme_arena_free(arena);
    mcme_buffer_free(&payload);
    return st;
}

McmeWorldBlockStatus mcme_world_block_spec(const McmeWorldInfo *info,
                                           int32_t dimension,
                                           long x, long y, long z,
                                           McmeBuffer *out)
{
    if (info == NULL || out == NULL)
        return MCME_WORLDBLOCK_INTERNAL;

    switch (info->kind) {
    case MCME_WORLD_JAVA:
        return java_get_block(info, dimension, x, y, z, out);
    case MCME_WORLD_BEDROCK:
        return bedrock_get_block(info, dimension, x, y, z, out);
    default:
        mcme_set_error("chunk: '%s' is not a Bedrock or Java world",
                       info->path);
        return MCME_WORLDBLOCK_USAGE;
    }
}
