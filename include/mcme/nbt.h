/*
 * NBT — Named Binary Tag, the format Minecraft stores worlds in.
 *
 * Two encodings share one tree:
 *
 *   BINARY  big-endian (Java) or little-endian (Bedrock). Strings and compound
 *           names are modified UTF-8 (see mutf8.h).
 *   SNBT    a text form, for editing and display.
 *
 * This library is validated byte-for-byte against the reference Python
 * `amulet_nbt` 2.1.8. The golden data and the exact behaviours it locks in are
 * recorded in wiki/nbt.md — read that before changing any formatting here,
 * because several of the rules look like bugs and are not:
 *
 *   - An EMPTY list is written with element-type byte 0x01, not 0x00. Whatever
 *     byte was read is preserved, so a round trip is byte-exact.
 *   - SNBT prints compound keys sorted case-insensitively; the BINARY form
 *     keeps file (insertion) order. The two orderings genuinely differ.
 *   - SNBT escapes only `"` and `\`. Control characters, including NUL and
 *     newline, are written raw. Strings are standard UTF-8 in SNBT even though
 *     they are modified UTF-8 in the binary form.
 *   - `inff`, `-inff`, `nanf` are emitted but the reference cannot re-parse
 *     them. We accept them on input so our round trip is stable.
 *
 * Ownership: everything a parse or build allocates comes from the caller's
 * arena. Freeing the arena frees the whole tree. Nothing here owns memory
 * except the output of mcme_nbt_write_*(), which fills a McmeBuffer the caller
 * manages.
 */

#ifndef MCME_NBT_H
#define MCME_NBT_H

#include "mcme/core.h"

/* Nesting limit. Untrusted world files can nest compounds without bound, so
   the parser stops here rather than blowing the C stack. */
#define MCME_NBT_MAX_DEPTH 512

typedef enum {
    MCME_TAG_End        = 0,
    MCME_TAG_Byte       = 1,
    MCME_TAG_Short      = 2,
    MCME_TAG_Int        = 3,
    MCME_TAG_Long       = 4,
    MCME_TAG_Float      = 5,
    MCME_TAG_Double     = 6,
    MCME_TAG_Byte_Array = 7,
    MCME_TAG_String     = 8,
    MCME_TAG_List       = 9,
    MCME_TAG_Compound   = 10,
    MCME_TAG_Int_Array  = 11,
    MCME_TAG_Long_Array = 12,
} McmeTagType;

/*
 * A length-counted UTF-8 string.
 *
 * Standard UTF-8, NOT modified UTF-8. Conversion happens only at the binary
 * boundary. The length is explicit because content may legitimately contain
 * U+0000, which encodes as a 0x00 byte; never use strlen() on these. data[len]
 * is still '\0' so the common ASCII case can be passed to C string functions.
 */
typedef struct {
    char  *data;
    size_t len;
} McmeStr;

typedef struct McmeTag McmeTag;

typedef struct {
    McmeStr   name;    /* the key; standard UTF-8 */
    McmeTag  *value;   /* borrowed; owned by the same arena */
} McmeCompoundEntry;

struct McmeTag {
    McmeTagType type;
    union {
        int8_t   as_byte;
        int16_t  as_short;
        int32_t  as_int;
        int64_t  as_long;
        float    as_float;
        double   as_double;

        struct { int8_t *data; size_t len; } bytes;     /* Byte_Array  */
        McmeStr string;                                 /* String      */
        struct {
            McmeTagType elem;    /* element type; meaningful even when len==0 */
            McmeTag   *items;
            size_t     len;
        } list;                                         /* List        */
        struct {
            McmeCompoundEntry *entries;
            size_t             len;
        } compound;                                     /* Compound    */
        struct { int32_t *data; size_t len; } ints;     /* Int_Array   */
        struct { int64_t *data; size_t len; } longs;    /* Long_Array  */
    };
};

/*
 * A parsed document: a root tag plus the name it carried in the binary form.
 *
 * The arena is BORROWED and must outlive the document. SNBT has no root name,
 * so a document built from SNBT has an empty root_name.
 */
typedef struct {
    McmeArena *arena;
    McmeTag   *root;
    McmeStr    root_name;
    bool       little_endian;   /* true = Bedrock, false = Java */
} McmeNbtDoc;

/* ========================================================================
 * Parsing
 * ======================================================================== */

/*
 * Parses one binary NBT document.
 *
 * `data`/`size` are the raw (uncompressed) NBT bytes. Gzip is handled by the
 * caller — see the region reader in Phase 3 — because NBT itself has no
 * compression and conflating the two makes it impossible to read an
 * uncompressed stream from the middle of a file.
 *
 * Returns false on malformed input; mcme_last_error() says where and why.
 * Every read is bounds-checked, so a truncated or hostile file fails cleanly
 * rather than reading past the buffer.
 */
bool mcme_nbt_parse(McmeNbtDoc *doc, McmeArena *arena,
                    const void *data, size_t size, bool little_endian);

/*
 * Parses SNBT text into a bare tag. SNBT carries no root name, so there is no
 * McmeNbtDoc; wrap the result yourself if you need one.
 */
bool mcme_nbt_parse_snbt(McmeArena *arena, const char *text, size_t len,
                         McmeTag **out);

/* ========================================================================
 * Writing
 * ======================================================================== */

/* Serializes to binary, using the document's recorded endianness. The root
   name is written from doc->root_name, so a round trip reproduces it. */
bool mcme_nbt_write_binary(const McmeNbtDoc *doc, McmeBuffer *out);

/* Same, but forces an endianness other than the document's. */
bool mcme_nbt_write_binary_as(const McmeNbtDoc *doc, McmeBuffer *out,
                              bool little_endian);

/*
 * Serializes to SNBT. `indent` <= 0 produces the compact single-line form the
 * reference emits; > 0 pretty-prints with that many spaces per level and quotes
 * every key.
 *
 * Compound keys are sorted case-insensitively here, matching the reference.
 * Use mcme_nbt_write_binary() when file order must be preserved.
 */
bool mcme_nbt_write_snbt(const McmeTag *tag, McmeBuffer *out, int indent);

/*
 * Serializes to JSON, for the CLI. Type tags are lost (a Long and an Int with
 * the same value look identical) and int64 values beyond 2^53 are not exactly
 * representable. SNBT is the faithful form; JSON exists for tooling.
 */
bool mcme_nbt_write_json(const McmeTag *tag, McmeBuffer *out, int indent);

/* ========================================================================
 * Navigation
 * ======================================================================== */

/* Compound member by key, or NULL. `key` is standard UTF-8. */
const McmeTag *mcme_nbt_get(const McmeTag *tag, const char *key);

/* List or array element, or NULL if out of range or not a list. */
const McmeTag *mcme_nbt_at(const McmeTag *tag, size_t index);

/*
 * Dotted path lookup: mcme_nbt_path(root, "Level.Sections.0.BlockEntities").
 * Each segment indexes a compound by key, or a list by decimal index. Returns
 * NULL if any step is missing or of the wrong shape. The path is not
 * allocated; it is scanned in place.
 */
const McmeTag *mcme_nbt_path(const McmeTag *root, const char *path);

/* Typed getters that fall back instead of failing, for reading fields that may
   legitimately be absent in older world versions. */
int32_t  mcme_nbt_int_or(const McmeTag *tag, int32_t fallback);
int64_t  mcme_nbt_long_or(const McmeTag *tag, int64_t fallback);
int8_t   mcme_nbt_byte_or(const McmeTag *tag, int8_t fallback);
double   mcme_nbt_double_or(const McmeTag *tag, double fallback);
bool     mcme_nbt_is(const McmeTag *tag, McmeTagType type);

const char *mcme_tag_type_name(McmeTagType type);   /* "compound", "int", ... */
const char *mcme_tag_type_snbt_suffix(McmeTagType type); /* "b", "s", "", ... */

/* ========================================================================
 * Building
 *
 * Used by the SNBT parser and by anything that constructs or edits tags. All
 * allocate from the arena you pass.
 * ======================================================================== */

McmeTag *mcme_nbt_new(McmeArena *arena, McmeTagType type);

bool mcme_nbt_set_byte(McmeTag *t, int8_t v);
bool mcme_nbt_set_short(McmeTag *t, int16_t v);
bool mcme_nbt_set_int(McmeTag *t, int32_t v);
bool mcme_nbt_set_long(McmeTag *t, int64_t v);
bool mcme_nbt_set_float(McmeTag *t, float v);
bool mcme_nbt_set_double(McmeTag *t, double v);

/* `utf8` is standard UTF-8; it is stored as-is, so the length is authoritative
   and an embedded NUL is preserved. */
bool mcme_nbt_set_string(McmeArena *arena, McmeTag *t, const char *utf8, size_t len);

bool mcme_nbt_set_bytes(McmeArena *arena, McmeTag *t, const int8_t *data, size_t len);
bool mcme_nbt_set_ints(McmeArena *arena, McmeTag *t, const int32_t *data, size_t len);
bool mcme_nbt_set_longs(McmeArena *arena, McmeTag *t, const int64_t *data, size_t len);

/* Allocates `len` element slots for a list and records the element type.
   For an empty list, pass MCME_TAG_Byte to match what the reference writes. */
bool mcme_nbt_list_alloc(McmeArena *arena, McmeTag *t, McmeTagType elem, size_t len);

/* Prepares an empty compound. `capacity` is a hint; the compound grows as
   entries are added. */
bool mcme_nbt_compound_init(McmeArena *arena, McmeTag *t, size_t capacity);

/*
 * Adds or replaces a compound entry.
 *
 * Replacing keeps the original key spelling and slot position, so adding a key
 * that already exists does not reorder the rest — important because binary
 * output is order-sensitive.
 */
bool mcme_nbt_compound_put(McmeArena *arena, McmeTag *compound,
                           const char *key, size_t key_len, McmeTag *value);

/* Interns a UTF-8 string into the arena as a NUL-terminated McmeStr. */
bool mcme_nbt_str_set(McmeArena *arena, McmeStr *out, const char *utf8, size_t len);

/*
 * Internal — shared by the binary and SNBT parsers.
 *
 * Copies `count` staging entries into the arena and installs them as the tag's
 * compound contents. The caller keeps ownership of `entries` and must free it
 * (arrfree) afterwards. Compounds are built in a growable staging array during
 * parsing because their length is not known until the TAG_End terminator, and
 * an arena cannot realloc in place.
 *
 * Repeated keys collapse here: the first occurrence keeps its position and the
 * last supplies the value, matching the reference (amulet_nbt 2.1.8) on both
 * the binary and SNBT paths. So the installed `len` may be less than `count`.
 */
bool mcme_nbt_compound_seal(McmeArena *arena, McmeTag *out,
                            McmeCompoundEntry *entries, size_t count);

#endif /* MCME_NBT_H */
