/*
 * The Bedrock 1.20 to Java 1.20 block map. The format facts live in
 * translate.h; this file is the table and the walking of it. The
 * name half between the GENERATED NAME MAP markers was enumerated
 * against PyMCTranslate for every block name the real world holds —
 * the LP world's whole store, all three dimensions — and each row
 * exists because a real palette or a real tick record needed it.
 * tools/ref_convert.py re-pins the table against the reference on
 * every battery run; the state half below stays hand-curated the
 * same way, growing when the next world needs a case.
 */

#include <string.h>

#include "mcme/translate.h"

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

/* ---- BEGIN GENERATED NAME MAP ----
 * Every Bedrock block name the real world holds that
 * Java 1.20 spells differently, enumerated against
 * PyMCTranslate (Bedrock 1.20.80 -> Java 1.20.0): no conditions is a wholesale rename, one
 * condition fires on a state value, two conditions on
 * a pair (a dead coral's colour, a cauldron's liquid
 * at a fill level). Rows are ordered most-specific
 * first within a name; the first row whose
 * conditions all match wins, else the name keeps its
 * own spelling (or the name's default row when it
 * has one). Every combination the world contains and
 * every small state product was checked against the
 * reference before this table was written --
 * decisions.md D17. tools/ref_convert.py re-pins it
 * against the reference on every battery run.
 * ---- */
struct NameMapRow {
    const char *from;
    const char *state;   /* NULL: no conditions, a wholesale rename */
    const char *sval;    /* string value; NULL with ival for a number */
    int32_t     ival;
    const char *state2;  /* second condition; NULL for a single-state row */
    const char *sval2;
    int32_t     ival2;
    const char *to;
};

static const struct NameMapRow kNameMap[] = {
    {"minecraft:acacia_double_slab", NULL, NULL, 0, NULL, NULL, 0, "minecraft:acacia_slab"},
    {"minecraft:anvil", "damage", "slightly_damaged", 0, NULL, NULL, 0, "minecraft:chipped_anvil"},
    {"minecraft:anvil", "damage", "very_damaged", 0, NULL, NULL, 0, "minecraft:damaged_anvil"},
    {"minecraft:azalea_leaves_flowered", NULL, NULL, 0, NULL, NULL, 0, "minecraft:flowering_azalea_leaves"},
    {"minecraft:bamboo_double_slab", NULL, NULL, 0, NULL, NULL, 0, "minecraft:bamboo_slab"},
    {"minecraft:bamboo_hanging_sign", "attached_bit", NULL, 1, NULL, NULL, 0, "minecraft:bamboo_hanging_sign"},
    {"minecraft:bamboo_hanging_sign", "hanging", NULL, 1, NULL, NULL, 0, "minecraft:bamboo_hanging_sign"},
    {"minecraft:bamboo_hanging_sign", NULL, NULL, 0, NULL, NULL, 0, "minecraft:bamboo_wall_hanging_sign"},
    {"minecraft:bamboo_mosaic_double_slab", NULL, NULL, 0, NULL, NULL, 0, "minecraft:bamboo_mosaic_slab"},
    {"minecraft:bamboo_standing_sign", NULL, NULL, 0, NULL, NULL, 0, "minecraft:bamboo_sign"},
    {"minecraft:bed", NULL, NULL, 0, NULL, NULL, 0, "minecraft:red_bed"},
    {"minecraft:beetroot", NULL, NULL, 0, NULL, NULL, 0, "minecraft:beetroots"},
    {"minecraft:big_dripleaf", "big_dripleaf_head", NULL, 1, NULL, NULL, 0, "minecraft:big_dripleaf"},
    {"minecraft:big_dripleaf", NULL, NULL, 0, NULL, NULL, 0, "minecraft:big_dripleaf_stem"},
    {"minecraft:birch_double_slab", NULL, NULL, 0, NULL, NULL, 0, "minecraft:birch_slab"},
    {"minecraft:brick_block", NULL, NULL, 0, NULL, NULL, 0, "minecraft:bricks"},
    {"minecraft:brown_mushroom_block", "huge_mushroom_bits", NULL, 10, NULL, NULL, 0, "minecraft:mushroom_stem"},
    {"minecraft:brown_mushroom_block", "huge_mushroom_bits", NULL, 15, NULL, NULL, 0, "minecraft:mushroom_stem"},
    {"minecraft:cauldron", "cauldron_liquid", "lava", 0, "fill_level", NULL, 1, "minecraft:lava_cauldron"},
    {"minecraft:cauldron", "cauldron_liquid", "lava", 0, "fill_level", NULL, 2, "minecraft:lava_cauldron"},
    {"minecraft:cauldron", "cauldron_liquid", "lava", 0, "fill_level", NULL, 3, "minecraft:lava_cauldron"},
    {"minecraft:cauldron", "cauldron_liquid", "lava", 0, "fill_level", NULL, 4, "minecraft:lava_cauldron"},
    {"minecraft:cauldron", "cauldron_liquid", "lava", 0, "fill_level", NULL, 5, "minecraft:lava_cauldron"},
    {"minecraft:cauldron", "cauldron_liquid", "lava", 0, "fill_level", NULL, 6, "minecraft:lava_cauldron"},
    {"minecraft:cauldron", "cauldron_liquid", "powder_snow", 0, "fill_level", NULL, 1, "minecraft:powder_snow_cauldron"},
    {"minecraft:cauldron", "cauldron_liquid", "powder_snow", 0, "fill_level", NULL, 2, "minecraft:powder_snow_cauldron"},
    {"minecraft:cauldron", "cauldron_liquid", "powder_snow", 0, "fill_level", NULL, 3, "minecraft:powder_snow_cauldron"},
    {"minecraft:cauldron", "cauldron_liquid", "powder_snow", 0, "fill_level", NULL, 4, "minecraft:powder_snow_cauldron"},
    {"minecraft:cauldron", "cauldron_liquid", "powder_snow", 0, "fill_level", NULL, 5, "minecraft:powder_snow_cauldron"},
    {"minecraft:cauldron", "cauldron_liquid", "powder_snow", 0, "fill_level", NULL, 6, "minecraft:powder_snow_cauldron"},
    {"minecraft:cauldron", "cauldron_liquid", "water", 0, "fill_level", NULL, 1, "minecraft:water_cauldron"},
    {"minecraft:cauldron", "cauldron_liquid", "water", 0, "fill_level", NULL, 2, "minecraft:water_cauldron"},
    {"minecraft:cauldron", "cauldron_liquid", "water", 0, "fill_level", NULL, 3, "minecraft:water_cauldron"},
    {"minecraft:cauldron", "cauldron_liquid", "water", 0, "fill_level", NULL, 4, "minecraft:water_cauldron"},
    {"minecraft:cauldron", "cauldron_liquid", "water", 0, "fill_level", NULL, 5, "minecraft:water_cauldron"},
    {"minecraft:cauldron", "cauldron_liquid", "water", 0, "fill_level", NULL, 6, "minecraft:water_cauldron"},
    {"minecraft:cave_vines_body_with_berries", NULL, NULL, 0, NULL, NULL, 0, "minecraft:cave_vines_plant"},
    {"minecraft:cave_vines_head_with_berries", NULL, NULL, 0, NULL, NULL, 0, "minecraft:cave_vines"},
    {"minecraft:chiseled_tuff", NULL, NULL, 0, NULL, NULL, 0, "minecraft:air"},
    {"minecraft:chiseled_tuff_bricks", NULL, NULL, 0, NULL, NULL, 0, "minecraft:air"},
    {"minecraft:cobbled_deepslate_double_slab", NULL, NULL, 0, NULL, NULL, 0, "minecraft:cobbled_deepslate_slab"},
    {"minecraft:cobblestone_wall", "wall_block_type", "andesite", 0, NULL, NULL, 0, "minecraft:andesite_wall"},
    {"minecraft:cobblestone_wall", "wall_block_type", "brick", 0, NULL, NULL, 0, "minecraft:brick_wall"},
    {"minecraft:cobblestone_wall", "wall_block_type", "diorite", 0, NULL, NULL, 0, "minecraft:diorite_wall"},
    {"minecraft:cobblestone_wall", "wall_block_type", "end_brick", 0, NULL, NULL, 0, "minecraft:end_stone_brick_wall"},
    {"minecraft:cobblestone_wall", "wall_block_type", "granite", 0, NULL, NULL, 0, "minecraft:granite_wall"},
    {"minecraft:cobblestone_wall", "wall_block_type", "mossy_cobblestone", 0, NULL, NULL, 0, "minecraft:mossy_cobblestone_wall"},
    {"minecraft:cobblestone_wall", "wall_block_type", "mossy_stone_brick", 0, NULL, NULL, 0, "minecraft:mossy_stone_brick_wall"},
    {"minecraft:cobblestone_wall", "wall_block_type", "nether_brick", 0, NULL, NULL, 0, "minecraft:nether_brick_wall"},
    {"minecraft:cobblestone_wall", "wall_block_type", "prismarine", 0, NULL, NULL, 0, "minecraft:prismarine_wall"},
    {"minecraft:cobblestone_wall", "wall_block_type", "red_nether_brick", 0, NULL, NULL, 0, "minecraft:red_nether_brick_wall"},
    {"minecraft:cobblestone_wall", "wall_block_type", "red_sandstone", 0, NULL, NULL, 0, "minecraft:red_sandstone_wall"},
    {"minecraft:cobblestone_wall", "wall_block_type", "sandstone", 0, NULL, NULL, 0, "minecraft:sandstone_wall"},
    {"minecraft:cobblestone_wall", "wall_block_type", "stone_brick", 0, NULL, NULL, 0, "minecraft:stone_brick_wall"},
    {"minecraft:coral_block", "coral_color", "blue", 0, "dead_bit", NULL, 1, "minecraft:dead_tube_coral_block"},
    {"minecraft:coral_block", "coral_color", "pink", 0, "dead_bit", NULL, 0, "minecraft:brain_coral_block"},
    {"minecraft:coral_block", "coral_color", "pink", 0, "dead_bit", NULL, 1, "minecraft:dead_brain_coral_block"},
    {"minecraft:coral_block", "coral_color", "purple", 0, "dead_bit", NULL, 0, "minecraft:bubble_coral_block"},
    {"minecraft:coral_block", "coral_color", "purple", 0, "dead_bit", NULL, 1, "minecraft:dead_bubble_coral_block"},
    {"minecraft:coral_block", "coral_color", "red", 0, "dead_bit", NULL, 0, "minecraft:fire_coral_block"},
    {"minecraft:coral_block", "coral_color", "red", 0, "dead_bit", NULL, 1, "minecraft:dead_fire_coral_block"},
    {"minecraft:coral_block", "coral_color", "yellow", 0, "dead_bit", NULL, 0, "minecraft:horn_coral_block"},
    {"minecraft:coral_block", "coral_color", "yellow", 0, "dead_bit", NULL, 1, "minecraft:dead_horn_coral_block"},
    {"minecraft:coral_block", NULL, NULL, 0, NULL, NULL, 0, "minecraft:tube_coral_block"},
    {"minecraft:coral_fan_hang", "coral_hang_type_bit", NULL, 0, "dead_bit", NULL, 1, "minecraft:dead_tube_coral_wall_fan"},
    {"minecraft:coral_fan_hang", "coral_hang_type_bit", NULL, 1, "dead_bit", NULL, 0, "minecraft:brain_coral_wall_fan"},
    {"minecraft:coral_fan_hang", "coral_hang_type_bit", NULL, 1, "dead_bit", NULL, 1, "minecraft:dead_brain_coral_wall_fan"},
    {"minecraft:coral_fan_hang", NULL, NULL, 0, NULL, NULL, 0, "minecraft:tube_coral_wall_fan"},
    {"minecraft:coral_fan_hang2", "coral_hang_type_bit", NULL, 0, "dead_bit", NULL, 1, "minecraft:dead_bubble_coral_wall_fan"},
    {"minecraft:coral_fan_hang2", "coral_hang_type_bit", NULL, 1, "dead_bit", NULL, 0, "minecraft:fire_coral_wall_fan"},
    {"minecraft:coral_fan_hang2", "coral_hang_type_bit", NULL, 1, "dead_bit", NULL, 1, "minecraft:dead_fire_coral_wall_fan"},
    {"minecraft:coral_fan_hang2", NULL, NULL, 0, NULL, NULL, 0, "minecraft:bubble_coral_wall_fan"},
    {"minecraft:coral_fan_hang3", "dead_bit", NULL, 1, NULL, NULL, 0, "minecraft:dead_horn_coral_wall_fan"},
    {"minecraft:coral_fan_hang3", NULL, NULL, 0, NULL, NULL, 0, "minecraft:horn_coral_wall_fan"},
    {"minecraft:crafter", NULL, NULL, 0, NULL, NULL, 0, "minecraft:air"},
    {"minecraft:crimson_double_slab", NULL, NULL, 0, NULL, NULL, 0, "minecraft:crimson_slab"},
    {"minecraft:dark_oak_double_slab", NULL, NULL, 0, NULL, NULL, 0, "minecraft:dark_oak_slab"},
    {"minecraft:daylight_detector_inverted", NULL, NULL, 0, NULL, NULL, 0, "minecraft:daylight_detector"},
    {"minecraft:deadbush", NULL, NULL, 0, NULL, NULL, 0, "minecraft:dead_bush"},
    {"minecraft:deepslate_brick_double_slab", NULL, NULL, 0, NULL, NULL, 0, "minecraft:deepslate_brick_slab"},
    {"minecraft:deepslate_tile_double_slab", NULL, NULL, 0, NULL, NULL, 0, "minecraft:deepslate_tile_slab"},
    {"minecraft:dirt", "dirt_type", "coarse", 0, NULL, NULL, 0, "minecraft:coarse_dirt"},
    {"minecraft:dirt_with_roots", NULL, NULL, 0, NULL, NULL, 0, "minecraft:rooted_dirt"},
    {"minecraft:double_cut_copper_slab", NULL, NULL, 0, NULL, NULL, 0, "minecraft:cut_copper_slab"},
    {"minecraft:double_plant", "double_plant_type", "fern", 0, NULL, NULL, 0, "minecraft:large_fern"},
    {"minecraft:double_plant", "double_plant_type", "grass", 0, NULL, NULL, 0, "minecraft:tall_grass"},
    {"minecraft:double_plant", "double_plant_type", "paeonia", 0, NULL, NULL, 0, "minecraft:peony"},
    {"minecraft:double_plant", "double_plant_type", "rose", 0, NULL, NULL, 0, "minecraft:rose_bush"},
    {"minecraft:double_plant", "double_plant_type", "syringa", 0, NULL, NULL, 0, "minecraft:lilac"},
    {"minecraft:double_plant", NULL, NULL, 0, NULL, NULL, 0, "minecraft:sunflower"},
    {"minecraft:double_stone_block_slab", "stone_slab_type", "brick", 0, NULL, NULL, 0, "minecraft:brick_slab"},
    {"minecraft:double_stone_block_slab", "stone_slab_type", "cobblestone", 0, NULL, NULL, 0, "minecraft:cobblestone_slab"},
    {"minecraft:double_stone_block_slab", "stone_slab_type", "nether_brick", 0, NULL, NULL, 0, "minecraft:nether_brick_slab"},
    {"minecraft:double_stone_block_slab", "stone_slab_type", "quartz", 0, NULL, NULL, 0, "minecraft:quartz_slab"},
    {"minecraft:double_stone_block_slab", "stone_slab_type", "sandstone", 0, NULL, NULL, 0, "minecraft:sandstone_slab"},
    {"minecraft:double_stone_block_slab", "stone_slab_type", "stone_brick", 0, NULL, NULL, 0, "minecraft:stone_brick_slab"},
    {"minecraft:double_stone_block_slab", "stone_slab_type", "wood", 0, NULL, NULL, 0, "minecraft:petrified_oak_slab"},
    {"minecraft:double_stone_block_slab", NULL, NULL, 0, NULL, NULL, 0, "minecraft:smooth_stone_slab"},
    {"minecraft:double_stone_block_slab2", "stone_slab_type_2", "mossy_cobblestone", 0, NULL, NULL, 0, "minecraft:mossy_cobblestone_slab"},
    {"minecraft:double_stone_block_slab2", "stone_slab_type_2", "prismarine_brick", 0, NULL, NULL, 0, "minecraft:prismarine_brick_slab"},
    {"minecraft:double_stone_block_slab2", "stone_slab_type_2", "prismarine_dark", 0, NULL, NULL, 0, "minecraft:dark_prismarine_slab"},
    {"minecraft:double_stone_block_slab2", "stone_slab_type_2", "prismarine_rough", 0, NULL, NULL, 0, "minecraft:prismarine_slab"},
    {"minecraft:double_stone_block_slab2", "stone_slab_type_2", "purpur", 0, NULL, NULL, 0, "minecraft:purpur_slab"},
    {"minecraft:double_stone_block_slab2", "stone_slab_type_2", "red_nether_brick", 0, NULL, NULL, 0, "minecraft:red_nether_brick_slab"},
    {"minecraft:double_stone_block_slab2", "stone_slab_type_2", "smooth_sandstone", 0, NULL, NULL, 0, "minecraft:smooth_sandstone_slab"},
    {"minecraft:double_stone_block_slab2", NULL, NULL, 0, NULL, NULL, 0, "minecraft:red_sandstone_slab"},
    {"minecraft:double_stone_block_slab3", "stone_slab_type_3", "andesite", 0, NULL, NULL, 0, "minecraft:andesite_slab"},
    {"minecraft:double_stone_block_slab3", "stone_slab_type_3", "diorite", 0, NULL, NULL, 0, "minecraft:diorite_slab"},
    {"minecraft:double_stone_block_slab3", "stone_slab_type_3", "granite", 0, NULL, NULL, 0, "minecraft:granite_slab"},
    {"minecraft:double_stone_block_slab3", "stone_slab_type_3", "polished_andesite", 0, NULL, NULL, 0, "minecraft:polished_andesite_slab"},
    {"minecraft:double_stone_block_slab3", "stone_slab_type_3", "polished_diorite", 0, NULL, NULL, 0, "minecraft:polished_diorite_slab"},
    {"minecraft:double_stone_block_slab3", "stone_slab_type_3", "polished_granite", 0, NULL, NULL, 0, "minecraft:polished_granite_slab"},
    {"minecraft:double_stone_block_slab3", "stone_slab_type_3", "smooth_red_sandstone", 0, NULL, NULL, 0, "minecraft:smooth_red_sandstone_slab"},
    {"minecraft:double_stone_block_slab3", NULL, NULL, 0, NULL, NULL, 0, "minecraft:end_stone_brick_slab"},
    {"minecraft:double_stone_block_slab4", "stone_slab_type_4", "cut_red_sandstone", 0, NULL, NULL, 0, "minecraft:cut_red_sandstone_slab"},
    {"minecraft:double_stone_block_slab4", "stone_slab_type_4", "cut_sandstone", 0, NULL, NULL, 0, "minecraft:cut_sandstone_slab"},
    {"minecraft:double_stone_block_slab4", "stone_slab_type_4", "smooth_quartz", 0, NULL, NULL, 0, "minecraft:smooth_quartz_slab"},
    {"minecraft:double_stone_block_slab4", "stone_slab_type_4", "stone", 0, NULL, NULL, 0, "minecraft:stone_slab"},
    {"minecraft:double_stone_block_slab4", NULL, NULL, 0, NULL, NULL, 0, "minecraft:mossy_stone_brick_slab"},
    {"minecraft:end_bricks", NULL, NULL, 0, NULL, NULL, 0, "minecraft:end_stone_bricks"},
    {"minecraft:exposed_double_cut_copper_slab", NULL, NULL, 0, NULL, NULL, 0, "minecraft:exposed_cut_copper_slab"},
    {"minecraft:fence", "wood_type", "acacia", 0, NULL, NULL, 0, "minecraft:acacia_fence"},
    {"minecraft:fence", "wood_type", "birch", 0, NULL, NULL, 0, "minecraft:birch_fence"},
    {"minecraft:fence", "wood_type", "dark_oak", 0, NULL, NULL, 0, "minecraft:dark_oak_fence"},
    {"minecraft:fence", "wood_type", "jungle", 0, NULL, NULL, 0, "minecraft:jungle_fence"},
    {"minecraft:fence", "wood_type", "spruce", 0, NULL, NULL, 0, "minecraft:spruce_fence"},
    {"minecraft:fence", NULL, NULL, 0, NULL, NULL, 0, "minecraft:oak_fence"},
    {"minecraft:fence_gate", NULL, NULL, 0, NULL, NULL, 0, "minecraft:oak_fence_gate"},
    {"minecraft:flowing_lava", NULL, NULL, 0, NULL, NULL, 0, "minecraft:lava"},
    {"minecraft:flowing_water", NULL, NULL, 0, NULL, NULL, 0, "minecraft:water"},
    {"minecraft:frame", NULL, NULL, 0, NULL, NULL, 0, "minecraft:air"},
    {"minecraft:frog_spawn", NULL, NULL, 0, NULL, NULL, 0, "minecraft:frogspawn"},
    {"minecraft:golden_rail", NULL, NULL, 0, NULL, NULL, 0, "minecraft:powered_rail"},
    {"minecraft:grass_path", NULL, NULL, 0, NULL, NULL, 0, "minecraft:dirt_path"},
    {"minecraft:hardened_clay", NULL, NULL, 0, NULL, NULL, 0, "minecraft:terracotta"},
    {"minecraft:jungle_double_slab", NULL, NULL, 0, NULL, NULL, 0, "minecraft:jungle_slab"},
    {"minecraft:lit_deepslate_redstone_ore", NULL, NULL, 0, NULL, NULL, 0, "minecraft:deepslate_redstone_ore"},
    {"minecraft:lit_furnace", NULL, NULL, 0, NULL, NULL, 0, "minecraft:furnace"},
    {"minecraft:lit_pumpkin", NULL, NULL, 0, NULL, NULL, 0, "minecraft:jack_o_lantern"},
    {"minecraft:lit_redstone_lamp", NULL, NULL, 0, NULL, NULL, 0, "minecraft:redstone_lamp"},
    {"minecraft:lit_redstone_ore", NULL, NULL, 0, NULL, NULL, 0, "minecraft:redstone_ore"},
    {"minecraft:lit_smoker", NULL, NULL, 0, NULL, NULL, 0, "minecraft:smoker"},
    {"minecraft:log", "old_log_type", "birch", 0, NULL, NULL, 0, "minecraft:birch_log"},
    {"minecraft:log", "old_log_type", "jungle", 0, NULL, NULL, 0, "minecraft:jungle_log"},
    {"minecraft:log", "old_log_type", "spruce", 0, NULL, NULL, 0, "minecraft:spruce_log"},
    {"minecraft:log", NULL, NULL, 0, NULL, NULL, 0, "minecraft:oak_log"},
    {"minecraft:log2", "new_log_type", "dark_oak", 0, NULL, NULL, 0, "minecraft:dark_oak_log"},
    {"minecraft:log2", NULL, NULL, 0, NULL, NULL, 0, "minecraft:acacia_log"},
    {"minecraft:magma", NULL, NULL, 0, NULL, NULL, 0, "minecraft:magma_block"},
    {"minecraft:mangrove_double_slab", NULL, NULL, 0, NULL, NULL, 0, "minecraft:mangrove_slab"},
    {"minecraft:mangrove_standing_sign", NULL, NULL, 0, NULL, NULL, 0, "minecraft:mangrove_sign"},
    {"minecraft:melon_block", NULL, NULL, 0, NULL, NULL, 0, "minecraft:melon"},
    {"minecraft:melon_stem", "facing_direction", NULL, 2, NULL, NULL, 0, "minecraft:attached_melon_stem"},
    {"minecraft:melon_stem", "facing_direction", NULL, 3, NULL, NULL, 0, "minecraft:attached_melon_stem"},
    {"minecraft:melon_stem", "facing_direction", NULL, 4, NULL, NULL, 0, "minecraft:attached_melon_stem"},
    {"minecraft:melon_stem", "facing_direction", NULL, 5, NULL, NULL, 0, "minecraft:attached_melon_stem"},
    {"minecraft:mob_spawner", NULL, NULL, 0, NULL, NULL, 0, "minecraft:spawner"},
    {"minecraft:monster_egg", "monster_egg_stone_type", "chiseled_stone_brick", 0, NULL, NULL, 0, "minecraft:infested_chiseled_stone_bricks"},
    {"minecraft:monster_egg", "monster_egg_stone_type", "cobblestone", 0, NULL, NULL, 0, "minecraft:infested_cobblestone"},
    {"minecraft:monster_egg", "monster_egg_stone_type", "cracked_stone_brick", 0, NULL, NULL, 0, "minecraft:infested_cracked_stone_bricks"},
    {"minecraft:monster_egg", "monster_egg_stone_type", "mossy_stone_brick", 0, NULL, NULL, 0, "minecraft:infested_mossy_stone_bricks"},
    {"minecraft:monster_egg", "monster_egg_stone_type", "stone_brick", 0, NULL, NULL, 0, "minecraft:infested_stone_bricks"},
    {"minecraft:monster_egg", NULL, NULL, 0, NULL, NULL, 0, "minecraft:infested_stone"},
    {"minecraft:moving_block", NULL, NULL, 0, NULL, NULL, 0, "minecraft:moving_piston"},
    {"minecraft:mud_brick_double_slab", NULL, NULL, 0, NULL, NULL, 0, "minecraft:mud_brick_slab"},
    {"minecraft:nether_brick", NULL, NULL, 0, NULL, NULL, 0, "minecraft:nether_bricks"},
    {"minecraft:normal_stone_stairs", NULL, NULL, 0, NULL, NULL, 0, "minecraft:stone_stairs"},
    {"minecraft:noteblock", NULL, NULL, 0, NULL, NULL, 0, "minecraft:note_block"},
    {"minecraft:oak_double_slab", NULL, NULL, 0, NULL, NULL, 0, "minecraft:oak_slab"},
    {"minecraft:oxidized_copper_trapdoor", NULL, NULL, 0, NULL, NULL, 0, "minecraft:oak_trapdoor"},
    {"minecraft:oxidized_double_cut_copper_slab", NULL, NULL, 0, NULL, NULL, 0, "minecraft:oxidized_cut_copper_slab"},
    {"minecraft:piston_arm_collision", NULL, NULL, 0, NULL, NULL, 0, "minecraft:piston_head"},
    {"minecraft:polished_blackstone_brick_double_slab", NULL, NULL, 0, NULL, NULL, 0, "minecraft:polished_blackstone_brick_slab"},
    {"minecraft:polished_blackstone_double_slab", NULL, NULL, 0, NULL, NULL, 0, "minecraft:polished_blackstone_slab"},
    {"minecraft:polished_deepslate_double_slab", NULL, NULL, 0, NULL, NULL, 0, "minecraft:polished_deepslate_slab"},
    {"minecraft:polished_tuff", NULL, NULL, 0, NULL, NULL, 0, "minecraft:air"},
    {"minecraft:polished_tuff_slab", NULL, NULL, 0, NULL, NULL, 0, "minecraft:stone_slab"},
    {"minecraft:portal", NULL, NULL, 0, NULL, NULL, 0, "minecraft:nether_portal"},
    {"minecraft:powered_comparator", NULL, NULL, 0, NULL, NULL, 0, "minecraft:comparator"},
    {"minecraft:powered_repeater", NULL, NULL, 0, NULL, NULL, 0, "minecraft:repeater"},
    {"minecraft:prismarine", "prismarine_block_type", "bricks", 0, NULL, NULL, 0, "minecraft:prismarine_bricks"},
    {"minecraft:prismarine", "prismarine_block_type", "dark", 0, NULL, NULL, 0, "minecraft:dark_prismarine"},
    {"minecraft:prismarine_bricks_stairs", NULL, NULL, 0, NULL, NULL, 0, "minecraft:prismarine_brick_stairs"},
    {"minecraft:pumpkin_stem", "facing_direction", NULL, 2, NULL, NULL, 0, "minecraft:attached_pumpkin_stem"},
    {"minecraft:pumpkin_stem", "facing_direction", NULL, 3, NULL, NULL, 0, "minecraft:attached_pumpkin_stem"},
    {"minecraft:pumpkin_stem", "facing_direction", NULL, 4, NULL, NULL, 0, "minecraft:attached_pumpkin_stem"},
    {"minecraft:pumpkin_stem", "facing_direction", NULL, 5, NULL, NULL, 0, "minecraft:attached_pumpkin_stem"},
    {"minecraft:purpur_block", "chisel_type", "chiseled", 0, NULL, NULL, 0, "minecraft:air"},
    {"minecraft:purpur_block", "chisel_type", "lines", 0, NULL, NULL, 0, "minecraft:purpur_pillar"},
    {"minecraft:purpur_block", "chisel_type", "smooth", 0, NULL, NULL, 0, "minecraft:air"},
    {"minecraft:quartz_block", "chisel_type", "chiseled", 0, NULL, NULL, 0, "minecraft:chiseled_quartz_block"},
    {"minecraft:quartz_block", "chisel_type", "lines", 0, NULL, NULL, 0, "minecraft:quartz_pillar"},
    {"minecraft:quartz_block", "chisel_type", "smooth", 0, NULL, NULL, 0, "minecraft:smooth_quartz"},
    {"minecraft:quartz_ore", NULL, NULL, 0, NULL, NULL, 0, "minecraft:nether_quartz_ore"},
    {"minecraft:red_mushroom_block", "huge_mushroom_bits", NULL, 10, NULL, NULL, 0, "minecraft:mushroom_stem"},
    {"minecraft:red_mushroom_block", "huge_mushroom_bits", NULL, 15, NULL, NULL, 0, "minecraft:mushroom_stem"},
    {"minecraft:red_nether_brick", NULL, NULL, 0, NULL, NULL, 0, "minecraft:red_nether_bricks"},
    {"minecraft:red_sandstone", "sand_stone_type", "cut", 0, NULL, NULL, 0, "minecraft:cut_red_sandstone"},
    {"minecraft:red_sandstone", "sand_stone_type", "heiroglyphs", 0, NULL, NULL, 0, "minecraft:chiseled_red_sandstone"},
    {"minecraft:red_sandstone", "sand_stone_type", "smooth", 0, NULL, NULL, 0, "minecraft:smooth_red_sandstone"},
    {"minecraft:redstone_torch", "torch_facing_direction", "east", 0, NULL, NULL, 0, "minecraft:redstone_wall_torch"},
    {"minecraft:redstone_torch", "torch_facing_direction", "north", 0, NULL, NULL, 0, "minecraft:redstone_wall_torch"},
    {"minecraft:redstone_torch", "torch_facing_direction", "south", 0, NULL, NULL, 0, "minecraft:redstone_wall_torch"},
    {"minecraft:redstone_torch", "torch_facing_direction", "west", 0, NULL, NULL, 0, "minecraft:redstone_wall_torch"},
    {"minecraft:reeds", NULL, NULL, 0, NULL, NULL, 0, "minecraft:sugar_cane"},
    {"minecraft:sand", "sand_type", "red", 0, NULL, NULL, 0, "minecraft:red_sand"},
    {"minecraft:sandstone", "sand_stone_type", "cut", 0, NULL, NULL, 0, "minecraft:cut_sandstone"},
    {"minecraft:sandstone", "sand_stone_type", "heiroglyphs", 0, NULL, NULL, 0, "minecraft:chiseled_sandstone"},
    {"minecraft:sandstone", "sand_stone_type", "smooth", 0, NULL, NULL, 0, "minecraft:smooth_sandstone"},
    {"minecraft:seaLantern", NULL, NULL, 0, NULL, NULL, 0, "minecraft:sea_lantern"},
    {"minecraft:seagrass", "sea_grass_type", "double_bot", 0, NULL, NULL, 0, "minecraft:tall_seagrass"},
    {"minecraft:seagrass", "sea_grass_type", "double_top", 0, NULL, NULL, 0, "minecraft:tall_seagrass"},
    {"minecraft:silver_glazed_terracotta", NULL, NULL, 0, NULL, NULL, 0, "minecraft:light_gray_glazed_terracotta"},
    {"minecraft:skull", "facing_direction", NULL, 2, NULL, NULL, 0, "minecraft:skeleton_wall_skull"},
    {"minecraft:skull", "facing_direction", NULL, 3, NULL, NULL, 0, "minecraft:skeleton_wall_skull"},
    {"minecraft:skull", "facing_direction", NULL, 4, NULL, NULL, 0, "minecraft:skeleton_wall_skull"},
    {"minecraft:skull", "facing_direction", NULL, 5, NULL, NULL, 0, "minecraft:skeleton_wall_skull"},
    {"minecraft:skull", NULL, NULL, 0, NULL, NULL, 0, "minecraft:skeleton_skull"},
    {"minecraft:slime", NULL, NULL, 0, NULL, NULL, 0, "minecraft:slime_block"},
    {"minecraft:small_dripleaf_block", NULL, NULL, 0, NULL, NULL, 0, "minecraft:small_dripleaf"},
    {"minecraft:snow", NULL, NULL, 0, NULL, NULL, 0, "minecraft:snow_block"},
    {"minecraft:snow_layer", NULL, NULL, 0, NULL, NULL, 0, "minecraft:snow"},
    {"minecraft:soul_torch", "torch_facing_direction", "east", 0, NULL, NULL, 0, "minecraft:soul_wall_torch"},
    {"minecraft:soul_torch", "torch_facing_direction", "north", 0, NULL, NULL, 0, "minecraft:soul_wall_torch"},
    {"minecraft:soul_torch", "torch_facing_direction", "south", 0, NULL, NULL, 0, "minecraft:soul_wall_torch"},
    {"minecraft:soul_torch", "torch_facing_direction", "west", 0, NULL, NULL, 0, "minecraft:soul_wall_torch"},
    {"minecraft:sponge", "sponge_type", "wet", 0, NULL, NULL, 0, "minecraft:wet_sponge"},
    {"minecraft:spruce_double_slab", NULL, NULL, 0, NULL, NULL, 0, "minecraft:spruce_slab"},
    {"minecraft:standing_banner", NULL, NULL, 0, NULL, NULL, 0, "minecraft:white_banner"},
    {"minecraft:standing_sign", NULL, NULL, 0, NULL, NULL, 0, "minecraft:oak_sign"},
    {"minecraft:sticky_piston_arm_collision", NULL, NULL, 0, NULL, NULL, 0, "minecraft:piston_head"},
    {"minecraft:stone_block_slab", "stone_slab_type", "brick", 0, NULL, NULL, 0, "minecraft:brick_slab"},
    {"minecraft:stone_block_slab", "stone_slab_type", "cobblestone", 0, NULL, NULL, 0, "minecraft:cobblestone_slab"},
    {"minecraft:stone_block_slab", "stone_slab_type", "nether_brick", 0, NULL, NULL, 0, "minecraft:nether_brick_slab"},
    {"minecraft:stone_block_slab", "stone_slab_type", "quartz", 0, NULL, NULL, 0, "minecraft:quartz_slab"},
    {"minecraft:stone_block_slab", "stone_slab_type", "sandstone", 0, NULL, NULL, 0, "minecraft:sandstone_slab"},
    {"minecraft:stone_block_slab", "stone_slab_type", "stone_brick", 0, NULL, NULL, 0, "minecraft:stone_brick_slab"},
    {"minecraft:stone_block_slab", "stone_slab_type", "wood", 0, NULL, NULL, 0, "minecraft:petrified_oak_slab"},
    {"minecraft:stone_block_slab", NULL, NULL, 0, NULL, NULL, 0, "minecraft:smooth_stone_slab"},
    {"minecraft:stone_block_slab2", "stone_slab_type_2", "mossy_cobblestone", 0, NULL, NULL, 0, "minecraft:mossy_cobblestone_slab"},
    {"minecraft:stone_block_slab2", "stone_slab_type_2", "prismarine_brick", 0, NULL, NULL, 0, "minecraft:prismarine_brick_slab"},
    {"minecraft:stone_block_slab2", "stone_slab_type_2", "prismarine_dark", 0, NULL, NULL, 0, "minecraft:dark_prismarine_slab"},
    {"minecraft:stone_block_slab2", "stone_slab_type_2", "prismarine_rough", 0, NULL, NULL, 0, "minecraft:prismarine_slab"},
    {"minecraft:stone_block_slab2", "stone_slab_type_2", "purpur", 0, NULL, NULL, 0, "minecraft:purpur_slab"},
    {"minecraft:stone_block_slab2", "stone_slab_type_2", "red_nether_brick", 0, NULL, NULL, 0, "minecraft:red_nether_brick_slab"},
    {"minecraft:stone_block_slab2", "stone_slab_type_2", "smooth_sandstone", 0, NULL, NULL, 0, "minecraft:smooth_sandstone_slab"},
    {"minecraft:stone_block_slab2", NULL, NULL, 0, NULL, NULL, 0, "minecraft:red_sandstone_slab"},
    {"minecraft:stone_block_slab3", "stone_slab_type_3", "andesite", 0, NULL, NULL, 0, "minecraft:andesite_slab"},
    {"minecraft:stone_block_slab3", "stone_slab_type_3", "diorite", 0, NULL, NULL, 0, "minecraft:diorite_slab"},
    {"minecraft:stone_block_slab3", "stone_slab_type_3", "granite", 0, NULL, NULL, 0, "minecraft:granite_slab"},
    {"minecraft:stone_block_slab3", "stone_slab_type_3", "polished_andesite", 0, NULL, NULL, 0, "minecraft:polished_andesite_slab"},
    {"minecraft:stone_block_slab3", "stone_slab_type_3", "polished_diorite", 0, NULL, NULL, 0, "minecraft:polished_diorite_slab"},
    {"minecraft:stone_block_slab3", "stone_slab_type_3", "polished_granite", 0, NULL, NULL, 0, "minecraft:polished_granite_slab"},
    {"minecraft:stone_block_slab3", "stone_slab_type_3", "smooth_red_sandstone", 0, NULL, NULL, 0, "minecraft:smooth_red_sandstone_slab"},
    {"minecraft:stone_block_slab3", NULL, NULL, 0, NULL, NULL, 0, "minecraft:end_stone_brick_slab"},
    {"minecraft:stone_block_slab4", "stone_slab_type_4", "cut_red_sandstone", 0, NULL, NULL, 0, "minecraft:cut_red_sandstone_slab"},
    {"minecraft:stone_block_slab4", "stone_slab_type_4", "cut_sandstone", 0, NULL, NULL, 0, "minecraft:cut_sandstone_slab"},
    {"minecraft:stone_block_slab4", "stone_slab_type_4", "smooth_quartz", 0, NULL, NULL, 0, "minecraft:smooth_quartz_slab"},
    {"minecraft:stone_block_slab4", "stone_slab_type_4", "stone", 0, NULL, NULL, 0, "minecraft:stone_slab"},
    {"minecraft:stone_block_slab4", NULL, NULL, 0, NULL, NULL, 0, "minecraft:mossy_stone_brick_slab"},
    {"minecraft:stone_stairs", NULL, NULL, 0, NULL, NULL, 0, "minecraft:cobblestone_stairs"},
    {"minecraft:stonebrick", "stone_brick_type", "chiseled", 0, NULL, NULL, 0, "minecraft:chiseled_stone_bricks"},
    {"minecraft:stonebrick", "stone_brick_type", "cracked", 0, NULL, NULL, 0, "minecraft:cracked_stone_bricks"},
    {"minecraft:stonebrick", "stone_brick_type", "mossy", 0, NULL, NULL, 0, "minecraft:mossy_stone_bricks"},
    {"minecraft:stonebrick", NULL, NULL, 0, NULL, NULL, 0, "minecraft:stone_bricks"},
    {"minecraft:stonecutter_block", NULL, NULL, 0, NULL, NULL, 0, "minecraft:stonecutter"},
    {"minecraft:tallgrass", "tall_grass_type", "fern", 0, NULL, NULL, 0, "minecraft:fern"},
    {"minecraft:tallgrass", "tall_grass_type", "snow", 0, NULL, NULL, 0, "minecraft:fern"},
    {"minecraft:tallgrass", NULL, NULL, 0, NULL, NULL, 0, "minecraft:grass"},
    {"minecraft:torch", "torch_facing_direction", "east", 0, NULL, NULL, 0, "minecraft:wall_torch"},
    {"minecraft:torch", "torch_facing_direction", "north", 0, NULL, NULL, 0, "minecraft:wall_torch"},
    {"minecraft:torch", "torch_facing_direction", "south", 0, NULL, NULL, 0, "minecraft:wall_torch"},
    {"minecraft:torch", "torch_facing_direction", "west", 0, NULL, NULL, 0, "minecraft:wall_torch"},
    {"minecraft:trapdoor", NULL, NULL, 0, NULL, NULL, 0, "minecraft:oak_trapdoor"},
    {"minecraft:trial_spawner", NULL, NULL, 0, NULL, NULL, 0, "minecraft:air"},
    {"minecraft:trip_wire", NULL, NULL, 0, NULL, NULL, 0, "minecraft:tripwire"},
    {"minecraft:tuff_bricks", NULL, NULL, 0, NULL, NULL, 0, "minecraft:air"},
    {"minecraft:undyed_shulker_box", NULL, NULL, 0, NULL, NULL, 0, "minecraft:shulker_box"},
    {"minecraft:unlit_redstone_torch", "torch_facing_direction", "east", 0, NULL, NULL, 0, "minecraft:redstone_wall_torch"},
    {"minecraft:unlit_redstone_torch", "torch_facing_direction", "north", 0, NULL, NULL, 0, "minecraft:redstone_wall_torch"},
    {"minecraft:unlit_redstone_torch", "torch_facing_direction", "south", 0, NULL, NULL, 0, "minecraft:redstone_wall_torch"},
    {"minecraft:unlit_redstone_torch", "torch_facing_direction", "west", 0, NULL, NULL, 0, "minecraft:redstone_wall_torch"},
    {"minecraft:unlit_redstone_torch", NULL, NULL, 0, NULL, NULL, 0, "minecraft:redstone_torch"},
    {"minecraft:unpowered_comparator", NULL, NULL, 0, NULL, NULL, 0, "minecraft:comparator"},
    {"minecraft:unpowered_repeater", NULL, NULL, 0, NULL, NULL, 0, "minecraft:repeater"},
    {"minecraft:vault", NULL, NULL, 0, NULL, NULL, 0, "minecraft:air"},
    {"minecraft:wall_banner", NULL, NULL, 0, NULL, NULL, 0, "minecraft:white_wall_banner"},
    {"minecraft:wall_sign", NULL, NULL, 0, NULL, NULL, 0, "minecraft:oak_wall_sign"},
    {"minecraft:warped_double_slab", NULL, NULL, 0, NULL, NULL, 0, "minecraft:warped_slab"},
    {"minecraft:waterlily", NULL, NULL, 0, NULL, NULL, 0, "minecraft:lily_pad"},
    {"minecraft:waxed_chiseled_copper", NULL, NULL, 0, NULL, NULL, 0, "minecraft:air"},
    {"minecraft:waxed_copper", NULL, NULL, 0, NULL, NULL, 0, "minecraft:waxed_copper_block"},
    {"minecraft:waxed_copper_bulb", NULL, NULL, 0, NULL, NULL, 0, "minecraft:air"},
    {"minecraft:waxed_copper_door", NULL, NULL, 0, NULL, NULL, 0, "minecraft:oak_door"},
    {"minecraft:waxed_copper_grate", NULL, NULL, 0, NULL, NULL, 0, "minecraft:air"},
    {"minecraft:waxed_double_cut_copper_slab", NULL, NULL, 0, NULL, NULL, 0, "minecraft:waxed_cut_copper_slab"},
    {"minecraft:waxed_exposed_copper_bulb", NULL, NULL, 0, NULL, NULL, 0, "minecraft:air"},
    {"minecraft:waxed_exposed_double_cut_copper_slab", NULL, NULL, 0, NULL, NULL, 0, "minecraft:waxed_exposed_cut_copper_slab"},
    {"minecraft:waxed_oxidized_chiseled_copper", NULL, NULL, 0, NULL, NULL, 0, "minecraft:air"},
    {"minecraft:waxed_oxidized_copper_bulb", NULL, NULL, 0, NULL, NULL, 0, "minecraft:air"},
    {"minecraft:waxed_oxidized_copper_door", NULL, NULL, 0, NULL, NULL, 0, "minecraft:oak_door"},
    {"minecraft:waxed_oxidized_copper_grate", NULL, NULL, 0, NULL, NULL, 0, "minecraft:air"},
    {"minecraft:waxed_oxidized_copper_trapdoor", NULL, NULL, 0, NULL, NULL, 0, "minecraft:oak_trapdoor"},
    {"minecraft:waxed_weathered_copper_bulb", NULL, NULL, 0, NULL, NULL, 0, "minecraft:air"},
    {"minecraft:web", NULL, NULL, 0, NULL, NULL, 0, "minecraft:cobweb"},
    {"minecraft:wooden_button", NULL, NULL, 0, NULL, NULL, 0, "minecraft:oak_button"},
    {"minecraft:wooden_door", NULL, NULL, 0, NULL, NULL, 0, "minecraft:oak_door"},
    {"minecraft:wooden_pressure_plate", NULL, NULL, 0, NULL, NULL, 0, "minecraft:oak_pressure_plate"},
    {"minecraft:yellow_flower", NULL, NULL, 0, NULL, NULL, 0, "minecraft:dandelion"},
};
/* ---- END GENERATED NAME MAP ---- */

/* The name map's lookup. Rows are grouped by `from` in byte order
   (the order the generator wrote them), a group runs most conditions
   first, and the first row whose conditions all hold answers the
   entry: a row with no conditions is the name's own default and
   sits last. Anything the table does not answer keeps its name —
   the reference's answer for a block it does not rename. */

static int name_cmp_from(const char *lit, const char *name, size_t len)
{
    size_t n = strlen(lit);
    size_t m = (n < len) ? n : len;
    int c = (m > 0) ? memcmp(lit, name, m) : 0;
    if (c != 0) return c;
    if (n == len) return 0;
    return (n < len) ? -1 : 1;
}

static bool name_value_matches(const char *sval, int32_t ival,
                               const McmeTag *v)
{
    if (v == NULL) return false;
    if (sval != NULL) {
        size_t vl = 0;
        const char *s = string_value(v, &vl);
        return s != NULL && vl == strlen(sval)
            && memcmp(s, sval, vl) == 0;
    }
    switch (v->type) {
    case MCME_TAG_Byte:
    case MCME_TAG_Short:
    case MCME_TAG_Int:
    case MCME_TAG_Long:
        return mcme_nbt_int_or(v, 0) == ival;
    default:
        return false;
    }
}

static void resolve_java_name(const char *name, size_t name_len,
                              const McmeTag *states,
                              const char **out, size_t *out_len)
{
    size_t rows = sizeof kNameMap / sizeof kNameMap[0];
    size_t lo = 0, hi = rows;
    while (lo < hi) {                       /* lower bound of name */
        size_t mid = lo + (hi - lo) / 2;
        if (name_cmp_from(kNameMap[mid].from, name, name_len) < 0)
            lo = mid + 1;
        else
            hi = mid;
    }
    for (size_t i = lo; i < rows
             && name_cmp_from(kNameMap[i].from, name, name_len) == 0;
         ++i) {
        const struct NameMapRow *r = &kNameMap[i];
        if (r->state == NULL) {             /* the name's default */
            *out = r->to;
            *out_len = strlen(r->to);
            return;
        }
        if (!name_value_matches(r->sval, r->ival,
                                mcme_nbt_get(states, r->state))) {
            continue;
        }
        if (r->state2 != NULL
                && !name_value_matches(r->sval2, r->ival2,
                                       mcme_nbt_get(states,
                                                    r->state2))) {
            continue;
        }
        *out = r->to;
        *out_len = strlen(r->to);
        return;
    }
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

    /* The name half of the map: Java spells some of these blocks
       differently, and the spelling can hang off a state value —
       red sand, a lilac, a spruce log. The table below says which
       names change and on what; an entry it does not know keeps its
       own name, which is the reference's answer too. */
    const char *jname = name;
    size_t jname_len = name_len;
    resolve_java_name(name, name_len, states, &jname, &jname_len);

    /* Everything else: the kept states, under Java's names. Any other
       state the entry carries is dropped and the block takes its Java
       default — the reference's own answer for unknown states. */
    bool is_axis_family = false;
    for (size_t s = 0;
         s < sizeof kAxisSuffixes / sizeof kAxisSuffixes[0]; ++s) {
        if (ends_with(jname, jname_len, kAxisSuffixes[s])) {
            is_axis_family = true;
            break;
        }
    }
    if (strlen(kMushroom) == jname_len
            && memcmp(jname, kMushroom, jname_len) == 0) {
        is_axis_family = false;
    }

    McmeTag *out = named_entry(arena, jname, jname_len);
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
