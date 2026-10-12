/*
 * Java chunk palettes: read one block out of a section's palette,
 * point one position at a different entry. The format facts live in
 * java_chunk.h; this file is the walking of them.
 */

#include <stdint.h>
#include <string.h>

#include "mcme/java_chunk.h"

enum {
    kBlocksPerSection = 4096,  /* 16 x 16 x 16                */
    kBitsFloor        = 4,     /* the format's minimum bits   */
    kBitsMax          = 15     /* what a packed entry allows  */
};

static const char kAir[] = "minecraft:air";

static void set_refusal(McmeWriteRefusal *r, McmeWriteRefusal v)
{
    if (r != NULL) *r = v;
}

/* Bits per packed entry for a palette of n entries: the need, with
   the format's floor. Capped at kBitsMax; the caller refuses a
   palette that would need more. */
static int palette_bits(size_t n)
{
    int bits = kBitsFloor;
    while (bits < kBitsMax && ((size_t)1 << bits) < n) ++bits;
    return bits;
}

/* The position's slot in its section's packing: X first, then Z,
   then Y — the order the long array is written in. */
static size_t block_pos(int x, int y, int z)
{
    return (size_t)(x & 15) + (size_t)(z & 15) * 16
           + (size_t)(y & 15) * 256;
}

/*
 * The section stored for a section Y. 1 with `*out` set, 0 when the
 * chunk lists none for it — the column is air there, which is an
 * answer, not a fault — and -1 where the stored tree is not the
 * format mcme reads, mcme_last_error() saying which part. Every
 * section it walks is validated, so a 0 means the whole list is
 * sound.
 */
static int find_section(const McmeTag *root, int y_sec,
                        const McmeTag **out)
{
    const McmeTag *secs = mcme_nbt_get(root, "sections");
    if (secs == NULL) return 0;
    if (secs->type != MCME_TAG_List) {
        mcme_set_error("java chunk: sections is a %s, not a list",
                       mcme_tag_type_name(secs->type));
        return -1;
    }
    for (size_t i = 0; i < secs->list.len; ++i) {
        const McmeTag *s = mcme_nbt_at(secs, i);
        const McmeTag *y = (s != NULL) ? mcme_nbt_get(s, "Y") : NULL;
        if (s == NULL || s->type != MCME_TAG_Compound
                || y == NULL || y->type != MCME_TAG_Byte) {
            mcme_set_error("java chunk: section %zu is not a "
                           "compound with a byte Y", i);
            return -1;
        }
        if ((int)y->as_byte == y_sec) {
            *out = s;
            return 1;
        }
    }
    return 0;
}

/*
 * A section's block_states as one view. A NULL palette is the
 * format's air section — no block_states, one implicit entry, every
 * index zero — and NULL data is the reference's own shorthand for
 * the single-entry palette it writes without an array.
 */
typedef struct {
    const McmeTag *palette;   /* list of compounds, or NULL  */
    const McmeTag *data;      /* long array, or NULL         */
    int bits;                 /* bits per packed entry       */
} SectionPalette;

static bool section_palette(const McmeTag *section, SectionPalette *v)
{
    v->palette = NULL;
    v->data = NULL;
    v->bits = kBitsFloor;

    const McmeTag *bs = mcme_nbt_get(section, "block_states");
    if (bs == NULL) return true;         /* the format's air section */
    if (bs->type != MCME_TAG_Compound) {
        mcme_set_error("java chunk: block_states is a %s, not a "
                       "compound", mcme_tag_type_name(bs->type));
        return false;
    }
    const McmeTag *pal = mcme_nbt_get(bs, "palette");
    if (pal == NULL || pal->type != MCME_TAG_List
            || pal->list.elem != MCME_TAG_Compound
            || pal->list.len == 0) {
        mcme_set_error("java chunk: block_states has no palette of "
                       "compounds");
        return false;
    }
    const McmeTag *data = mcme_nbt_get(bs, "data");
    if (data != NULL && data->type != MCME_TAG_Long_Array) {
        mcme_set_error("java chunk: block_states data is a %s, not "
                       "a long array", mcme_tag_type_name(data->type));
        return false;
    }
    if (pal->list.len > 1 && data == NULL) {
        mcme_set_error("java chunk: a palette of %zu entries has no "
                       "data array", pal->list.len);
        return false;
    }
    v->palette = pal;
    v->data = data;
    v->bits = palette_bits(pal->list.len);
    return true;
}

/* The palette entry a packed index points at, read the way the
   format packs: floor(64 / bits) entries to a long, none spanning
   two. The implicit and single-entry palettes hold entry 0. */
static bool palette_index(const SectionPalette *v, size_t pos,
                          size_t *out)
{
    if (v->palette == NULL || v->palette->list.len == 1) {
        *out = 0;
        return true;
    }
    size_t per_long = 64u / (size_t)v->bits;
    size_t word = pos / per_long;
    size_t bit = (pos % per_long) * (size_t)v->bits;
    if (word >= v->data->longs.len) {
        mcme_set_error("java chunk: the data array of %zu longs is "
                       "short for the palette and bits it carries",
                       v->data->longs.len);
        return false;
    }
    uint64_t packed = (uint64_t)v->data->longs.data[word];
    uint64_t mask = ((uint64_t)1 << v->bits) - 1u;
    *out = (size_t)((packed >> bit) & mask);
    return true;
}

/* One palette entry as the spec `chunk set` accepts: the Name, with
   the Properties in braces when there are any. */
static bool entry_spec(const McmeTag *entry, McmeBuffer *out)
{
    if (entry == NULL || entry->type != MCME_TAG_Compound) {
        mcme_set_error("java chunk: a palette entry is not a "
                       "compound");
        return false;
    }
    const McmeTag *name = mcme_nbt_get(entry, "Name");
    if (name == NULL || name->type != MCME_TAG_String) {
        mcme_set_error("java chunk: a palette entry has no Name "
                       "string");
        return false;
    }
    if (!mcme_buffer_append(out, name->string.data, name->string.len)) {
        return false;
    }
    const McmeTag *props = mcme_nbt_get(entry, "Properties");
    if (props != NULL && props->type == MCME_TAG_Compound
            && props->compound.len > 0) {
        return mcme_nbt_write_snbt(props, out, 0);
    }
    return true;
}

/* Whether a palette entry is the block the caller named: the same
   Name and the same Properties — compared as the SNBT both would
   print, which sorts keys, so entry order cannot lie about
   equality. */
static bool entry_is(const McmeTag *entry, const char *name,
                     const McmeTag *states)
{
    if (entry == NULL || entry->type != MCME_TAG_Compound) {
        return false;
    }
    const McmeTag *nm = mcme_nbt_get(entry, "Name");
    if (nm == NULL || nm->type != MCME_TAG_String) return false;
    size_t name_len = strlen(name);
    if (nm->string.len != name_len
            || memcmp(nm->string.data, name, name_len) != 0) {
        return false;
    }
    const McmeTag *props = mcme_nbt_get(entry, "Properties");
    bool have = props != NULL && props->type == MCME_TAG_Compound
                && props->compound.len > 0;
    bool want = states != NULL && states->type == MCME_TAG_Compound
                && states->compound.len > 0;
    if (have != want) return false;
    if (!have) return true;

    bool equal = false;
    McmeBuffer a, b;
    mcme_buffer_init(&a);
    mcme_buffer_init(&b);
    if (mcme_nbt_write_snbt(props, &a, 0)
            && mcme_nbt_write_snbt(states, &b, 0)) {
        equal = a.len == b.len && memcmp(a.data, b.data, a.len) == 0;
    }
    mcme_buffer_free(&a);
    mcme_buffer_free(&b);
    return equal;
}

/* The spec `name` + `states` spells air: what an untouched section
   holds, checked before any edit so a no-op edits nothing. */
static bool spec_is_air(const char *name, const McmeTag *states)
{
    size_t n = sizeof kAir - 1;
    return strlen(name) == n && memcmp(name, kAir, n) == 0
           && (states == NULL
               || (states->type == MCME_TAG_Compound
                   && states->compound.len == 0));
}

bool mcme_java_block_spec(const McmeTag *root, int x, int y, int z,
                          McmeBuffer *out)
{
    if (root == NULL || out == NULL) {
        mcme_set_error("java chunk: null argument");
        return false;
    }
    if (root->type != MCME_TAG_Compound) {
        mcme_set_error("java chunk: the chunk is a %s, not a "
                       "compound", mcme_tag_type_name(root->type));
        return false;
    }

    const McmeTag *section = NULL;
    int found = find_section(root, y >> 4, &section);
    if (found < 0) return false;
    if (found == 0) return mcme_buffer_append_cstr(out, kAir);

    SectionPalette v;
    if (!section_palette(section, &v)) return false;
    if (v.palette == NULL) {
        return mcme_buffer_append_cstr(out, kAir);
    }

    size_t idx;
    if (!palette_index(&v, block_pos(x, y, z), &idx)) return false;
    if (idx >= v.palette->list.len) {
        mcme_set_error("java chunk: the index at block (%d, %d, %d) "
                       "is %zu, outside its palette of %zu entries",
                       x, y, z, idx, v.palette->list.len);
        return false;
    }
    return entry_spec(mcme_nbt_at(v.palette, idx), out);
}

/*
 * The section for an edit: the one the chunk lists, or one made and
 * spliced into the list at its Y — the order the reference writes —
 * for the caller to keep editing. The tree is the caller's: the
 * getters are const, the edit is not.
 */
static McmeTag *section_for_edit(McmeArena *arena, McmeTag *root,
                                 int y_sec, McmeWriteRefusal *refusal)
{
    const McmeTag *found_c = NULL;
    int found = find_section(root, y_sec, &found_c);
    if (found < 0) {
        set_refusal(refusal, MCME_WRITE_REFUSAL_NOT_READABLE);
        return NULL;
    }
    if (found == 1) return (McmeTag *)found_c;

    const McmeTag *secs = mcme_nbt_get(root, "sections");
    size_t n = (secs != NULL) ? secs->list.len : 0;
    size_t at = n;                        /* where the new one goes */
    if (secs != NULL) {
        at = 0;
        for (size_t i = 0; i < n; ++i) {
            const McmeTag *s = mcme_nbt_at(secs, i);
            if ((int)mcme_nbt_get(s, "Y")->as_byte > y_sec) {
                at = i;
                break;
            }
            at = i + 1;
        }
    }

    McmeTag *grown = mcme_nbt_new(arena, MCME_TAG_List);
    McmeTag *made = mcme_nbt_new(arena, MCME_TAG_Compound);
    McmeTag *ytag = mcme_nbt_new(arena, MCME_TAG_Byte);
    if (grown == NULL || made == NULL || ytag == NULL
            || !mcme_nbt_list_alloc(arena, grown, MCME_TAG_Compound,
                                    n + 1)
            || !mcme_nbt_compound_init(arena, made, 2)
            || !mcme_nbt_set_byte(ytag, (int8_t)y_sec)
            || !mcme_nbt_compound_put(arena, made, "Y", 1, ytag)) {
        set_refusal(refusal, MCME_WRITE_REFUSAL_INTERNAL);
        return NULL;
    }
    for (size_t i = 0; i < n; ++i) {
        grown->list.items[i] = secs->list.items[i];
    }
    for (size_t i = n; i > at; --i) {
        grown->list.items[i] = grown->list.items[i - 1];
    }
    grown->list.items[at] = *made;
    if (!mcme_nbt_compound_put(arena, root, "sections", 8, grown)) {
        set_refusal(refusal, MCME_WRITE_REFUSAL_INTERNAL);
        return NULL;
    }
    return &grown->list.items[at];
}

bool mcme_java_set_block(McmeArena *arena, McmeTag *root,
                         int x, int y, int z,
                         const char *name, const McmeTag *states,
                         bool *changed,
                         McmeWriteRefusal *refusal)
{
    set_refusal(refusal, MCME_WRITE_REFUSAL_NONE);
    if (changed != NULL) *changed = false;
    if (arena == NULL || root == NULL || name == NULL) {
        mcme_set_error("java chunk: null argument");
        set_refusal(refusal, MCME_WRITE_REFUSAL_USAGE);
        return false;
    }
    if (root->type != MCME_TAG_Compound) {
        mcme_set_error("java chunk: the chunk is a %s, not a "
                       "compound", mcme_tag_type_name(root->type));
        set_refusal(refusal, MCME_WRITE_REFUSAL_NOT_READABLE);
        return false;
    }
    if (states != NULL && states->type != MCME_TAG_Compound) {
        mcme_set_error("java chunk: states is a %s, not a compound",
                       mcme_tag_type_name(states->type));
        set_refusal(refusal, MCME_WRITE_REFUSAL_USAGE);
        return false;
    }
    int y_sec = y >> 4;
    if (y_sec < -128 || y_sec > 127) {
        mcme_set_error("java chunk: the Y of block (%d, %d, %d) is "
                       "past the sections a chunk can list", x, y, z);
        set_refusal(refusal, MCME_WRITE_REFUSAL_INTERNAL);
        return false;
    }

    /* Air on a position no section covers edits nothing: the column
       already says it, and no section is invented to hold it. */
    if (spec_is_air(name, states)) {
        const McmeTag *sec = NULL;
        int found = find_section(root, y_sec, &sec);
        if (found < 0) {
            set_refusal(refusal, MCME_WRITE_REFUSAL_NOT_READABLE);
            return false;
        }
        if (found == 0) return true;
    }

    McmeTag *section = section_for_edit(arena, root, y_sec, refusal);
    if (section == NULL) return false;

    SectionPalette v;
    if (!section_palette(section, &v)) {
        set_refusal(refusal, MCME_WRITE_REFUSAL_NOT_READABLE);
        return false;
    }
    size_t old_len = (v.palette != NULL) ? v.palette->list.len : 1u;
    size_t pos = block_pos(x, y, z);
    size_t at_idx;
    if (!palette_index(&v, pos, &at_idx)) {
        set_refusal(refusal, MCME_WRITE_REFUSAL_NOT_READABLE);
        return false;
    }
    if (v.palette != NULL && at_idx >= v.palette->list.len) {
        mcme_set_error("java chunk: the index at block (%d, %d, %d) "
                       "is %zu, outside its palette of %zu entries",
                       x, y, z, at_idx, v.palette->list.len);
        set_refusal(refusal, MCME_WRITE_REFUSAL_NOT_READABLE);
        return false;
    }

    /* The block already stored here: nothing to say differently,
       and the caller does not write. Air is the implicit entry an
       untouched section holds. */
    if ((v.palette == NULL && spec_is_air(name, states))
            || (v.palette != NULL
                && entry_is(mcme_nbt_at(v.palette, at_idx),
                            name, states))) {
        return true;
    }

    /* The entry to point at: one the palette already holds when the
       block is one, a new one at the end when it is not — never
       replacing an entry in place, which would change what every
       other position holding the old entry means. The old entry
       keeps its index for the positions that still mean it. */
    size_t target = old_len;
    for (size_t i = 0; v.palette != NULL && i < old_len; ++i) {
        if (entry_is(mcme_nbt_at(v.palette, i), name, states)) {
            target = i;
            break;
        }
    }
    if (target == old_len
            && old_len + 1u > ((size_t)1 << kBitsMax)) {
        mcme_set_error("java chunk: a palette past %zu entries will "
                       "not pack", (size_t)1 << kBitsMax);
        set_refusal(refusal, MCME_WRITE_REFUSAL_INTERNAL);
        return false;
    }

    /* Repack the section: every position's entry, the edited one at
       the target, at the bits the palette now needs. Old indices
       keep their numbers — appending does not renumber — so the
       old decode and the new packing share one scheme. */
    size_t new_len = (target == old_len) ? old_len + 1u : old_len;
    int nb = palette_bits(new_len);
    size_t per_long = 64u / (size_t)nb;
    size_t nlongs = (kBlocksPerSection + per_long - 1u) / per_long;
    int64_t *packed = mcme_arena_alloc(arena, nlongs * sizeof *packed);
    if (packed == NULL) {
        set_refusal(refusal, MCME_WRITE_REFUSAL_INTERNAL);
        return false;
    }
    memset(packed, 0, nlongs * sizeof *packed);
    for (size_t p = 0; p < kBlocksPerSection; ++p) {
        size_t idx;
        if (p == pos) {
            idx = target;
        } else if (!palette_index(&v, p, &idx)) {
            set_refusal(refusal, MCME_WRITE_REFUSAL_NOT_READABLE);
            return false;   /* the edit has not touched the tree */
        }
        size_t w = p / per_long;
        size_t bit = (p % per_long) * (size_t)nb;
        packed[w] |= (int64_t)((uint64_t)idx << bit);
    }

    const McmeTag *bs_c = mcme_nbt_get(section, "block_states");
    McmeTag *bs = (McmeTag *)bs_c;
    if (bs == NULL) {
        bs = mcme_nbt_new(arena, MCME_TAG_Compound);
        if (bs == NULL || !mcme_nbt_compound_init(arena, bs, 2)
                || !mcme_nbt_compound_put(arena, section,
                                          "block_states", 12, bs)) {
            set_refusal(refusal, MCME_WRITE_REFUSAL_INTERNAL);
            return false;
        }
    }
    if (target == old_len) {
        McmeTag *pal = mcme_nbt_new(arena, MCME_TAG_List);
        McmeTag *entry = mcme_nbt_new(arena, MCME_TAG_Compound);
        McmeTag *nm = mcme_nbt_new(arena, MCME_TAG_String);
        if (pal == NULL || entry == NULL || nm == NULL
                || !mcme_nbt_list_alloc(arena, pal, MCME_TAG_Compound,
                                        new_len)
                || !mcme_nbt_compound_init(arena, entry, 2)
                || !mcme_nbt_set_string(arena, nm, name, strlen(name))
                || !mcme_nbt_compound_put(arena, entry, "Name", 4,
                                          nm)) {
            set_refusal(refusal, MCME_WRITE_REFUSAL_INTERNAL);
            return false;
        }
        if (states != NULL && states->compound.len > 0) {
            if (!mcme_nbt_compound_put(arena, entry, "Properties",
                                       10, (McmeTag *)states)) {
                set_refusal(refusal, MCME_WRITE_REFUSAL_INTERNAL);
                return false;
            }
        }
        if (v.palette == NULL) {
            /* the implicit air becomes the explicit entry 0 the
               format says it was */
            McmeTag *air = mcme_nbt_new(arena, MCME_TAG_Compound);
            McmeTag *anm = mcme_nbt_new(arena, MCME_TAG_String);
            if (air == NULL || anm == NULL
                    || !mcme_nbt_compound_init(arena, air, 1)
                    || !mcme_nbt_set_string(arena, anm, kAir,
                                            sizeof kAir - 1)
                    || !mcme_nbt_compound_put(arena, air, "Name", 4,
                                              anm)) {
                set_refusal(refusal, MCME_WRITE_REFUSAL_INTERNAL);
                return false;
            }
            pal->list.items[0] = *air;
        } else {
            for (size_t i = 0; i < old_len; ++i) {
                pal->list.items[i] = v.palette->list.items[i];
            }
        }
        pal->list.items[target] = *entry;   /* target == old_len */
        if (!mcme_nbt_compound_put(arena, bs, "palette", 7, pal)) {
            set_refusal(refusal, MCME_WRITE_REFUSAL_INTERNAL);
            return false;
        }
    }
    McmeTag *data = mcme_nbt_new(arena, MCME_TAG_Long_Array);
    if (data == NULL
            || !mcme_nbt_set_longs(arena, data, packed, nlongs)
            || !mcme_nbt_compound_put(arena, bs, "data", 4, data)) {
        set_refusal(refusal, MCME_WRITE_REFUSAL_INTERNAL);
        return false;
    }

    if (changed != NULL) *changed = true;
    return true;
}
