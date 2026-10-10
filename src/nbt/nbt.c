#include "mcme/nbt.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mcme/mutf8.h"
#include "stb_ds.h"

/* ========================================================================
 * Reader
 *
 * Every read is bounds-checked against the buffer. A truncated or hostile file
 * must fail cleanly with an error, never read out of bounds. This is the whole
 * reason the parser is written as an explicit cursor rather than pointer casts.
 * ======================================================================== */

typedef struct {
    const uint8_t *data;
    size_t size;
    size_t pos;
    McmeArena *arena;
    bool little_endian;
    int depth;
} NbtReader;

static bool r_need(NbtReader *r, size_t n)
{
    if (n > r->size - r->pos) {
        mcme_set_error("nbt: truncated at byte %zu (need %zu more)", r->pos, n - (r->size - r->pos));
        return false;
    }
    return true;
}

static bool r_u8(NbtReader *r, uint8_t *out)
{
    if (!r_need(r, 1)) return false;
    *out = r->data[r->pos++];
    return true;
}

static bool r_u16(NbtReader *r, uint16_t *out)
{
    if (!r_need(r, 2)) return false;
    const uint8_t *p = r->data + r->pos;
    *out = r->little_endian ? (uint16_t)(p[0] | ((uint16_t)p[1] << 8))
                            : (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
    r->pos += 2;
    return true;
}

static bool r_i32(NbtReader *r, int32_t *out)
{
    if (!r_need(r, 4)) return false;
    const uint8_t *p = r->data + r->pos;
    uint32_t u;
    if (r->little_endian) {
        u = (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
    } else {
        u = ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
    }
    *out = (int32_t)u;
    r->pos += 4;
    return true;
}

static bool r_i64(NbtReader *r, int64_t *out)
{
    if (!r_need(r, 8)) return false;
    const uint8_t *p = r->data + r->pos;
    uint64_t u = 0;
    if (r->little_endian) {
        for (int i = 0; i < 8; ++i) u |= (uint64_t)p[i] << (8 * i);
    } else {
        for (int i = 0; i < 8; ++i) u |= (uint64_t)p[i] << (56 - 8 * i);
    }
    *out = (int64_t)u;
    r->pos += 8;
    return true;
}

static bool r_f32(NbtReader *r, float *out)
{
    int32_t bits;
    if (!r_i32(r, &bits)) return false;
    memcpy(out, &bits, sizeof(*out));   /* bits -> float, host order */
    return true;
}

static bool r_f64(NbtReader *r, double *out)
{
    int64_t bits;
    if (!r_i64(r, &bits)) return false;
    memcpy(out, &bits, sizeof(*out));
    return true;
}

/* Returns a pointer to `n` in-buffer bytes and advances past them. */
static bool r_slice(NbtReader *r, size_t n, const uint8_t **out)
{
    if (!r_need(r, n)) return false;
    *out = r->data + r->pos;
    r->pos += n;
    return true;
}

/* Reads a modified-UTF-8 string and stores it as standard UTF-8 in the arena. */
static bool r_string(NbtReader *r, McmeStr *out)
{
    uint16_t len;
    if (!r_u16(r, &len)) return false;

    const uint8_t *bytes;
    if (!r_slice(r, len, &bytes)) return false;

    McmeBuffer tmp;
    mcme_buffer_init(&tmp);
    if (!mcme_mutf8_decode((const char *)bytes, len, &tmp)) {
        mcme_buffer_free(&tmp);
        return false;
    }

    bool ok = mcme_nbt_str_set(r->arena, out, (const char *)tmp.data, tmp.len);
    mcme_buffer_free(&tmp);
    return ok;
}

/* ========================================================================
 * Payload parsing
 * ======================================================================== */

static bool parse_payload(NbtReader *r, uint8_t type, McmeTag *t);

static bool parse_compound(NbtReader *r, McmeTag *t)
{
    /* Compound length is not known up front — entries run until TAG_End — so
       they are staged in a growable array and copied into the arena once. The
       arena cannot realloc in place, and a two-pass parse would have to
       duplicate all the validation logic. */
    McmeCompoundEntry *entries = NULL;

    for (;;) {
        uint8_t child_type;
        if (!r_u8(r, &child_type)) { arrfree(entries); return false; }

        if (child_type == MCME_TAG_End) break;   /* normal terminator */

        if (child_type > MCME_TAG_Long_Array) {
            mcme_set_error("nbt: bad tag type %u at byte %zu", child_type, r->pos - 1);
            arrfree(entries);
            return false;
        }

        McmeStr name;
        if (!r_string(r, &name)) { arrfree(entries); return false; }

        McmeTag *child = mcme_arena_alloc(r->arena, sizeof(McmeTag));
        if (!child) { arrfree(entries); return false; }

        if (r->depth >= MCME_NBT_MAX_DEPTH) {
            mcme_set_error("nbt: nesting deeper than %d at byte %zu",
                           MCME_NBT_MAX_DEPTH, r->pos);
            arrfree(entries);
            return false;
        }
        r->depth++;
        bool ok = parse_payload(r, child_type, child);
        r->depth--;
        if (!ok) { arrfree(entries); return false; }

        arrput(entries, ((McmeCompoundEntry){ .name = name, .value = child }));
    }

    bool ok = mcme_nbt_compound_seal(r->arena, t, entries, (size_t)arrlen(entries));
    arrfree(entries);
    return ok;
}

static bool parse_list(NbtReader *r, McmeTag *t)
{
    uint8_t elem;
    int32_t n;
    if (!r_u8(r, &elem)) return false;
    if (!r_i32(r, &n)) return false;

    if (n < 0) {
        mcme_set_error("nbt: negative list length %d at byte %zu", n, r->pos - 4);
        return false;
    }

    /* A list of TAG_End with a nonzero count is malformed, and would otherwise
       let a one-byte file request a multi-gigabyte allocation. */
    if (elem == MCME_TAG_End && n > 0) {
        mcme_set_error("nbt: list of End with length %d at byte %zu", n, r->pos - 4);
        return false;
    }

    /* Every non-End element consumes at least one byte, so a count larger than
       the bytes remaining cannot be real. This caps the allocation to a small
       multiple of the input size instead of trusting a raw length field. */
    if ((size_t)n > r->size - r->pos) {
        mcme_set_error("nbt: list length %d exceeds remaining %zu bytes at byte %zu",
                       n, r->size - r->pos, r->pos - 4);
        return false;
    }

    McmeTag *items = NULL;
    if (n > 0) {
        items = mcme_arena_alloc(r->arena, (size_t)n * sizeof(McmeTag));
        if (!items) return false;
        for (int32_t i = 0; i < n; ++i) {
            if (r->depth >= MCME_NBT_MAX_DEPTH) {
                mcme_set_error("nbt: nesting deeper than %d at byte %zu",
                               MCME_NBT_MAX_DEPTH, r->pos);
                return false;
            }
            r->depth++;
            bool ok = parse_payload(r, elem, &items[i]);
            r->depth--;
            if (!ok) return false;
        }
    }

    t->list.elem = (McmeTagType)elem;
    t->list.items = items;
    t->list.len = (size_t)n;
    return true;
}

static bool parse_payload(NbtReader *r, uint8_t type, McmeTag *t)
{
    t->type = (McmeTagType)type;

    switch (type) {
        case MCME_TAG_Byte: {
            if (!r_need(r, 1)) return false;
            t->as_byte = (int8_t)r->data[r->pos++];
            return true;
        }
        case MCME_TAG_Short: {
            uint16_t u;
            if (!r_u16(r, &u)) return false;
            t->as_short = (int16_t)u;
            return true;
        }
        case MCME_TAG_Int:      return r_i32(r, &t->as_int);
        case MCME_TAG_Long:     return r_i64(r, &t->as_long);
        case MCME_TAG_Float:    return r_f32(r, &t->as_float);
        case MCME_TAG_Double:   return r_f64(r, &t->as_double);
        case MCME_TAG_String:   return r_string(r, &t->string);

        case MCME_TAG_Byte_Array: {
            int32_t n;
            if (!r_i32(r, &n)) return false;
            if (n < 0 || (size_t)n > r->size - r->pos) {
                mcme_set_error("nbt: byte array length %d invalid at byte %zu", n, r->pos - 4);
                return false;
            }
            const uint8_t *bytes;
            if (!r_slice(r, (size_t)n, &bytes)) return false;
            int8_t *data = NULL;
            if (n > 0) {
                data = mcme_arena_alloc(r->arena, (size_t)n);
                if (!data) return false;
                memcpy(data, bytes, (size_t)n);
            }
            t->bytes.data = data;
            t->bytes.len = (size_t)n;
            return true;
        }

        case MCME_TAG_Int_Array: {
            int32_t n;
            if (!r_i32(r, &n)) return false;
            if (n < 0 || (size_t)n > (r->size - r->pos) / 4) {
                mcme_set_error("nbt: int array length %d invalid at byte %zu", n, r->pos - 4);
                return false;
            }
            int32_t *data = NULL;
            if (n > 0) {
                data = mcme_arena_alloc(r->arena, (size_t)n * sizeof(int32_t));
                if (!data) return false;
                for (int32_t i = 0; i < n; ++i) {
                    if (!r_i32(r, &data[i])) return false;
                }
            }
            t->ints.data = data;
            t->ints.len = (size_t)n;
            return true;
        }

        case MCME_TAG_Long_Array: {
            int32_t n;
            if (!r_i32(r, &n)) return false;
            if (n < 0 || (size_t)n > (r->size - r->pos) / 8) {
                mcme_set_error("nbt: long array length %d invalid at byte %zu", n, r->pos - 4);
                return false;
            }
            int64_t *data = NULL;
            if (n > 0) {
                data = mcme_arena_alloc(r->arena, (size_t)n * sizeof(int64_t));
                if (!data) return false;
                for (int32_t i = 0; i < n; ++i) {
                    if (!r_i64(r, &data[i])) return false;
                }
            }
            t->longs.data = data;
            t->longs.len = (size_t)n;
            return true;
        }

        case MCME_TAG_List:     return parse_list(r, t);
        case MCME_TAG_Compound: return parse_compound(r, t);

        default:
            mcme_set_error("nbt: tag type %u not valid here at byte %zu", type, r->pos);
            return false;
    }
}

bool mcme_nbt_parse(McmeNbtDoc *doc, McmeArena *arena,
                    const void *data, size_t size, bool little_endian)
{
    if (!doc || !arena || (!data && size > 0)) {
        mcme_set_error("nbt: null argument");
        return false;
    }

    NbtReader r = {
        .data = data,
        .size = size,
        .pos = 0,
        .arena = arena,
        .little_endian = little_endian,
        .depth = 0,
    };

    uint8_t type;
    if (!r_u8(&r, &type)) {
        mcme_set_error("nbt: empty input");
        return false;
    }
    /* The root may be ANY real tag type, not only a compound. The reference
       (amulet_nbt 2.1.8) loads a root TAG_Byte or TAG_List without complaint;
       real Minecraft files just always happen to use a compound. Refusing them
       here would mean failing to read a file the reference reads.
       Only two things are refused: TAG_End, which is a terminator rather than a
       value, and type numbers above MCME_TAG_Long_Array — matching the
       reference's "Tag 0 does not exist" and "Tag 110 does not exist". */
    if (type == MCME_TAG_End || type > MCME_TAG_Long_Array) {
        mcme_set_error("nbt: tag type %u does not exist", type);
        return false;
    }

    McmeStr name;
    if (!r_string(&r, &name)) return false;

    McmeTag *root = mcme_arena_alloc(arena, sizeof(McmeTag));
    if (!root) return false;
    if (!parse_payload(&r, type, root)) return false;

    doc->arena = arena;
    doc->root = root;
    doc->root_name = name;
    doc->little_endian = little_endian;
    return true;
}

/* ========================================================================
 * Building and navigation
 * ======================================================================== */

bool mcme_nbt_str_set(McmeArena *arena, McmeStr *out, const char *utf8, size_t len)
{
    /* Validate here rather than at each caller, because this is the one place
       every string enters the tree: binary read (via r_string), quoted and bare
       SNBT, keys, and the public setter. The invariant this establishes is "a
       string in a tree can always be written back out", so malformed text is
       rejected on read instead of turning into a failed save later — which, for
       an editor, would be silent data loss. */
    if (!mcme_utf8_valid(utf8, len)) {
        mcme_set_error("nbt: string is not valid UTF-8");
        return false;
    }

    char *buf = mcme_arena_alloc(arena, len + 1);
    if (!buf) return false;
    if (len > 0) memcpy(buf, utf8, len);
    buf[len] = '\0';
    out->data = buf;
    out->len = len;
    return true;
}

/*
 * Ordering key for grouping duplicate compound keys.
 *
 * Names sort by their bytes (length-bounded, so embedded NULs compare
 * correctly) with the original index as a tiebreak. The tiebreak gives a total
 * order — a plain name comparison would rank equal names as "equal" and leave
 * qsort free to permute them arbitrarily.
 */
typedef struct {
    const char *data;
    size_t      len;
    size_t      idx;
} SealKey;

/* Names only: used to decide when two sorted keys belong to the same group. */
static int seal_name_cmp(const SealKey *x, const SealKey *y)
{
    size_t n = x->len < y->len ? x->len : y->len;
    int c = n ? memcmp(x->data, y->data, n) : 0;
    if (c != 0) return c;
    if (x->len != y->len) return x->len < y->len ? -1 : 1;
    return 0;
}

static int seal_key_cmp(const void *a, const void *b)
{
    const SealKey *x = (const SealKey *)a;
    const SealKey *y = (const SealKey *)b;
    int c = seal_name_cmp(x, y);
    if (c != 0) return c;
    if (x->idx != y->idx) return x->idx < y->idx ? -1 : 1;
    return 0;
}

bool mcme_nbt_compound_seal(McmeArena *arena, McmeTag *out,
                            McmeCompoundEntry *entries, size_t count)
{
    out->type = MCME_TAG_Compound;
    out->compound.len = 0;
    out->compound.entries = NULL;
    if (count == 0) return true;

    /* A compound's keys are unique. A repeated key collapses the way the
       reference does: the FIRST occurrence keeps its position and the LAST one
       supplies the value (plain Python-dict behaviour), and it does this for
       both the binary and the SNBT reader — amulet turns a 20-byte file with a
       duplicated key back into 12. Doing it here rather than in each parser
       keeps one rule instead of two.

       The grouping is a sort, not a search per key: a compound's entry count
       is attacker-controlled, so looking each key up by scanning would make
       parsing quadratic and let a modest file stall the editor. */
    size_t *src = malloc(count * sizeof *src);
    SealKey *keys = malloc(count * sizeof *keys);
    if (!src || !keys) {
        free(keys);
        free(src);
        mcme_set_error("nbt: out of memory building compound");
        return false;
    }

    for (size_t i = 0; i < count; ++i) {
        src[i] = SIZE_MAX;                      /* SIZE_MAX = not the first copy */
        keys[i].data = entries[i].name.data;
        keys[i].len  = entries[i].name.len;
        keys[i].idx  = i;
    }
    qsort(keys, count, sizeof *keys, seal_key_cmp);

    size_t live = 0;
    for (size_t g = 0; g < count; ) {
        size_t h = g + 1;
        while (h < count && seal_name_cmp(&keys[g], &keys[h]) == 0) ++h;
        /* Sorted by (name, idx): keys[g] is the earliest copy, keys[h-1] the
           latest. Only the earliest survives, carrying the latest's value. */
        src[keys[g].idx] = keys[h - 1].idx;
        live++;
        g = h;
    }

    McmeCompoundEntry *copy = mcme_arena_alloc(arena, live * sizeof *copy);
    bool ok = copy != NULL;
    if (ok) {
        size_t w = 0;
        for (size_t i = 0; i < count; ++i) {
            if (src[i] == SIZE_MAX) continue;
            copy[w] = entries[i];
            copy[w].value = entries[src[i]].value;
            w++;
        }
        out->compound.entries = copy;
        out->compound.len = live;
    } else {
        mcme_set_error("nbt: out of memory building compound");
    }

    free(keys);
    free(src);
    return ok;
}

McmeTag *mcme_nbt_new(McmeArena *arena, McmeTagType type)
{
    McmeTag *t = mcme_arena_alloc(arena, sizeof(McmeTag));
    if (!t) return NULL;
    memset(t, 0, sizeof(*t));
    t->type = type;
    return t;
}

bool mcme_nbt_set_byte(McmeTag *t, int8_t v)   { t->type = MCME_TAG_Byte;   t->as_byte = v;   return true; }
bool mcme_nbt_set_short(McmeTag *t, int16_t v) { t->type = MCME_TAG_Short;  t->as_short = v;  return true; }
bool mcme_nbt_set_int(McmeTag *t, int32_t v)   { t->type = MCME_TAG_Int;    t->as_int = v;    return true; }
bool mcme_nbt_set_long(McmeTag *t, int64_t v)  { t->type = MCME_TAG_Long;   t->as_long = v;   return true; }
bool mcme_nbt_set_float(McmeTag *t, float v)   { t->type = MCME_TAG_Float;  t->as_float = v;  return true; }
bool mcme_nbt_set_double(McmeTag *t, double v) { t->type = MCME_TAG_Double; t->as_double = v; return true; }

bool mcme_nbt_set_string(McmeArena *arena, McmeTag *t, const char *utf8, size_t len)
{
    t->type = MCME_TAG_String;
    return mcme_nbt_str_set(arena, &t->string, utf8, len);
}

bool mcme_nbt_set_bytes(McmeArena *arena, McmeTag *t, const int8_t *data, size_t len)
{
    t->type = MCME_TAG_Byte_Array;
    t->bytes.len = len;
    t->bytes.data = NULL;
    if (len == 0) return true;
    t->bytes.data = mcme_arena_alloc(arena, len);
    if (!t->bytes.data) return false;
    memcpy(t->bytes.data, data, len);
    return true;
}

bool mcme_nbt_set_ints(McmeArena *arena, McmeTag *t, const int32_t *data, size_t len)
{
    t->type = MCME_TAG_Int_Array;
    t->ints.len = len;
    t->ints.data = NULL;
    if (len == 0) return true;
    t->ints.data = mcme_arena_alloc(arena, len * sizeof(int32_t));
    if (!t->ints.data) return false;
    memcpy(t->ints.data, data, len * sizeof(int32_t));
    return true;
}

bool mcme_nbt_set_longs(McmeArena *arena, McmeTag *t, const int64_t *data, size_t len)
{
    t->type = MCME_TAG_Long_Array;
    t->longs.len = len;
    t->longs.data = NULL;
    if (len == 0) return true;
    t->longs.data = mcme_arena_alloc(arena, len * sizeof(int64_t));
    if (!t->longs.data) return false;
    memcpy(t->longs.data, data, len * sizeof(int64_t));
    return true;
}

bool mcme_nbt_list_alloc(McmeArena *arena, McmeTag *t, McmeTagType elem, size_t len)
{
    t->type = MCME_TAG_List;
    t->list.elem = elem;
    t->list.len = len;
    t->list.items = NULL;
    if (len == 0) return true;
    t->list.items = mcme_arena_alloc(arena, len * sizeof(McmeTag));
    if (!t->list.items) return false;
    memset(t->list.items, 0, len * sizeof(McmeTag));
    return true;
}

bool mcme_nbt_compound_init(McmeArena *arena, McmeTag *t, size_t capacity)
{
    (void)arena;
    t->type = MCME_TAG_Compound;
    t->compound.entries = NULL;
    t->compound.len = 0;
    (void)capacity;   /* capacity is a hint; mcme_nbt_compound_put grows as needed */
    return true;
}

bool mcme_nbt_compound_put(McmeArena *arena, McmeTag *compound,
                           const char *key, size_t key_len, McmeTag *value)
{
    if (!compound || compound->type != MCME_TAG_Compound) {
        mcme_set_error("nbt: compound_put on a non-compound tag");
        return false;
    }

    size_t n = compound->compound.len;

    /* Replacing an existing key keeps its slot and spelling so the rest of the
       ordering is untouched — binary output is order-sensitive. */
    for (size_t i = 0; i < n; ++i) {
        McmeCompoundEntry *e = &compound->compound.entries[i];
        if (e->name.len == key_len && memcmp(e->name.data, key, key_len) == 0) {
            e->value = value;
            return true;
        }
    }

    /* Append: allocate a fresh array of n+1 and copy. O(n) per insert. This is
       fine for building and editing compounds; the parsers use a staging array
       instead so they stay O(n). */
    McmeCompoundEntry *arr = mcme_arena_alloc(arena, (n + 1) * sizeof(McmeCompoundEntry));
    if (!arr) return false;
    if (n > 0) memcpy(arr, compound->compound.entries, n * sizeof(McmeCompoundEntry));

    if (!mcme_nbt_str_set(arena, &arr[n].name, key, key_len)) return false;
    arr[n].value = value;

    compound->compound.entries = arr;
    compound->compound.len = n + 1;
    return true;
}

const McmeTag *mcme_nbt_get(const McmeTag *tag, const char *key)
{
    if (!tag || tag->type != MCME_TAG_Compound || !key) return NULL;
    size_t len = strlen(key);
    for (size_t i = 0; i < tag->compound.len; ++i) {
        const McmeCompoundEntry *e = &tag->compound.entries[i];
        if (e->name.len == len && memcmp(e->name.data, key, len) == 0) {
            return e->value;
        }
    }
    return NULL;
}

const McmeTag *mcme_nbt_at(const McmeTag *tag, size_t index)
{
    if (!tag || tag->type != MCME_TAG_List) return NULL;
    if (index >= tag->list.len) return NULL;
    return &tag->list.items[index];
}

const McmeTag *mcme_nbt_path(const McmeTag *root, const char *path)
{
    const McmeTag *cur = root;
    const char *p = path;

    while (cur && *p) {
        const char *dot = strchr(p, '.');
        size_t seglen = dot ? (size_t)(dot - p) : strlen(p);

        if (seglen == 0) {
            /* Tolerate "a..b" by skipping the empty segment. */
            p = dot ? dot + 1 : p + seglen;
            continue;
        }

        char seg[256];
        if (seglen >= sizeof(seg)) return NULL;
        memcpy(seg, p, seglen);
        seg[seglen] = '\0';

        if (cur->type == MCME_TAG_List) {
            char *end;
            long idx = strtol(seg, &end, 10);
            if (*end != '\0' || idx < 0) return NULL;
            cur = mcme_nbt_at(cur, (size_t)idx);
        } else if (cur->type == MCME_TAG_Compound) {
            cur = mcme_nbt_get(cur, seg);
        } else {
            return NULL;
        }

        p = dot ? dot + 1 : p + seglen;
    }
    return cur;
}

int32_t mcme_nbt_int_or(const McmeTag *tag, int32_t fallback)
{
    if (!tag) return fallback;
    switch (tag->type) {
        case MCME_TAG_Int:   return tag->as_int;
        case MCME_TAG_Byte:  return tag->as_byte;
        case MCME_TAG_Short: return tag->as_short;
        case MCME_TAG_Long:  return (int32_t)tag->as_long;
        default: return fallback;
    }
}

int64_t mcme_nbt_long_or(const McmeTag *tag, int64_t fallback)
{
    if (!tag) return fallback;
    switch (tag->type) {
        case MCME_TAG_Long:  return tag->as_long;
        case MCME_TAG_Int:   return tag->as_int;
        case MCME_TAG_Short: return tag->as_short;
        case MCME_TAG_Byte:  return tag->as_byte;
        default: return fallback;
    }
}

int8_t mcme_nbt_byte_or(const McmeTag *tag, int8_t fallback)
{
    if (!tag) return fallback;
    switch (tag->type) {
        case MCME_TAG_Byte:  return tag->as_byte;
        case MCME_TAG_Int:   return (int8_t)tag->as_int;
        case MCME_TAG_Short: return (int8_t)tag->as_short;
        default: return fallback;
    }
}

double mcme_nbt_double_or(const McmeTag *tag, double fallback)
{
    if (!tag) return fallback;
    switch (tag->type) {
        case MCME_TAG_Double: return tag->as_double;
        case MCME_TAG_Float:  return (double)tag->as_float;
        case MCME_TAG_Int:    return (double)tag->as_int;
        case MCME_TAG_Long:   return (double)tag->as_long;
        default: return fallback;
    }
}

bool mcme_nbt_is(const McmeTag *tag, McmeTagType type)
{
    return tag != NULL && tag->type == type;
}

const char *mcme_tag_type_name(McmeTagType type)
{
    switch (type) {
        case MCME_TAG_Byte:       return "byte";
        case MCME_TAG_Short:      return "short";
        case MCME_TAG_Int:        return "int";
        case MCME_TAG_Long:       return "long";
        case MCME_TAG_Float:      return "float";
        case MCME_TAG_Double:     return "double";
        case MCME_TAG_Byte_Array: return "byte_array";
        case MCME_TAG_String:     return "string";
        case MCME_TAG_List:       return "list";
        case MCME_TAG_Compound:   return "compound";
        case MCME_TAG_Int_Array:  return "int_array";
        case MCME_TAG_Long_Array: return "long_array";
        default:                  return "end";
    }
}

const char *mcme_tag_type_snbt_suffix(McmeTagType type)
{
    switch (type) {
        case MCME_TAG_Byte:   return "b";
        case MCME_TAG_Short:  return "s";
        case MCME_TAG_Long:   return "L";
        case MCME_TAG_Float:  return "f";
        case MCME_TAG_Double: return "d";
        default:              return "";
    }
}

/* ========================================================================
 * Binary writing
 * ======================================================================== */

static bool write_payload(const McmeTag *t, McmeBuffer *out, bool le);

/* Encodes a UTF-8 string as modified UTF-8 and writes it length-prefixed. */
static bool write_string(const McmeStr *s, McmeBuffer *out, bool le)
{
    McmeBuffer tmp;
    mcme_buffer_init(&tmp);
    if (!mcme_mutf8_encode(s->data, s->len, &tmp)) {
        mcme_buffer_free(&tmp);
        return false;
    }
    if (tmp.len > 0xFFFF) {
        mcme_set_error("nbt: string of %zu bytes exceeds the 65535 limit", tmp.len);
        mcme_buffer_free(&tmp);
        return false;
    }
    bool ok = (le ? mcme_buffer_append_u16le(out, (uint16_t)tmp.len)
                  : mcme_buffer_append_u16be(out, (uint16_t)tmp.len))
           && mcme_buffer_append(out, tmp.data, tmp.len);
    mcme_buffer_free(&tmp);
    return ok;
}

static bool write_payload(const McmeTag *t, McmeBuffer *out, bool le)
{
    switch (t->type) {
        case MCME_TAG_Byte:
            return mcme_buffer_append_u8(out, (uint8_t)t->as_byte);

        case MCME_TAG_Short:
            return le ? mcme_buffer_append_u16le(out, (uint16_t)t->as_short)
                      : mcme_buffer_append_u16be(out, (uint16_t)t->as_short);

        case MCME_TAG_Int:
            return le ? mcme_buffer_append_i32le(out, t->as_int)
                      : mcme_buffer_append_i32be(out, t->as_int);

        case MCME_TAG_Long:
            return le ? mcme_buffer_append_i64le(out, t->as_long)
                      : mcme_buffer_append_i64be(out, t->as_long);

        case MCME_TAG_Float:
            return le ? mcme_buffer_append_f32le(out, t->as_float)
                      : mcme_buffer_append_f32be(out, t->as_float);

        case MCME_TAG_Double:
            return le ? mcme_buffer_append_f64le(out, t->as_double)
                      : mcme_buffer_append_f64be(out, t->as_double);

        case MCME_TAG_String:
            return write_string(&t->string, out, le);

        case MCME_TAG_Byte_Array: {
            int32_t n = (int32_t)t->bytes.len;
            if (le) {
                if (!mcme_buffer_append_i32le(out, n)) return false;
            } else {
                if (!mcme_buffer_append_i32be(out, n)) return false;
            }
            return mcme_buffer_append(out, t->bytes.data, t->bytes.len);
        }

        case MCME_TAG_Int_Array: {
            int32_t n = (int32_t)t->ints.len;
            if (le) {
                if (!mcme_buffer_append_i32le(out, n)) return false;
                for (size_t i = 0; i < t->ints.len; ++i)
                    if (!mcme_buffer_append_i32le(out, t->ints.data[i])) return false;
            } else {
                if (!mcme_buffer_append_i32be(out, n)) return false;
                for (size_t i = 0; i < t->ints.len; ++i)
                    if (!mcme_buffer_append_i32be(out, t->ints.data[i])) return false;
            }
            return true;
        }

        case MCME_TAG_Long_Array: {
            int32_t n = (int32_t)t->longs.len;
            if (le) {
                if (!mcme_buffer_append_i32le(out, n)) return false;
                for (size_t i = 0; i < t->longs.len; ++i)
                    if (!mcme_buffer_append_i64le(out, t->longs.data[i])) return false;
            } else {
                if (!mcme_buffer_append_i32be(out, n)) return false;
                for (size_t i = 0; i < t->longs.len; ++i)
                    if (!mcme_buffer_append_i64be(out, t->longs.data[i])) return false;
            }
            return true;
        }

        case MCME_TAG_List: {
            uint8_t elem = (uint8_t)t->list.elem;
            int32_t n = (int32_t)t->list.len;
            if (!mcme_buffer_append_u8(out, elem)) return false;
            if (le) {
                if (!mcme_buffer_append_i32le(out, n)) return false;
            } else {
                if (!mcme_buffer_append_i32be(out, n)) return false;
            }
            for (size_t i = 0; i < t->list.len; ++i) {
                if (!write_payload(&t->list.items[i], out, le)) return false;
            }
            return true;
        }

        case MCME_TAG_Compound: {
            for (size_t i = 0; i < t->compound.len; ++i) {
                const McmeCompoundEntry *e = &t->compound.entries[i];
                if (!mcme_buffer_append_u8(out, (uint8_t)e->value->type)) return false;
                if (!write_string(&e->name, out, le)) return false;
                if (!write_payload(e->value, out, le)) return false;
            }
            return mcme_buffer_append_u8(out, (uint8_t)MCME_TAG_End);
        }

        default:
            mcme_set_error("nbt: cannot write tag type %d", (int)t->type);
            return false;
    }
}

bool mcme_nbt_write_binary_as(const McmeNbtDoc *doc, McmeBuffer *out, bool le)
{
    if (!doc || !doc->root || !out) {
        mcme_set_error("nbt: null argument");
        return false;
    }
    if (!mcme_buffer_append_u8(out, (uint8_t)doc->root->type)) return false;
    if (!write_string(&doc->root_name, out, le)) return false;
    return write_payload(doc->root, out, le);
}

bool mcme_nbt_write_binary(const McmeNbtDoc *doc, McmeBuffer *out)
{
    return mcme_nbt_write_binary_as(doc, out, doc->little_endian);
}

/* ========================================================================
 * Number formatting
 *
 * SNBT numbers are printed exactly the way Python's repr() formats a double,
 * because that is what the reference emits and our golden data diffs against
 * it byte-for-byte. That means:
 *
 *   - the SHORTEST decimal string that reads back as the same double
 *   - scientific notation when decpt <= -4 or decpt > digit count
 *   - exponents always signed and at least two digits (e+38, e-05, e-323)
 *   - a trailing ".0" so a whole number still reads as a float
 * ======================================================================== */

static int ascii_lower(int c) { return (c >= 'A' && c <= 'Z') ? c + 32 : c; }
static int ascii_isupper(int c) { return c >= 'A' && c <= 'Z'; }

/*
 * Reformats a printf %e string ("d.ddddde±XX") into Python repr style.
 *
 * `tmp` must already be the shortest round-tripping form. We only reshape it:
 * decide fixed vs scientific using CPython's rule, then lay out the digits.
 */
static void repr_reshape(const char *tmp, char *buf, size_t cap)
{
    const char *p = tmp;
    bool neg = false;
    if (*p == '-') { neg = true; p++; }

    char digits[32];
    int nd = 0;
    while (*p && *p != 'e' && nd < (int)sizeof(digits) - 1) {
        if (isdigit((unsigned char)*p)) digits[nd++] = *p;
        p++;
    }
    if (*p == '.') {
        p++;
        while (*p && *p != 'e' && nd < (int)sizeof(digits) - 1) {
            if (isdigit((unsigned char)*p)) digits[nd++] = *p;
            p++;
        }
    }
    digits[nd] = '\0';

    int exp = 0;
    if (*p == 'e') exp = atoi(p + 1);

    int decpt = exp + 1;
    int precision = nd;

    /* CPython's fixed-vs-scientific decision. */
    bool use_exp = (decpt <= -4) || (decpt > precision);

    size_t o = 0;
    if (neg) { if (o + 1 < cap) buf[o++] = '-'; }

    if (use_exp) {
        if (o + 1 < cap) buf[o++] = digits[0];
        if (nd > 1) {
            if (o + 1 < cap) buf[o++] = '.';
            for (int i = 1; i < nd && o + 1 < cap; ++i) buf[o++] = digits[i];
        }
        int w = snprintf(buf + o, cap > o ? cap - o : 0, "e%+03d", exp);
        if (w > 0) o += (size_t)w;
    } else {
        if (decpt <= 0) {
            /* 0.000ddd — leading zeros after the point. */
            if (o + 2 < cap) { buf[o++] = '0'; buf[o++] = '.'; }
            for (int i = 0; i < -decpt && o + 1 < cap; ++i) buf[o++] = '0';
            for (int i = 0; i < nd && o + 1 < cap; ++i) buf[o++] = digits[i];
        } else if (decpt >= precision) {
            /* ddd00.0 — trailing zeros, then a forced decimal point. */
            for (int i = 0; i < nd && o + 1 < cap; ++i) buf[o++] = digits[i];
            for (int i = 0; i < decpt - precision && o + 1 < cap; ++i) buf[o++] = '0';
            if (o + 2 < cap) { buf[o++] = '.'; buf[o++] = '0'; }
        } else {
            /* ddd.ddd — decimal point in the middle. */
            for (int i = 0; i < decpt && o + 1 < cap; ++i) buf[o++] = digits[i];
            if (o + 1 < cap) buf[o++] = '.';
            for (int i = decpt; i < nd && o + 1 < cap; ++i) buf[o++] = digits[i];
        }
    }
    if (o < cap) buf[o] = '\0'; else buf[cap - 1] = '\0';
}

/* Formats a double in Python repr style into buf. */
static void format_double_repr(double v, char *buf, size_t cap)
{
    if (isnan(v)) { snprintf(buf, cap, "nan"); return; }
    if (isinf(v)) { snprintf(buf, cap, v < 0 ? "-inf" : "inf"); return; }
    if (v == 0.0) {
        /* Distinguish -0.0, which NBT preserves. */
        snprintf(buf, cap, signbit(v) ? "-0.0" : "0.0");
        return;
    }

    /* Shortest round-trip: widen precision until the text reads back as the
       identical double. Doubles need at most 17 significant digits. */
    for (int prec = 0; prec <= 16; ++prec) {
        char tmp[64];
        snprintf(tmp, sizeof(tmp), "%.*e", prec, v);
        if (strtod(tmp, NULL) == v) {
            repr_reshape(tmp, buf, cap);
            return;
        }
    }
    snprintf(buf, cap, "%.17g", v);   /* unreachable for finite doubles */
}

/* Formats a float by widening to double exactly, then repr-ing. A float32's
   value widened to double is exact, and that is what the reference prints. */
static void format_float_repr(float v, char *buf, size_t cap)
{
    format_double_repr((double)v, buf, cap);
}

/* ========================================================================
 * SNBT writing
 * ======================================================================== */

/* SNBT compound keys sort case-insensitively; at the first character that is
   equal folded but differs in case, lowercase sorts before uppercase. This is
   why the reference prints {ab, aB, Ab, AB} in that order. */
static int snbt_key_strcmp(const char *a, const char *b)
{
    const unsigned char *pa = (const unsigned char *)a;
    const unsigned char *pb = (const unsigned char *)b;

    while (*pa && *pb) {
        int fa = ascii_lower(*pa), fb = ascii_lower(*pb);
        if (fa != fb) return fa < fb ? -1 : 1;
        if (*pa != *pb) {
            int aup = ascii_isupper(*pa), bup = ascii_isupper(*pb);
            if (aup != bup) return aup ? 1 : -1;
        }
        pa++;
        pb++;
    }
    if (*pa) return 1;
    if (*pb) return -1;
    return 0;
}

/* qsort comparator over pointers-to-entry, so we can sort without moving the
   entries themselves (which live in the arena and are order-sensitive). */
static int snbt_key_cmp_ptr(const void *pa, const void *pb)
{
    const McmeCompoundEntry *a = *(const McmeCompoundEntry * const *)pa;
    const McmeCompoundEntry *b = *(const McmeCompoundEntry * const *)pb;
    return snbt_key_strcmp(a->name.data, b->name.data);
}

/* True if every byte is in the set the reference leaves unquoted. */
static bool snbt_key_is_bare(const char *s, size_t len)
{
    if (len == 0) return false;
    for (size_t i = 0; i < len; ++i) {
        unsigned char c = (unsigned char)s[i];
        bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
               || (c >= '0' && c <= '9')
               || c == '_' || c == '.' || c == '+' || c == '-';
        if (!ok) return false;
    }
    return true;
}

/* Writes a UTF-8 string with SNBT's escaping: always quoted, and only `"` and
   backslash are escaped. Control characters are written raw. */
static bool snbt_write_string(const McmeStr *s, McmeBuffer *out)
{
    if (!mcme_buffer_append_char(out, '"')) return false;
    for (size_t i = 0; i < s->len; ++i) {
        char c = s->data[i];
        if (c == '"' || c == '\\') {
            if (!mcme_buffer_append_char(out, '\\')) return false;
        }
        if (!mcme_buffer_append_char(out, c)) return false;
    }
    return mcme_buffer_append_char(out, '"');
}

static bool snbt_indent(McmeBuffer *out, int indent, int depth)
{
    if (indent <= 0) return true;
    if (!mcme_buffer_append_char(out, '\n')) return false;
    for (int i = 0; i < indent * depth; ++i) {
        if (!mcme_buffer_append_char(out, ' ')) return false;
    }
    return true;
}

static bool snbt_write_value(const McmeTag *t, McmeBuffer *out, int indent, int depth);

static bool snbt_write_compound(const McmeTag *t, McmeBuffer *out, int indent, int depth)
{
    size_t n = t->compound.len;
    if (!mcme_buffer_append_char(out, '{')) return false;

    if (n == 0) return mcme_buffer_append_char(out, '}');

    /* Sort a copy of the entry pointers; the entries themselves stay put. */
    const McmeCompoundEntry **order = malloc(n * sizeof(*order));
    if (!order) {
        mcme_set_error("nbt: out of memory sorting %zu keys", n);
        return false;
    }
    for (size_t i = 0; i < n; ++i) order[i] = &t->compound.entries[i];
    qsort(order, n, sizeof(*order), snbt_key_cmp_ptr);

    bool ok = true;
    for (size_t i = 0; ok && i < n; ++i) {
        /* Compact form separates entries with ", "; pretty form with "," plus
           a newline, which snbt_indent() emits next. The reference's compact
           output is "{a: 1, b: 2}" — the space matters for byte-exactness. */
        if (i > 0) {
            ok = mcme_buffer_append_char(out, ',');
            if (ok && indent <= 0) ok = mcme_buffer_append_char(out, ' ');
        }
        if (ok) ok = snbt_indent(out, indent, depth + 1);
        if (!ok) break;

        const McmeStr *key = &order[i]->name;
        if (indent > 0 || !snbt_key_is_bare(key->data, key->len)) {
            ok = snbt_write_string(key, out);
        } else {
            ok = mcme_buffer_append(out, key->data, key->len);
        }
        if (!ok) break;
        ok = mcme_buffer_append_cstr(out, indent > 0 ? ": " : ": ");
        if (!ok) break;
        ok = snbt_write_value(order[i]->value, out, indent, depth + 1);
    }
    free(order);
    if (!ok) return false;

    if (indent > 0) ok = snbt_indent(out, indent, depth);
    return ok && mcme_buffer_append_char(out, '}');
}

static bool snbt_write_list(const McmeTag *t, McmeBuffer *out, int indent, int depth)
{
    if (!mcme_buffer_append_char(out, '[')) return false;
    for (size_t i = 0; i < t->list.len; ++i) {
        if (i > 0) {
            if (!mcme_buffer_append_char(out, ',')) return false;
            if (indent <= 0 && !mcme_buffer_append_char(out, ' ')) return false;
        }
        if (t->list.len > 1 && indent > 0 && !snbt_indent(out, indent, depth + 1)) return false;
        if (!snbt_write_value(&t->list.items[i], out, indent, depth + 1)) return false;
    }
    if (indent > 0 && t->list.len > 0 && !snbt_indent(out, indent, depth)) return false;
    return mcme_buffer_append_char(out, ']');
}

/* Array literals carry an element-type marker and per-element suffixes. Note
   the byte-array elements use UPPERCASE B, unlike a standalone ByteTag's
   lowercase b — matching the reference exactly. */
static bool snbt_write_array(const McmeTag *t, McmeBuffer *out)
{
    switch (t->type) {
        case MCME_TAG_Byte_Array:
            if (!mcme_buffer_append_cstr(out, "[B;")) return false;
            for (size_t i = 0; i < t->bytes.len; ++i) {
                if (i > 0 && !mcme_buffer_append_cstr(out, ", ")) return false;
                char tmp[16];
                snprintf(tmp, sizeof(tmp), "%dB", (int)t->bytes.data[i]);
                if (!mcme_buffer_append_cstr(out, tmp)) return false;
            }
            return mcme_buffer_append_char(out, ']');

        case MCME_TAG_Int_Array:
            if (!mcme_buffer_append_cstr(out, "[I;")) return false;
            for (size_t i = 0; i < t->ints.len; ++i) {
                if (i > 0 && !mcme_buffer_append_cstr(out, ", ")) return false;
                char tmp[16];
                snprintf(tmp, sizeof(tmp), "%d", t->ints.data[i]);
                if (!mcme_buffer_append_cstr(out, tmp)) return false;
            }
            return mcme_buffer_append_char(out, ']');

        case MCME_TAG_Long_Array:
            if (!mcme_buffer_append_cstr(out, "[L;")) return false;
            for (size_t i = 0; i < t->longs.len; ++i) {
                if (i > 0 && !mcme_buffer_append_cstr(out, ", ")) return false;
                char tmp[24];
                snprintf(tmp, sizeof(tmp), "%lldL", (long long)t->longs.data[i]);
                if (!mcme_buffer_append_cstr(out, tmp)) return false;
            }
            return mcme_buffer_append_char(out, ']');

        default:
            return false;
    }
}

static bool snbt_write_value(const McmeTag *t, McmeBuffer *out, int indent, int depth)
{
    char tmp[64];

    switch (t->type) {
        case MCME_TAG_Byte:
            snprintf(tmp, sizeof(tmp), "%db", (int)t->as_byte);
            return mcme_buffer_append_cstr(out, tmp);

        case MCME_TAG_Short:
            snprintf(tmp, sizeof(tmp), "%ds", (int)t->as_short);
            return mcme_buffer_append_cstr(out, tmp);

        case MCME_TAG_Int:
            snprintf(tmp, sizeof(tmp), "%d", t->as_int);
            return mcme_buffer_append_cstr(out, tmp);

        case MCME_TAG_Long:
            snprintf(tmp, sizeof(tmp), "%lldL", (long long)t->as_long);
            return mcme_buffer_append_cstr(out, tmp);

        case MCME_TAG_Float:
            format_float_repr(t->as_float, tmp, sizeof(tmp));
            if (!mcme_buffer_append_cstr(out, tmp)) return false;
            return mcme_buffer_append_cstr(out, "f");

        case MCME_TAG_Double:
            format_double_repr(t->as_double, tmp, sizeof(tmp));
            if (!mcme_buffer_append_cstr(out, tmp)) return false;
            return mcme_buffer_append_cstr(out, "d");

        case MCME_TAG_String:
            return snbt_write_string(&t->string, out);

        case MCME_TAG_Compound:
            return snbt_write_compound(t, out, indent, depth);

        case MCME_TAG_List:
            return snbt_write_list(t, out, indent, depth);

        case MCME_TAG_Byte_Array:
        case MCME_TAG_Int_Array:
        case MCME_TAG_Long_Array:
            return snbt_write_array(t, out);

        default:
            mcme_set_error("nbt: cannot write tag type %d as SNBT", (int)t->type);
            return false;
    }
}

bool mcme_nbt_write_snbt(const McmeTag *tag, McmeBuffer *out, int indent)
{
    if (!tag || !out) {
        mcme_set_error("nbt: null argument");
        return false;
    }
    return snbt_write_value(tag, out, indent, 0);
}

/* ========================================================================
 * JSON writing
 *
 * For the CLI. Loses type tags and cannot represent NaN/Infinity (emitted as
 * null). SNBT is the faithful form; this exists for tooling that eats JSON.
 * ======================================================================== */

static bool json_write_escaped(const char *s, size_t len, McmeBuffer *out)
{
    if (!mcme_buffer_append_char(out, '"')) return false;
    for (size_t i = 0; i < len; ++i) {
        unsigned char c = (unsigned char)s[i];
        switch (c) {
            case '"':  if (!mcme_buffer_append_cstr(out, "\\\"")) return false; break;
            case '\\': if (!mcme_buffer_append_cstr(out, "\\\\")) return false; break;
            case '\n': if (!mcme_buffer_append_cstr(out, "\\n")) return false; break;
            case '\r': if (!mcme_buffer_append_cstr(out, "\\r")) return false; break;
            case '\t': if (!mcme_buffer_append_cstr(out, "\\t")) return false; break;
            default:
                if (c < 0x20) {
                    char tmp[8];
                    snprintf(tmp, sizeof(tmp), "\\u%04x", c);
                    if (!mcme_buffer_append_cstr(out, tmp)) return false;
                } else {
                    if (!mcme_buffer_append_u8(out, c)) return false;
                }
                break;
        }
    }
    return mcme_buffer_append_char(out, '"');
}

static bool json_indent(McmeBuffer *out, int indent, int depth)
{
    if (indent <= 0) return true;
    if (!mcme_buffer_append_char(out, '\n')) return false;
    for (int i = 0; i < indent * depth; ++i) {
        if (!mcme_buffer_append_char(out, ' ')) return false;
    }
    return true;
}

static bool json_write_value(const McmeTag *t, McmeBuffer *out, int indent, int depth)
{
    char tmp[64];

    switch (t->type) {
        case MCME_TAG_Byte:
            snprintf(tmp, sizeof(tmp), "%d", (int)t->as_byte);
            return mcme_buffer_append_cstr(out, tmp);
        case MCME_TAG_Short:
            snprintf(tmp, sizeof(tmp), "%d", (int)t->as_short);
            return mcme_buffer_append_cstr(out, tmp);
        case MCME_TAG_Int:
            snprintf(tmp, sizeof(tmp), "%d", t->as_int);
            return mcme_buffer_append_cstr(out, tmp);
        case MCME_TAG_Long:
            snprintf(tmp, sizeof(tmp), "%lld", (long long)t->as_long);
            return mcme_buffer_append_cstr(out, tmp);

        case MCME_TAG_Float: {
            float f = t->as_float;
            if (isnan(f) || isinf(f)) return mcme_buffer_append_cstr(out, "null");
            format_float_repr(f, tmp, sizeof(tmp));
            return mcme_buffer_append_cstr(out, tmp);
        }
        case MCME_TAG_Double: {
            double d = t->as_double;
            if (isnan(d) || isinf(d)) return mcme_buffer_append_cstr(out, "null");
            format_double_repr(d, tmp, sizeof(tmp));
            return mcme_buffer_append_cstr(out, tmp);
        }

        case MCME_TAG_String:
            return json_write_escaped(t->string.data, t->string.len, out);

        case MCME_TAG_Compound: {
            if (!mcme_buffer_append_char(out, '{')) return false;
            for (size_t i = 0; i < t->compound.len; ++i) {
                if (i > 0 && !mcme_buffer_append_char(out, ',')) return false;
                if (!json_indent(out, indent, depth + 1)) return false;
                const McmeCompoundEntry *e = &t->compound.entries[i];
                if (!json_write_escaped(e->name.data, e->name.len, out)) return false;
                if (!mcme_buffer_append_cstr(out, indent > 0 ? ": " : ":")) return false;
                if (!json_write_value(e->value, out, indent, depth + 1)) return false;
            }
            if (t->compound.len > 0 && !json_indent(out, indent, depth)) return false;
            return mcme_buffer_append_char(out, '}');
        }

        case MCME_TAG_List: {
            if (!mcme_buffer_append_char(out, '[')) return false;
            for (size_t i = 0; i < t->list.len; ++i) {
                if (i > 0 && !mcme_buffer_append_char(out, ',')) return false;
                if (!json_indent(out, indent, depth + 1)) return false;
                if (!json_write_value(&t->list.items[i], out, indent, depth + 1)) return false;
            }
            if (t->list.len > 0 && !json_indent(out, indent, depth)) return false;
            return mcme_buffer_append_char(out, ']');
        }

        case MCME_TAG_Byte_Array:
            if (!mcme_buffer_append_char(out, '[')) return false;
            for (size_t i = 0; i < t->bytes.len; ++i) {
                if (i > 0 && !mcme_buffer_append_char(out, ',')) return false;
                snprintf(tmp, sizeof(tmp), "%d", (int)t->bytes.data[i]);
                if (!mcme_buffer_append_cstr(out, tmp)) return false;
            }
            return mcme_buffer_append_char(out, ']');

        case MCME_TAG_Int_Array:
            if (!mcme_buffer_append_char(out, '[')) return false;
            for (size_t i = 0; i < t->ints.len; ++i) {
                if (i > 0 && !mcme_buffer_append_char(out, ',')) return false;
                snprintf(tmp, sizeof(tmp), "%d", t->ints.data[i]);
                if (!mcme_buffer_append_cstr(out, tmp)) return false;
            }
            return mcme_buffer_append_char(out, ']');

        case MCME_TAG_Long_Array:
            if (!mcme_buffer_append_char(out, '[')) return false;
            for (size_t i = 0; i < t->longs.len; ++i) {
                if (i > 0 && !mcme_buffer_append_char(out, ',')) return false;
                snprintf(tmp, sizeof(tmp), "%lld", (long long)t->longs.data[i]);
                if (!mcme_buffer_append_cstr(out, tmp)) return false;
            }
            return mcme_buffer_append_char(out, ']');

        default:
            mcme_set_error("nbt: cannot write tag type %d as JSON", (int)t->type);
            return false;
    }
}

bool mcme_nbt_write_json(const McmeTag *tag, McmeBuffer *out, int indent)
{
    if (!tag || !out) {
        mcme_set_error("nbt: null argument");
        return false;
    }
    return json_write_value(tag, out, indent, 0);
}
