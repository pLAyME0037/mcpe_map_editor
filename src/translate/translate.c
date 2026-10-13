/*
 * The Bedrock 1.20 to Java 1.20 block map. The format facts live in
 * translate.h; this file is the table and the walking of it. Every
 * rule here exists because a real palette needed it — the golden
 * Bedrock fixture's own blocks — and tools/ref_convert.py pins each
 * one against PyMCTranslate, the same translator Amulet converts
 * with. The map grows the same way when the next world needs a case.
 */

#include <string.h>

#include "mcme/translate.h"

static const char kSand[]     = "minecraft:sand";
static const char kRedSand[]  = "minecraft:red_sand";
static const char kWire[]     = "minecraft:redstone_wire";
static const char kMushroom[] = "minecraft:mushroom_stem";

/* States that pass through under Java's own name. The two editions
   store these values the same way — enums as strings, booleans and
   small ints as bytes — so the value tag itself is copied untouched. */
struct Keep {
    const char *block;
    const char *from;
    const char *to;
};

static const struct Keep kKeep[] = {
    {"minecraft:grass_block", "snowy",     "snowy"},
    {"minecraft:furnace",     "lit",       "lit"},
};

/* Java names the axis rename applies to. A log, its wood form, and the
   nether stems and hyphae all spell the same property `axis` on the
   Java side and `pillar_axis` on the Bedrock one. mushroom_stem is
   deliberately absent: Java 1.20 gives it no properties at all, so its
   pillar_axis takes the default like any other unknown state. */
static const char *kAxisSuffixes[] = {"_log", "_wood", "_stem", "_hyphae"};

static bool ends_with(const char *s, size_t len, const char *suffix)
{
    size_t n = strlen(suffix);
    return len >= n && memcmp(s + (len - n), suffix, n) == 0;
}

/* The string value of a tag, or NULL when it is not a string. */
static const char *string_value(const McmeTag *t, size_t *out_len)
{
    if (t == NULL || t->type != MCME_TAG_String) return NULL;
    if (out_len != NULL) *out_len = (size_t)t->string.len;
    return t->string.data;
}

/* A fresh `Name`-only entry, the shape air and every property-less
   block takes. */
static McmeTag *named_entry(McmeArena *arena, const char *name, size_t len)
{
    McmeTag *entry = mcme_nbt_new(arena, MCME_TAG_Compound);
    McmeTag *nm = mcme_nbt_new(arena, MCME_TAG_String);
    if (entry == NULL || nm == NULL
            || !mcme_nbt_compound_init(arena, entry, 1)
            || !mcme_nbt_set_string(arena, nm, name, len)
            || !mcme_nbt_compound_put(arena, entry, "Name", 4, nm)) {
        return NULL;
    }
    return entry;
}

McmeTag *mcme_translate_block(McmeArena *arena, const McmeTag *entry)
{
    if (arena == NULL || entry == NULL) {
        mcme_set_error("translate: null argument");
        return NULL;
    }
    if (entry->type != MCME_TAG_Compound) {
        mcme_set_error("translate: the palette entry is a %s, not a "
                       "compound", mcme_tag_type_name(entry->type));
        return NULL;
    }

    size_t name_len = 0;
    const char *name =
        string_value(mcme_nbt_get(entry, "name"), &name_len);
    if (name == NULL) {
        mcme_set_error("translate: the palette entry has no string "
                       "'name'");
        return NULL;
    }

    /* A legacy palette entry — pre-1.9 blocks, often {name, val} —
       has no Java 1.20 spelling this build knows. */
    if (mcme_nbt_get(entry, "version") == NULL) {
        mcme_set_error("translate: '%.*s' is a legacy palette entry "
                       "without a version; no Java spelling is known",
                       (int)name_len, name);
        return NULL;
    }

    const McmeTag *states = mcme_nbt_get(entry, "states");

    /* Red sand is a name, not a state, on the Java side. */
    if (strlen(kSand) == name_len
            && memcmp(name, kSand, name_len) == 0) {
        const McmeTag *sand = mcme_nbt_get(states, "sand_type");
        const char *sand_type = string_value(sand, NULL);
        if (sand_type != NULL && strcmp(sand_type, "red") == 0) {
            return named_entry(arena, kRedSand, sizeof kRedSand - 1);
        }
        /* Any other sand is the plain block: sand_type carries no
           Java state (default sand has none). */
        return named_entry(arena, name, name_len);
    }

    /* Redstone wire: the reference answers every Bedrock wire the
       same — four side connections and power 0 — so the map answers
       the same. Bedrock's own connection and powered states do not
       survive its universal mapping either. */
    if (strlen(kWire) == name_len
            && memcmp(name, kWire, name_len) == 0) {
        static const char *kWireProps[] = {
            "east", "north", "south", "west"
        };
        McmeTag *entry2 = named_entry(arena, name, name_len);
        McmeTag *props = mcme_nbt_new(arena, MCME_TAG_Compound);
        if (entry2 == NULL || props == NULL
                || !mcme_nbt_compound_init(arena, props, 5)) {
            return NULL;
        }
        for (size_t i = 0; i < 4; ++i) {
            McmeTag *v = mcme_nbt_new(arena, MCME_TAG_String);
            if (v == NULL
                    || !mcme_nbt_set_string(arena, v, "side", 4)
                    || !mcme_nbt_compound_put(arena, props,
                                              kWireProps[i],
                                              strlen(kWireProps[i]),
                                              v)) {
                return NULL;
            }
        }
        McmeTag *power = mcme_nbt_new(arena, MCME_TAG_String);
        if (power == NULL
                || !mcme_nbt_set_string(arena, power, "0", 1)
                || !mcme_nbt_compound_put(arena, props, "power", 5,
                                          power)
                || !mcme_nbt_compound_put(arena, entry2, "Properties",
                                          10, props)) {
            return NULL;
        }
        return entry2;
    }

    /* Everything else: the kept states, under Java's names. Any other
       state the entry carries is dropped and the block takes its Java
       default — the reference's own answer for unknown states. */
    bool is_axis_family = false;
    for (size_t s = 0;
         s < sizeof kAxisSuffixes / sizeof kAxisSuffixes[0]; ++s) {
        if (ends_with(name, name_len, kAxisSuffixes[s])) {
            is_axis_family = true;
            break;
        }
    }
    if (strlen(kMushroom) == name_len
            && memcmp(name, kMushroom, name_len) == 0) {
        is_axis_family = false;
    }

    McmeTag *out = named_entry(arena, name, name_len);
    McmeTag *props = mcme_nbt_new(arena, MCME_TAG_Compound);
    if (out == NULL || props == NULL
            || !mcme_nbt_compound_init(arena, props, 2)) {
        return NULL;
    }
    size_t kept = 0;

    for (size_t i = 0; i < sizeof kKeep / sizeof kKeep[0]; ++i) {
        if (strlen(kKeep[i].block) != name_len
                || memcmp(name, kKeep[i].block, name_len) != 0) {
            continue;
        }
        const McmeTag *v = mcme_nbt_get(states, kKeep[i].from);
        if (v == NULL) continue;
        if (!mcme_nbt_compound_put(arena, props, kKeep[i].to,
                                   strlen(kKeep[i].to),
                                   (McmeTag *)v)) {
            return NULL;
        }
        ++kept;
    }

    if (is_axis_family) {
        const McmeTag *v = mcme_nbt_get(states, "pillar_axis");
        if (v == NULL) {
            /* nothing to keep */
        } else if (!mcme_nbt_compound_put(arena, props, "axis", 4,
                                          (McmeTag *)v)) {
            return NULL;
        } else {
            ++kept;
        }
    }

    if (kept > 0
            && !mcme_nbt_compound_put(arena, out, "Properties", 10,
                                      props)) {
        return NULL;
    }
    return out;
}
