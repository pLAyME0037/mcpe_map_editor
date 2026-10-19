#include "mcme/resource.h"

#include <stdio.h>
#include <string.h>

#include "mcme/core.h"

/*
 * The block tint (D44) — the reference's own lookup, transcribed
 * rule for rule from `_get_block_tint` (minecraft_model_reader's
 * resource_pack_manager.py):
 *
 *   if namespace == "minecraft":
 *       redstone_wire  -> RedstoneTints[power]     (py_str)
 *       pumpkin_stem   -> StemTints[age]            (py_str)
 *       melon_stem     -> StemTints[age]            (py_str)
 *   return BlockTints.get("ns:base", (1, 1, 1))
 *
 * The state-keyed branches index a dict with the state's STRING
 * value, so a state the block lacks is a KeyError and a state
 * the store typed is an AttributeError — both raised inside the
 * reference's per-apply try, both caught there. mcme answers
 * that case as false, and the caller's walk does the same thing
 * the caught exception does: every apply refused, the missing
 * block (variants) or the empty mesh (multipart).
 *
 * Colours are the tables' own bytes; `_get_colour` divides by
 * 255 and this does the identical division on doubles, so the
 * answers are the same IEEE products the reference compares.
 */

/* rgb, keyed the way each table keys. */
struct TintEntry {
    const char *key;
    unsigned char r, g, b;
};

#define GRASS   0x92, 0xBC, 0x58 /* GrassColour             */
#define FOLIAGE 0x77, 0xAB, 0x2F /* DefaultFoliageColour    */

/* BlockTints — keyed by the block's full namespaced name. */
static const struct TintEntry k_named[] = {
    { "minecraft:leaf_litter",         0xA3, 0x75, 0x46 },
    { "minecraft:pink_petals",         GRASS },
    { "minecraft:wildflowers",         GRASS },
    { "minecraft:large_fern",          GRASS },
    { "minecraft:tall_grass",          GRASS },
    { "minecraft:potted_fern",         GRASS },
    { "minecraft:water_cauldron",      0x44, 0xAF, 0xF5 },
    { "minecraft:lily_pad",            0x20, 0x80, 0x30 },
    { "minecraft:attached_pumpkin_stem", 0xE0, 0xC7, 0x1C },
    { "minecraft:attached_melon_stem", 0xE0, 0xC7, 0x1C },
    { "minecraft:vine",                FOLIAGE },
    { "minecraft:short_grass",         GRASS },
    { "minecraft:fern",                GRASS },
    { "minecraft:bush",                GRASS },
    { "minecraft:jungle_leaves",       FOLIAGE },
    { "minecraft:acacia_leaves",       FOLIAGE },
    { "minecraft:dark_oak_leaves",     FOLIAGE },
    { "minecraft:birch_leaves",        0x80, 0xA7, 0x55 },
    { "minecraft:spruce_leaves",       0x61, 0x99, 0x61 },
    { "minecraft:oak_leaves",          FOLIAGE },
    { "minecraft:mangrove_leaves",     FOLIAGE },
    { "minecraft:grass_block",         GRASS },
};

/* RedstoneTints — keyed by the power state's own text. */
static const struct TintEntry k_redstone[] = {
    { "0",   0x4C, 0x00, 0x00 }, { "1",   0x70, 0x00, 0x00 },
    { "2",   0x7A, 0x00, 0x00 }, { "3",   0x84, 0x00, 0x00 },
    { "4",   0x8E, 0x00, 0x00 }, { "5",   0x99, 0x00, 0x00 },
    { "6",   0xA3, 0x00, 0x00 }, { "7",   0xAD, 0x00, 0x00 },
    { "8",   0xB7, 0x00, 0x00 }, { "9",   0xC1, 0x00, 0x00 },
    { "10",  0xCC, 0x00, 0x00 }, { "11",  0xD6, 0x00, 0x00 },
    { "12",  0xE0, 0x00, 0x00 }, { "13",  0xEA, 0x06, 0x00 },
    { "14",  0xF4, 0x1B, 0x00 }, { "15",  0xFF, 0x32, 0x00 },
};

/* StemTints — keyed by the age state's own text. */
static const struct TintEntry k_stem[] = {
    { "0", 0x00, 0xFF, 0x00 }, { "1", 0x20, 0xF7, 0x04 },
    { "2", 0x40, 0xEF, 0x08 }, { "3", 0x60, 0xE7, 0x0C },
    { "4", 0x80, 0xDF, 0x10 }, { "5", 0xA0, 0xD7, 0x14 },
    { "6", 0xC0, 0xCF, 0x18 }, { "7", 0xE0, 0xC7, 0x1C },
};

static bool table_lookup(const struct TintEntry *table, size_t count,
                         const char *key, double out[3])
{
    for (size_t i = 0; i < count; ++i) {
        if (strcmp(table[i].key, key) == 0) {
            out[0] = (double)table[i].r / 255.0;
            out[1] = (double)table[i].g / 255.0;
            out[2] = (double)table[i].b / 255.0;
            return true;
        }
    }
    return false;
}

static const McmeState *find_state(const McmeState *states, size_t n,
                                   const char *name)
{
    for (size_t i = 0; i < n; ++i)
        if (states[i].name && strcmp(states[i].name, name) == 0)
            return &states[i];
    return NULL;
}

bool mcme_block_tint(const char *namespace, const char *base_name,
                     const McmeState *states, size_t state_count,
                     double out[3])
{
    if (!base_name || !out) {
        mcme_set_error("block tint: null argument");
        return false;
    }
    if (!namespace) namespace = "minecraft";

    /* The state-keyed branches live inside the minecraft
       namespace only, and demand the state as a string: absent,
       typed, or outside the table's keys each raise in the
       reference and answer false here. */
    const char *keyed = NULL;
    const struct TintEntry *table = NULL;
    size_t count = 0;
    if (strcmp(namespace, "minecraft") == 0) {
        if (strcmp(base_name, "redstone_wire") == 0) {
            keyed = "power";
            table = k_redstone;
            count = sizeof k_redstone / sizeof k_redstone[0];
        } else if (strcmp(base_name, "pumpkin_stem") == 0 ||
                   strcmp(base_name, "melon_stem") == 0) {
            keyed = "age";
            table = k_stem;
            count = sizeof k_stem / sizeof k_stem[0];
        }
    }
    if (keyed) {
        const McmeState *st = states
            ? find_state(states, state_count, keyed) : NULL;
        if (!st || !st->value) return false; /* KeyError | AttributeError */
        return table_lookup(table, count, st->value, out);
    }

    /* The name table, keyed by the full namespaced name. */
    char full[320];
    int n = snprintf(full, sizeof full, "%s:%s", namespace, base_name);
    if (n < 0 || (size_t)n >= sizeof full) {
        out[0] = out[1] = out[2] = 1.0;
        return true; /* no table can name it */
    }
    if (table_lookup(k_named, sizeof k_named / sizeof k_named[0],
                     full, out))
        return true;
    out[0] = out[1] = out[2] = 1.0;
    return true;
}
