/*
 * Bedrock subchunk payload decoder. The format and the strictness contract
 * are documented in mcme/bedrock_chunk.h; this file is the enforcement.
 *
 * Every read is bounds-checked against `size` before it happens, and every
 * refusal names what it found — an error message is the only diagnosis
 * someone gets from a world that will not open.
 */

#include "mcme/bedrock_chunk.h"

#include <stdio.h>
#include <string.h>

/* The widths that occur on disk. 7 and 9..15 never appear: the game's own
   enum jumps 6 -> 8 -> 16, and both disk readers reject the gaps. 127 is the
   header some biome records use for "copy the previous palette"; mcbe-leveldb
   flags its meaning for subchunks as unverified, so it is refused here rather
   than guessed at. */
static bool valid_bits(unsigned bits)
{
    switch (bits) {
    case 0: case 1: case 2: case 3: case 4: case 5: case 6: case 8: case 16:
        return true;
    default:
        return false;
    }
}

static bool need(const uint8_t *data, size_t size, size_t pos, size_t n,
                 const char *what)
{
    if (pos + n <= size && pos + n >= pos) return true;
    mcme_set_error("subchunk: truncated payload: %s needs %zu bytes at "
                   "offset %zu of %zu", what, n, pos, size);
    return false;
}

/* ---- BEGIN GENERATED NUMERICAL MAP ----
 * The bedrock 1.12 numerical map as the reference's own
 * ints_to_block reads it (D25): id -> name over all 256 ids, the
 * name half of a pre-palette cell's composition. The cell's data
 * nibble becomes the synthesized entry's `val` and the payload's
 * era its `version`; translate.c answers the pair through the
 * flat/legacy tables. Six ids (210, 211, 212, 217, 230, 242) are
 * not in the map — they answer as the minecraft:numerical
 * placeholder the reference passes through, and mcme's
 * unknown-block policy keeps that name and drops the states.
 * tools/ref_convert.py re-pins every id on every battery run. */
static const char *const kFlatIds[256] = {
    "minecraft:air",
    "minecraft:stone",
    "minecraft:grass",
    "minecraft:dirt",
    "minecraft:cobblestone",
    "minecraft:planks",
    "minecraft:sapling",
    "minecraft:bedrock",
    "minecraft:flowing_water",
    "minecraft:water",
    "minecraft:flowing_lava",
    "minecraft:lava",
    "minecraft:sand",
    "minecraft:gravel",
    "minecraft:gold_ore",
    "minecraft:iron_ore",
    "minecraft:coal_ore",
    "minecraft:log",
    "minecraft:leaves",
    "minecraft:sponge",
    "minecraft:glass",
    "minecraft:lapis_ore",
    "minecraft:lapis_block",
    "minecraft:dispenser",
    "minecraft:sandstone",
    "minecraft:noteblock",
    "minecraft:bed",
    "minecraft:golden_rail",
    "minecraft:detector_rail",
    "minecraft:sticky_piston",
    "minecraft:web",
    "minecraft:tallgrass",
    "minecraft:deadbush",
    "minecraft:piston",
    "minecraft:pistonArmCollision",
    "minecraft:wool",
    "minecraft:element_0",
    "minecraft:yellow_flower",
    "minecraft:red_flower",
    "minecraft:brown_mushroom",
    "minecraft:red_mushroom",
    "minecraft:gold_block",
    "minecraft:iron_block",
    "minecraft:double_stone_slab",
    "minecraft:stone_slab",
    "minecraft:brick_block",
    "minecraft:tnt",
    "minecraft:bookshelf",
    "minecraft:mossy_cobblestone",
    "minecraft:obsidian",
    "minecraft:torch",
    "minecraft:fire",
    "minecraft:mob_spawner",
    "minecraft:oak_stairs",
    "minecraft:chest",
    "minecraft:redstone_wire",
    "minecraft:diamond_ore",
    "minecraft:diamond_block",
    "minecraft:crafting_table",
    "minecraft:wheat",
    "minecraft:farmland",
    "minecraft:furnace",
    "minecraft:lit_furnace",
    "minecraft:standing_sign",
    "minecraft:wooden_door",
    "minecraft:ladder",
    "minecraft:rail",
    "minecraft:stone_stairs",
    "minecraft:wall_sign",
    "minecraft:lever",
    "minecraft:stone_pressure_plate",
    "minecraft:iron_door",
    "minecraft:wooden_pressure_plate",
    "minecraft:redstone_ore",
    "minecraft:lit_redstone_ore",
    "minecraft:unlit_redstone_torch",
    "minecraft:redstone_torch",
    "minecraft:stone_button",
    "minecraft:snow_layer",
    "minecraft:ice",
    "minecraft:snow",
    "minecraft:cactus",
    "minecraft:clay",
    "minecraft:reeds",
    "minecraft:jukebox",
    "minecraft:fence",
    "minecraft:pumpkin",
    "minecraft:netherrack",
    "minecraft:soul_sand",
    "minecraft:glowstone",
    "minecraft:portal",
    "minecraft:lit_pumpkin",
    "minecraft:cake",
    "minecraft:unpowered_repeater",
    "minecraft:powered_repeater",
    "minecraft:invisibleBedrock",
    "minecraft:trapdoor",
    "minecraft:monster_egg",
    "minecraft:stonebrick",
    "minecraft:brown_mushroom_block",
    "minecraft:red_mushroom_block",
    "minecraft:iron_bars",
    "minecraft:glass_pane",
    "minecraft:melon_block",
    "minecraft:pumpkin_stem",
    "minecraft:melon_stem",
    "minecraft:vine",
    "minecraft:fence_gate",
    "minecraft:brick_stairs",
    "minecraft:stone_brick_stairs",
    "minecraft:mycelium",
    "minecraft:waterlily",
    "minecraft:nether_brick",
    "minecraft:nether_brick_fence",
    "minecraft:nether_brick_stairs",
    "minecraft:nether_wart",
    "minecraft:enchanting_table",
    "minecraft:brewing_stand",
    "minecraft:cauldron",
    "minecraft:end_portal",
    "minecraft:end_portal_frame",
    "minecraft:end_stone",
    "minecraft:dragon_egg",
    "minecraft:redstone_lamp",
    "minecraft:lit_redstone_lamp",
    "minecraft:dropper",
    "minecraft:activator_rail",
    "minecraft:cocoa",
    "minecraft:sandstone_stairs",
    "minecraft:emerald_ore",
    "minecraft:ender_chest",
    "minecraft:tripwire_hook",
    "minecraft:tripWire",
    "minecraft:emerald_block",
    "minecraft:spruce_stairs",
    "minecraft:birch_stairs",
    "minecraft:jungle_stairs",
    "minecraft:command_block",
    "minecraft:beacon",
    "minecraft:cobblestone_wall",
    "minecraft:flower_pot",
    "minecraft:carrots",
    "minecraft:potatoes",
    "minecraft:wooden_button",
    "minecraft:skull",
    "minecraft:anvil",
    "minecraft:trapped_chest",
    "minecraft:light_weighted_pressure_plate",
    "minecraft:heavy_weighted_pressure_plate",
    "minecraft:unpowered_comparator",
    "minecraft:powered_comparator",
    "minecraft:daylight_detector",
    "minecraft:redstone_block",
    "minecraft:quartz_ore",
    "minecraft:hopper",
    "minecraft:quartz_block",
    "minecraft:quartz_stairs",
    "minecraft:double_wooden_slab",
    "minecraft:wooden_slab",
    "minecraft:stained_hardened_clay",
    "minecraft:stained_glass_pane",
    "minecraft:leaves2",
    "minecraft:log2",
    "minecraft:acacia_stairs",
    "minecraft:dark_oak_stairs",
    "minecraft:slime",
    "minecraft:glow_stick",
    "minecraft:iron_trapdoor",
    "minecraft:prismarine",
    "minecraft:seaLantern",
    "minecraft:hay_block",
    "minecraft:carpet",
    "minecraft:hardened_clay",
    "minecraft:coal_block",
    "minecraft:packed_ice",
    "minecraft:double_plant",
    "minecraft:standing_banner",
    "minecraft:wall_banner",
    "minecraft:daylight_detector_inverted",
    "minecraft:red_sandstone",
    "minecraft:red_sandstone_stairs",
    "minecraft:double_stone_slab2",
    "minecraft:stone_slab2",
    "minecraft:spruce_fence_gate",
    "minecraft:birch_fence_gate",
    "minecraft:jungle_fence_gate",
    "minecraft:dark_oak_fence_gate",
    "minecraft:acacia_fence_gate",
    "minecraft:repeating_command_block",
    "minecraft:chain_command_block",
    "minecraft:hard_glass_pane",
    "minecraft:hard_stained_glass_pane",
    "minecraft:chemical_heat",
    "minecraft:spruce_door",
    "minecraft:birch_door",
    "minecraft:jungle_door",
    "minecraft:acacia_door",
    "minecraft:dark_oak_door",
    "minecraft:grass_path",
    "minecraft:frame",
    "minecraft:chorus_flower",
    "minecraft:purpur_block",
    "minecraft:colored_torch_rg",
    "minecraft:purpur_stairs",
    "minecraft:colored_torch_bp",
    "minecraft:undyed_shulker_box",
    "minecraft:end_bricks",
    "minecraft:frosted_ice",
    "minecraft:end_rod",
    "minecraft:end_gateway",
    "minecraft:numerical",
    "minecraft:numerical",
    "minecraft:numerical",
    "minecraft:magma",
    "minecraft:nether_wart_block",
    "minecraft:red_nether_brick",
    "minecraft:bone_block",
    "minecraft:numerical",
    "minecraft:shulker_box",
    "minecraft:purple_glazed_terracotta",
    "minecraft:white_glazed_terracotta",
    "minecraft:orange_glazed_terracotta",
    "minecraft:magenta_glazed_terracotta",
    "minecraft:light_blue_glazed_terracotta",
    "minecraft:yellow_glazed_terracotta",
    "minecraft:lime_glazed_terracotta",
    "minecraft:pink_glazed_terracotta",
    "minecraft:gray_glazed_terracotta",
    "minecraft:silver_glazed_terracotta",
    "minecraft:cyan_glazed_terracotta",
    "minecraft:numerical",
    "minecraft:blue_glazed_terracotta",
    "minecraft:brown_glazed_terracotta",
    "minecraft:green_glazed_terracotta",
    "minecraft:red_glazed_terracotta",
    "minecraft:black_glazed_terracotta",
    "minecraft:concrete",
    "minecraft:concretePowder",
    "minecraft:chemistry_table",
    "minecraft:underwater_torch",
    "minecraft:chorus_plant",
    "minecraft:stained_glass",
    "minecraft:numerical",
    "minecraft:podzol",
    "minecraft:beetroot",
    "minecraft:stonecutter",
    "minecraft:glowingobsidian",
    "minecraft:netherreactor",
    "minecraft:info_update",
    "minecraft:info_update2",
    "minecraft:movingBlock",
    "minecraft:observer",
    "minecraft:structure_block",
    "minecraft:hard_glass",
    "minecraft:hard_stained_glass",
    "minecraft:reserved6",
};
/* ---- END GENERATED NUMERICAL MAP ---- */

/* ---- BEGIN GENERATED FLAT ERAS ----
 * Which era stamp each pre-palette payload version answers at
 * (D25): a 0x2c key's payload byte doubles as the chunk version,
 * amulet caps the anchor at 1.12, and through its chunk-version
 * table every payload 0-5 resolves to bedrock (1,1,0) and 6-7 to
 * bedrock (1,2,0) in any world that can hold the chunk. The
 * stamps are those eras' packed block versions
 * (major<<24 | minor<<16); index 1 is the palettized version 1,
 * which never reaches this table. Re-derived from amulet's own
 * tables on every battery run (tools/ref_convert.py). */
static const int32_t kFlatEra[8] = {
    16842752, 0, 16842752, 16842752, 16842752, 16842752, 16908288, 16908288
};
/* ---- END GENERATED FLAT ERAS ---- */

static uint8_t bits_for(size_t max_index);

/* One synthesized palette entry for a pre-palette cell: the 1.12
   numerical name, the data nibble as `val`, the payload's era as
   `version` — the three fields translate.c reads to answer the
   cell where the reference answers it (D25). */
static bool synthesize_flat(McmeNbtDoc *out, McmeArena *arena,
                            uint8_t id, unsigned data, int32_t era)
{
    const char *name = kFlatIds[id];
    McmeTag *doc = mcme_nbt_new(arena, MCME_TAG_Compound);
    McmeTag *nm = mcme_nbt_new(arena, MCME_TAG_String);
    McmeTag *val = mcme_nbt_new(arena, MCME_TAG_Int);
    McmeTag *ver = mcme_nbt_new(arena, MCME_TAG_Int);
    if (doc == NULL || nm == NULL || val == NULL || ver == NULL)
        return false;
    if (!mcme_nbt_compound_init(arena, doc, 3)
            || !mcme_nbt_set_string(arena, nm, name, strlen(name))
            || !mcme_nbt_set_int(val, (int32_t)data)
            || !mcme_nbt_set_int(ver, era)
            || !mcme_nbt_compound_put(arena, doc, "name", 4, nm)
            || !mcme_nbt_compound_put(arena, doc, "val", 3, val)
            || !mcme_nbt_compound_put(arena, doc, "version", 7, ver)) {
        return false;
    }
    memset(out, 0, sizeof *out);
    out->arena = arena;
    out->root = doc;
    out->little_endian = true;
    return true;
}

/* The two planes as one synthesized layer. Cells map to their
   pair's slot in first-seen order; `bits` is the width that holds
   those indices, the rule set() writes — the model's bookkeeping
   only, because the encoder refuses a version this function
   produces. The planes are bounds-checked by the caller. */
static bool decode_flat(McmeBedrockLayer *layer, McmeArena *arena,
                        const uint8_t *p, uint8_t version)
{
    static const size_t ncell = MCME_BEDROCK_SUBCHUNK_BLOCKS;
    const uint8_t *ids = p + 1;
    const uint8_t *nib = p + 1 + ncell;
    int32_t era = kFlatEra[version];

    uint16_t *indices = mcme_arena_alloc(arena, ncell * sizeof *indices);
    int32_t *slot_of = mcme_arena_alloc(arena, ncell * sizeof *slot_of);
    McmeNbtDoc *palette = mcme_arena_alloc(arena, ncell * sizeof *palette);
    if (indices == NULL || slot_of == NULL || palette == NULL)
        return false;
    for (size_t i = 0; i < ncell; ++i) slot_of[i] = -1;

    size_t palette_len = 0;
    for (size_t i = 0; i < ncell; ++i) {
        uint8_t id = ids[i];
        unsigned data = (i & 1) ? (nib[i / 2] >> 4) : (nib[i / 2] & 15u);
        int32_t pair = (int32_t)id * 16 + (int32_t)data;
        if (slot_of[pair] < 0) {
            if (!synthesize_flat(&palette[palette_len], arena, id, data,
                                 era)) {
                return false;
            }
            slot_of[pair] = (int32_t)palette_len;
            ++palette_len;
        }
        indices[i] = (uint16_t)slot_of[pair];
    }

    layer->bits = bits_for(palette_len - 1);
    layer->indices = indices;
    layer->palette_len = palette_len;
    layer->palette = palette;
    return true;
}

bool mcme_bedrock_subchunk_decode(McmeBedrockSubchunk *out, McmeArena *arena,
                                  const void *data, size_t size)
{
    if (!out || !arena || (!data && size > 0)) {
        mcme_set_error("subchunk: null argument");
        return false;
    }
    memset(out, 0, sizeof *out);

    const uint8_t *p = (const uint8_t *)data;
    size_t pos = 0;

    /* --- Wrapper ------------------------------------------------------- */
    if (!need(p, size, 0, 1, "the version byte")) return false;
    uint8_t version = p[0];
    size_t layer_count;
    bool has_index = false;
    bool flat = false;
    int8_t index = 0;

    switch (version) {
    case 1:
        /* The brief post-palettization format: one layer, no count byte.
           See the gist the wiki cites; amulet's decoder treats it the same. */
        layer_count = 1;
        pos = 1;
        break;
    case 8:
        if (!need(p, size, 1, 1, "the storage-layer count")) return false;
        layer_count = p[1];
        pos = 2;
        break;
    case 9:
        if (!need(p, size, 1, 2, "the storage-layer count and Y index"))
            return false;
        layer_count = p[1];
        index = (int8_t)p[2];
        has_index = true;
        pos = 3;
        break;
    case 0: case 2: case 3: case 4: case 5: case 6: case 7: {
        /* The pre-palette format (D25): one plane of 4096 block ids
           then 2048 bytes of data nibbles, low nibble first — the
           bytes amulet's flat branch reads, the same way. No
           storage count, no Y index: one implicit layer. */
        if (!need(p, size, 1, MCME_BEDROCK_SUBCHUNK_BLOCKS,
                  "the block id plane of the pre-palette format"))
            return false;
        if (!need(p, size, 1 + MCME_BEDROCK_SUBCHUNK_BLOCKS,
                  MCME_BEDROCK_SUBCHUNK_BLOCKS / 2,
                  "the data nibble plane of the pre-palette format"))
            return false;
        flat = true;
        layer_count = 1;
        pos = 1 + MCME_BEDROCK_SUBCHUNK_BLOCKS
                    + MCME_BEDROCK_SUBCHUNK_BLOCKS / 2;
        break;
    }
    default:
        mcme_set_error("subchunk: unknown version %u", version);
        return false;
    }

    McmeBedrockLayer *layers = NULL;
    if (layer_count > 0) {
        layers = mcme_arena_alloc(arena, layer_count * sizeof *layers);
        if (!layers) return false;
        memset(layers, 0, layer_count * sizeof *layers);
    }

    /* --- Layers -------------------------------------------------------- */
    if (flat && !decode_flat(&layers[0], arena, p, version)) return false;
    size_t paletted = flat ? 0 : layer_count;
    for (size_t i = 0; i < paletted; ++i) {
        if (!need(p, size, pos, 1, "a layer header")) return false;
        uint8_t header = p[pos];

        if (header & 1u) {
            mcme_set_error("subchunk: layer %zu has header 0x%02x: runtime "
                           "palettes are network data, not a stored world",
                           i, header);
            return false;
        }
        unsigned bits = header >> 1;
        if (!valid_bits(bits)) {
            mcme_set_error("subchunk: layer %zu stores %u bits per index; "
                           "valid widths are 0,1,2,3,4,5,6,8,16", i, bits);
            return false;
        }
        pos += 1;

        size_t palette_len;
        if (bits > 0) {
            size_t per_word = 32u / bits;
            size_t words = (MCME_BEDROCK_SUBCHUNK_BLOCKS + per_word - 1)
                           / per_word;
            if (!need(p, size, pos, words * 4, "the index words")) return false;

            uint16_t *indices =
                mcme_arena_alloc(arena, MCME_BEDROCK_SUBCHUNK_BLOCKS
                                        * sizeof *indices);
            if (!indices) return false;

            const uint8_t *w = p + pos;
            uint32_t mask = (1u << bits) - 1u;
            for (size_t u = 0; u < MCME_BEDROCK_SUBCHUNK_BLOCKS; ++u) {
                size_t word = u / per_word;
                uint32_t v = (uint32_t)w[word * 4]
                           | ((uint32_t)w[word * 4 + 1] << 8)
                           | ((uint32_t)w[word * 4 + 2] << 16)
                           | ((uint32_t)w[word * 4 + 3] << 24);
                indices[u] = (uint16_t)((v >> ((u % per_word) * bits)) & mask);
            }
            pos += words * 4;
            layers[i].indices = indices;

            if (!need(p, size, pos, 4, "the palette length")) return false;
            palette_len = (size_t)p[pos]
                        | ((size_t)p[pos + 1] << 8)
                        | ((size_t)p[pos + 2] << 16)
                        | ((size_t)p[pos + 3] << 24);
            pos += 4;

            if (palette_len == 0) {
                mcme_set_error("subchunk: layer %zu has %u-bit indices but an "
                               "empty palette", i, bits);
                return false;
            }
            /* Each entry is at least four bytes of NBT (type, empty name,
               TAG_End), so a length that cannot fit is refused here rather
               than driving an allocation. */
            if (palette_len > (size - pos) / 4) {
                mcme_set_error("subchunk: layer %zu palette claims %zu "
                               "entries but only %zu bytes remain",
                               i, palette_len, size - pos);
                return false;
            }
        } else {
            /* bits == 0: no words, no length field — the single NBT entry IS
               the palette, and every index is 0. Both disk readers agree;
               the golden fixture pins it. Materialise the zeros so callers
               see one shape. */
            uint16_t *indices =
                mcme_arena_alloc(arena, MCME_BEDROCK_SUBCHUNK_BLOCKS
                                        * sizeof *indices);
            if (!indices) return false;
            memset(indices, 0,
                   MCME_BEDROCK_SUBCHUNK_BLOCKS * sizeof *indices);
            layers[i].indices = indices;
            palette_len = 1;
        }

        McmeNbtDoc *palette = mcme_arena_alloc(arena, palette_len
                                                    * sizeof *palette);
        if (!palette) return false;

        for (size_t j = 0; j < palette_len; ++j) {
            size_t consumed = 0;
            if (!mcme_nbt_parse_prefix(&palette[j], arena, p + pos, size - pos,
                                       true /* little endian */, &consumed)) {
                /* Re-wrap with the position in subchunk terms. The NBT error
                   is relative to the slice it was handed. */
                char why[256];
                snprintf(why, sizeof why, "%s", mcme_last_error());
                mcme_set_error("subchunk: layer %zu palette entry %zu at "
                               "offset %zu: %s", i, j, pos, why);
                return false;
            }
            pos += consumed;
        }

        layers[i].bits = (uint8_t)bits;
        layers[i].palette_len = palette_len;
        layers[i].palette = palette;
    }

    /* --- Nothing may follow the last layer --------------------------------
       A decoder that tolerates a tail cannot round-trip: the tail would be
       silently dropped on the way back out. Refusing is how it gets noticed. */
    if (pos != size) {
        mcme_set_error("subchunk: %zu trailing bytes after %zu layer(s)",
                       size - pos, layer_count);
        return false;
    }

    out->version = version;
    out->has_index = has_index;
    out->index = index;
    out->layer_count = layer_count;
    out->layers = layers;
    return true;
}

/* ========================================================================
 * Encoding
 *
 * Two phases: every refusal is decided in the outer function before the
 * first byte is appended, so a refused encode leaves the caller's buffer
 * exactly as it was. The only failure that can interrupt writing is the
 * buffer's own allocation.
 * ======================================================================== */

static bool encode_layer(const McmeBedrockLayer *layer, McmeBuffer *out)
{
    if (layer->bits == 0) {
        /* One entry fills the layer: header, then the NBT itself — no
           length field, the layout the decoder pins. */
        if (!mcme_buffer_append_u8(out, 0)) return false;
        return mcme_nbt_write_binary_as(&layer->palette[0], out,
                                        true /* little endian */);
    }

    if (!mcme_buffer_append_u8(out, (uint8_t)(layer->bits << 1))) return false;

    /* Words: floor(32/bits) indices each, packed from the least-significant
       bit upward, zeroed first so the padding above the last index comes
       out zero — the one documented non-inversion (see the header). The
       word count is exactly what the decoder computes: ceil(4096/vpw). */
    size_t per_word = 32u / layer->bits;
    uint32_t word = 0;
    for (size_t u = 0; u < MCME_BEDROCK_SUBCHUNK_BLOCKS; ++u) {
        word |= (uint32_t)layer->indices[u]
                << ((u % per_word) * layer->bits);
        if (u % per_word == per_word - 1
                || u == MCME_BEDROCK_SUBCHUNK_BLOCKS - 1) {
            if (!mcme_buffer_append_u8(out, (uint8_t)word) ||
                !mcme_buffer_append_u8(out, (uint8_t)(word >> 8)) ||
                !mcme_buffer_append_u8(out, (uint8_t)(word >> 16)) ||
                !mcme_buffer_append_u8(out, (uint8_t)(word >> 24)))
                return false;
            word = 0;
        }
    }

    uint32_t len = (uint32_t)layer->palette_len;
    if (!mcme_buffer_append_u8(out, (uint8_t)len) ||
        !mcme_buffer_append_u8(out, (uint8_t)(len >> 8)) ||
        !mcme_buffer_append_u8(out, (uint8_t)(len >> 16)) ||
        !mcme_buffer_append_u8(out, (uint8_t)(len >> 24)))
        return false;

    for (size_t j = 0; j < layer->palette_len; ++j) {
        if (!mcme_nbt_write_binary_as(&layer->palette[j], out,
                                      true /* little endian */))
            return false;
    }
    return true;
}

/* Phase one: everything that could make the bytes unwritable, decided
   before out is touched. Whether an index points inside its palette is
   deliberately NOT checked here — the decoder does not check it either,
   and encode exists to invert decode. */
static bool check_layer(const McmeBedrockLayer *l, size_t i)
{
    if (!valid_bits(l->bits)) {
        mcme_set_error("subchunk: encode: layer %zu stores %u bits per "
                       "index; valid widths are 0,1,2,3,4,5,6,8,16",
                       i, l->bits);
        return false;
    }
    if (l->indices == NULL) {
        mcme_set_error("subchunk: encode: layer %zu has no indices", i);
        return false;
    }
    if (l->palette == NULL) {
        mcme_set_error("subchunk: encode: layer %zu has no palette", i);
        return false;
    }
    if (l->palette_len == 0) {
        mcme_set_error("subchunk: encode: layer %zu has an empty palette", i);
        return false;
    }
    if (l->palette_len > 0xffffffffu) {
        mcme_set_error("subchunk: encode: layer %zu palette of %zu cannot "
                       "be written", i, l->palette_len);
        return false;
    }
    for (size_t j = 0; j < l->palette_len; ++j) {
        if (l->palette[j].root == NULL) {
            mcme_set_error("subchunk: encode: layer %zu palette entry %zu "
                           "has no root tag", i, j);
            return false;
        }
    }

    if (l->bits == 0) {
        if (l->palette_len != 1) {
            mcme_set_error("subchunk: encode: layer %zu: 0-bit indices "
                           "require exactly one palette entry, have %zu",
                           i, l->palette_len);
            return false;
        }
        for (size_t u = 0; u < MCME_BEDROCK_SUBCHUNK_BLOCKS; ++u) {
            if (l->indices[u] != 0) {
                mcme_set_error("subchunk: encode: layer %zu: index %zu is %u "
                               "but a 0-bit layer can only store 0",
                               i, u, l->indices[u]);
                return false;
            }
        }
    } else {
        uint32_t mask = (1u << l->bits) - 1u;
        for (size_t u = 0; u < MCME_BEDROCK_SUBCHUNK_BLOCKS; ++u) {
            if ((uint32_t)l->indices[u] > mask) {
                mcme_set_error("subchunk: encode: layer %zu: index %zu is "
                               "%u, which %u bits cannot hold",
                               i, u, l->indices[u], l->bits);
                return false;
            }
        }
    }
    return true;
}

bool mcme_bedrock_subchunk_encode(const McmeBedrockSubchunk *sc,
                                  McmeBuffer *out)
{
    if (!sc || !out) {
        mcme_set_error("subchunk: encode: null argument");
        return false;
    }

    switch (sc->version) {
    case 1:
        if (sc->layer_count != 1) {
            mcme_set_error("subchunk: encode: version 1 stores exactly one "
                           "layer, have %zu", sc->layer_count);
            return false;
        }
        if (sc->has_index) {
            mcme_set_error("subchunk: encode: version 1 has no Y index byte "
                           "to write");
            return false;
        }
        break;
    case 8:
        if (sc->has_index) {
            mcme_set_error("subchunk: encode: version 8 has no Y index byte "
                           "to write");
            return false;
        }
        if (sc->layer_count > 255) {
            mcme_set_error("subchunk: encode: version 8 stores at most 255 "
                           "layers, have %zu", sc->layer_count);
            return false;
        }
        break;
    case 9:
        if (!sc->has_index) {
            mcme_set_error("subchunk: encode: version 9 requires a Y index");
            return false;
        }
        if (sc->layer_count > 255) {
            mcme_set_error("subchunk: encode: version 9 stores at most 255 "
                           "layers, have %zu", sc->layer_count);
            return false;
        }
        break;
    default:
        mcme_set_error("subchunk: encode: version %u is not 1, 8 or 9",
                       sc->version);
        return false;
    }

    if (sc->layer_count > 0 && sc->layers == NULL) {
        mcme_set_error("subchunk: encode: %zu layers but no layer array",
                       sc->layer_count);
        return false;
    }
    for (size_t i = 0; i < sc->layer_count; ++i) {
        if (!check_layer(&sc->layers[i], i)) return false;
    }

    /* Phase two: write. */
    if (!mcme_buffer_append_u8(out, sc->version)) return false;
    if (sc->version != 1) {
        if (!mcme_buffer_append_u8(out, (uint8_t)sc->layer_count)) return false;
    }
    if (sc->version == 9) {
        if (!mcme_buffer_append_u8(out, (uint8_t)sc->index)) return false;
    }
    for (size_t i = 0; i < sc->layer_count; ++i) {
        if (!encode_layer(&sc->layers[i], out)) return false;
    }
    return true;
}

/* ========================================================================
 * Setting one block
 * ======================================================================== */

/* The width the reference's encoder picks for a palette whose
   largest index in use is max_index: the bit length of that
   index, with the gaps at 7 and 9..15 snapped to the next
   width the format carries. */
static uint8_t bits_for(size_t max_index)
{
    unsigned bits = 0;
    for (size_t v = max_index; v > 0; v >>= 1) ++bits;
    if (bits == 7) bits = 8;
    else if (bits >= 9 && bits <= 15) bits = 16;
    if (bits == 0) bits = 1;
    return (uint8_t)bits;
}

bool mcme_bedrock_subchunk_set(McmeBedrockSubchunk *sc,
                              McmeArena *arena,
                              int32_t x, int32_t y, int32_t z,
                              const McmeNbtDoc *block)
{
    if (sc == NULL || arena == NULL || block == NULL) {
        mcme_set_error("subchunk: set: null argument");
        return false;
    }
    if (block->root == NULL
            || block->root->type != MCME_TAG_Compound) {
        mcme_set_error("subchunk: set: block is not a compound "
                       "document");
        return false;
    }
    if (block->root_name.len != 0) {
        mcme_set_error("subchunk: set: a palette entry carries "
                       "no root name");
        return false;
    }
    if (x < 0 || x > 15 || y < 0 || y > 15 || z < 0 || z > 15) {
        mcme_set_error("subchunk: set: block (%d, %d, %d) is "
                       "outside the subchunk", x, y, z);
        return false;
    }
    if (sc->layer_count == 0) {
        mcme_set_error("subchunk: set: the all-air marker has "
                       "no layer to set");
        return false;
    }

    McmeBedrockLayer *layer = &sc->layers[0];
    size_t u = (size_t)x * 256 + (size_t)z * 16 + (size_t)y;

    /* Serialised, the entry is the comparison: two documents are
       the same block exactly when the reference's bytes for
       them are the same. */
    McmeBuffer want;
    mcme_buffer_init(&want);
    if (!mcme_nbt_write_binary_as(block, &want, true)) return false;

    size_t index = layer->palette_len;         /* "not present" */
    for (size_t j = 0; j < layer->palette_len; ++j) {
        McmeBuffer got;
        mcme_buffer_init(&got);
        bool ok = mcme_nbt_write_binary_as(&layer->palette[j],
                                            &got, true);
        bool same = ok && got.len == want.len
                    && memcmp(got.data, want.data, got.len) == 0;
        mcme_buffer_free(&got);
        if (!ok) {
            mcme_buffer_free(&want);
            return false;
        }
        if (same) {
            index = j;
            break;
        }
    }

    if (index == layer->palette_len) {
        if (index > UINT16_MAX) {
            mcme_buffer_free(&want);
            mcme_set_error("subchunk: set: the palette already "
                           "holds %zu entries; no index left to "
                           "add one", index);
            return false;
        }
        /* The palette array is arena memory and cannot grow, so a
           new one is allocated and the documents — arena-owned,
           never moved — are copied over. */
        McmeNbtDoc *palette = mcme_arena_alloc(
            arena, (layer->palette_len + 1) * sizeof *palette);
        mcme_buffer_free(&want);
        if (palette == NULL) return false;
        memcpy(palette, layer->palette,
               layer->palette_len * sizeof *palette);
        palette[layer->palette_len] = *block;
        layer->palette = palette;
        layer->palette_len += 1;
    } else {
        mcme_buffer_free(&want);
    }

    layer->indices[u] = (uint16_t)index;

    /* The stored width only ever grows, and only when the
       edit forces it: a no-op leaves every byte of the
       payload alone, and a layer stored wider than its
       palette needs stays that way — the reference writes
       such layers too, and narrowing here would rewrite
       indices this edit did not touch. The exception is
       the compact single-entry form, which an edit that
       adds a second entry must leave for a wider one. */
    size_t max_index = 0;
    for (size_t v = 0; v < MCME_BEDROCK_SUBCHUNK_BLOCKS; ++v) {
        if (layer->indices[v] > max_index) max_index = layer->indices[v];
    }
    if (max_index == 0 && layer->palette_len == 1) {
        layer->bits = 0;
    } else {
        uint8_t need = bits_for(max_index);
        if (need > layer->bits) layer->bits = need;
    }
    return true;
}

/* --- Data3D biome record (tag 0x2b) -------------------------------------
 *
 * Format contract in mcme/bedrock_chunk.h. The packed half is the same
 * word layout the subchunk layers use — little-endian 32-bit words,
 * floor(32/bits) values from the low bit up — so only the sampling is
 * new: cell (x, y, z) of the 4x4x4 reads block (4x, 4y, 4z) of the
 * 16x16x16 storage, in the storage's XZY order, and lands in Java's
 * container order x + z*4 + y*16.
 */

static uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8)
         | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

bool mcme_bedrock_biome_decode(McmeBedrockBiomes *out, McmeArena *arena,
                               const void *data, size_t size, int8_t floor_cy)
{
    if (!out || !arena || (!data && size > 0)) {
        mcme_set_error("biome record: null argument");
        return false;
    }
    memset(out, 0, sizeof *out);

    const uint8_t *p = (const uint8_t *)data;

    if (size < 512) {
        mcme_set_error("biome record: %zu bytes, shorter than the 512-byte "
                       "heightmap every one starts with", size);
        return false;
    }
    for (size_t i = 0; i < 256; ++i) {
        uint32_t u = (uint32_t)p[i * 2] | ((uint32_t)p[i * 2 + 1] << 8);
        out->heightmap[i] = (int16_t)(u & 0x8000u ? (int32_t)u - 65536
                                                   : (int32_t)u);
    }

    out->arrays = mcme_arena_alloc(arena, MCME_BEDROCK_BIOME_MAX_ARRAYS
                                           * sizeof *out->arrays);
    if (!out->arrays) return false;
    memset(out->arrays, 0, MCME_BEDROCK_BIOME_MAX_ARRAYS
                               * sizeof *out->arrays);

    size_t pos = 512;
    while (pos < size) {
        uint8_t header = p[pos];

        if (header & 0x80u) {
            /* Signed, this byte is negative: the game's end padding.
               The reference skips it a byte at a time and so loses
               nothing; do the same. */
            ++pos;
            continue;
        }
        unsigned bits = header >> 1;
        ++pos;

        if (bits > 16) {
            mcme_set_error("biome record: array %zu stores %u bits per cell; "
                           "valid widths are 0 (one id) and 1..16",
                           out->count, bits);
            return false;
        }
        if (out->count >= MCME_BEDROCK_BIOME_MAX_ARRAYS) {
            mcme_set_error("biome record: more arrays than one world height "
                           "holds above floor %d", (int)floor_cy);
            return false;
        }

        struct McmeBedrockBiomeArray *arr = &out->arrays[out->count];

        if (bits == 0) {
            if (pos + 4 > size) {
                mcme_set_error("biome record: truncated: the single cell id "
                               "of array %zu needs 4 bytes at offset %zu of "
                               "%zu", out->count, pos, size);
                return false;
            }
            uint32_t v = le32(p + pos);
            pos += 4;
            for (size_t j = 0; j < MCME_BEDROCK_BIOME_CELLS; ++j) {
                arr->cells[j] = v;
            }
        } else {
            size_t per_word = 32u / bits;
            size_t words = (MCME_BEDROCK_SUBCHUNK_BLOCKS + per_word - 1)
                           / per_word;
            if (pos + words * 4 > size) {
                mcme_set_error("biome record: truncated: the %u-bit cells of "
                               "array %zu need %zu bytes at offset %zu of %zu",
                               bits, out->count, words * 4, pos, size);
                return false;
            }
            const uint8_t *w = p + pos;
            pos += words * 4;

            if (pos + 4 > size) {
                mcme_set_error("biome record: truncated: the palette length "
                               "of array %zu needs 4 bytes at offset %zu of "
                               "%zu", out->count, pos, size);
                return false;
            }
            uint32_t plen = le32(p + pos);
            pos += 4;
            if (plen == 0) {
                mcme_set_error("biome record: array %zu stores %u-bit cells "
                               "with an empty palette", out->count, bits);
                return false;
            }
            if (plen > (size - pos) / 4) {
                mcme_set_error("biome record: truncated: the palette of array "
                               "%zu claims %u ids past offset %zu of %zu",
                               out->count, plen, pos, size);
                return false;
            }

            uint32_t mask = (1u << bits) - 1u;
            for (size_t cyi = 0; cyi < 4; ++cyi) {
                for (size_t zi = 0; zi < 4; ++zi) {
                    for (size_t xi = 0; xi < 4; ++xi) {
                        size_t u = ((size_t)(xi * 4) << 8)
                                 | ((size_t)(zi * 4) << 4)
                                 | (size_t)(cyi * 4);
                        size_t word = u / per_word;
                        uint32_t packed = (le32(w + word * 4)
                                           >> ((u % per_word) * bits)) & mask;
                        if (packed >= plen) {
                            mcme_set_error("biome record: array %zu holds cell "
                                           "index %u past its %u-id palette",
                                           out->count, packed, plen);
                            return false;
                        }
                        uint32_t id = le32(p + pos + (size_t)packed * 4);
                        arr->cells[cyi * 16 + zi * 4 + xi] = id;
                    }
                }
            }
            pos += (size_t)plen * 4;
        }

        arr->cy = (int8_t)(floor_cy + (int32_t)out->count);
        ++out->count;
    }
    return true;
}

/*
 * The older spelling (D26): 768 bytes — the same 512-byte heightmap
 * half, then 256 raw biome ids, one per column, no palette. The
 * reference reads the bytes as frombuffer(...).reshape(16, 16).T
 * and broadcasts them with convert_to_3d: every section from Y0
 * through Y255 (cy 0..15 — the unbounded array's default range,
 * measured from Biomes3D's own defaults, not from any floor) takes
 * the cell value A[4x, 4z] = bytes[64z + 4x], the same value down
 * the section's four cells. This decoder materializes that
 * broadcast: 16 arrays at absolute cy 0..15, whatever the
 * dimension's floor, cells in Java's container order like the 3D
 * record's. The heightmap half decodes exactly as the 3D record's
 * first 512 bytes.
 *
 * Refused with the finding in mcme_last_error(): any size other
 * than exactly 768 — the reference slices exactly a 512-byte map
 * and a 256-byte table and its reshape answers for nothing else.
 * One refusal is the whole record's — the caller counts it once.
 *
 * Ownership: everything returned lives in the arena; the input
 * bytes are borrowed for the call only.
 */
bool mcme_bedrock_biome_2d_decode(McmeBedrockBiomes *out,
                                  McmeArena *arena,
                                  const void *data, size_t size)
{
    if (out == NULL || arena == NULL || data == NULL) return false;
    memset(out, 0, sizeof *out);

    if (size != MCME_BEDROCK_BIOME_2D_SIZE) {
        mcme_set_error("biome record (2D): %zu bytes; the older "
                       "spelling is a 512-byte heightmap and 256 "
                       "biome ids, nothing else", size);
        return false;
    }
    const uint8_t *p = (const uint8_t *)data;
    for (size_t i = 0; i < 256; ++i) {
        uint32_t u = (uint32_t)p[i * 2] | ((uint32_t)p[i * 2 + 1] << 8);
        out->heightmap[i] = (int16_t)(u & 0x8000u ? (int32_t)u - 65536
                                                   : (int32_t)u);
    }

    out->arrays = mcme_arena_alloc(arena, MCME_BEDROCK_BIOME_MAX_ARRAYS
                                           * sizeof *out->arrays);
    if (!out->arrays) return false;
    memset(out->arrays, 0, MCME_BEDROCK_BIOME_MAX_ARRAYS
                               * sizeof *out->arrays);

    for (size_t cyi = 0; cyi < 16; ++cyi) {
        struct McmeBedrockBiomeArray *arr = &out->arrays[cyi];
        arr->cy = (int8_t)cyi;           /* absolute: 0..15 */
        for (size_t zi = 0; zi < 4; ++zi) {
            for (size_t xi = 0; xi < 4; ++xi) {
                uint32_t id = p[512 + 64 * zi + 4 * xi];
                for (size_t yi = 0; yi < 4; ++yi) {
                    arr->cells[yi * 16 + zi * 4 + xi] = id;
                }
            }
        }
    }
    out->count = 16;
    return true;
}
