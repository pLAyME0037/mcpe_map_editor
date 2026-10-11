#include "mcme/bedrock_keys.h"

#include <string.h>

/* Reads one little-endian int32. Bedrock is little-endian throughout and the
   build machine is not guaranteed to be, so this is a shift rather than a
   dereference. */
static int32_t read_le32(const uint8_t *p)
{
    return (int32_t)((uint32_t)p[0] | ((uint32_t)p[1] << 8) |
                     ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24));
}

static bool is_chunk_tag(uint8_t tag)
{
    return tag == MCME_BEDROCK_TAG_SUBCHUNK_OLD ||
           tag == MCME_BEDROCK_TAG_SUBCHUNK ||
           tag == MCME_BEDROCK_TAG_CHUNK_VERSION;
}

bool mcme_bedrock_key_parse(const uint8_t *key, size_t len, McmeBedrockKey *out)
{
    if (out != NULL) memset(out, 0, sizeof *out);
    if (key == NULL || out == NULL) return false;

    /* x, z, tag, and the one spare byte some tags use for a subchunk index.
       Anything else is not a chunk key — a level record, an actor, a
       portallist — which is the common case in a real store, so this path is
       the one that runs most often and is checked first. */
    size_t tag_at = 0;
    if (len == 9 || len == 10) {
        tag_at = 8;
        out->has_dimension = false;
        out->dimension = 0;
    } else if (len == 13 || len == 14) {
        tag_at = 12;
        out->has_dimension = true;
        out->dimension = read_le32(key + 8);
    } else {
        return false;
    }

    out->x = read_le32(key);
    out->z = read_le32(key + 4);
    out->tag = key[tag_at];
    out->is_chunk = is_chunk_tag(out->tag);
    return true;
}

const char *mcme_bedrock_tag_name(uint8_t tag)
{
    switch (tag) {
    case MCME_BEDROCK_TAG_SUBCHUNK_OLD: return "subchunk-old";
    case MCME_BEDROCK_TAG_SUBCHUNK:     return "subchunk";
    case MCME_BEDROCK_TAG_CHUNK_VERSION: return "version";
    case MCME_BEDROCK_TAG_BIOME:        return "biome";
    default:                            return NULL;
    }
}
