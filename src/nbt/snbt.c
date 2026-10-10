#include "mcme/nbt.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "stb_ds.h"

/*
 * SNBT parser.
 *
 * A recursive-descent parser for the text form of NBT. Beyond the reference's
 * behaviour it deliberately accepts two things the reference does not, because
 * they make round-trips stable:
 *
 *   1. `inff`, `-inff`, `nanf` (and their `d` forms) parse as float/double
 *      infinity and NaN. The reference EMITS these but re-parses them as
 *      strings, so its own output does not survive a round trip.
 *   2. Bare `inf`/`nan`/`-inf` parse as doubles, same reason.
 *
 * Everything else matches amulet_nbt 2.1.8, including: empty lists get
 * element-type Byte, arrays use `[B;…]`/`[I;…]`/`[L;…]`, `true`/`false` are
 * bytes, and unquoted barewords are strings.
 */

typedef struct {
    const char *text;
    size_t len;
    size_t pos;
    McmeArena *arena;
    int depth;
} SnbtParser;

static int ascii_lower(int c) { return (c >= 'A' && c <= 'Z') ? c + 32 : c; }

static void skip_ws(SnbtParser *p)
{
    while (p->pos < p->len) {
        char c = p->text[p->pos];
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') p->pos++;
        else break;
    }
}

static bool at_end(SnbtParser *p) { return p->pos >= p->len; }
static char peek(SnbtParser *p)   { return p->pos < p->len ? p->text[p->pos] : '\0'; }

/* Reports a parse failure at `pos`, with line/column so a bad SNBT file is
   easy to locate. Keeping the position explicit lets a caller point at where
   the offending construct BEGAN rather than where the parser stopped. */
static bool fail_at(SnbtParser *p, size_t pos, const char *what)
{
    size_t line = 1, col = 1;
    for (size_t i = 0; i < pos && i < p->len; ++i) {
        if (p->text[i] == '\n') { line++; col = 1; } else col++;
    }
    mcme_set_error("snbt: %s at line %zu column %zu (byte %zu)",
                   what, line, col, pos);
    return false;
}

static bool fail(SnbtParser *p, const char *what)
{
    return fail_at(p, p->pos, what);
}

/* Length-bounded, case-insensitive ASCII comparison. */
static bool ieq_n(const char *a, size_t alen, const char *b)
{
    size_t blen = strlen(b);
    if (alen != blen) return false;
    for (size_t i = 0; i < alen; ++i) {
        if (ascii_lower((unsigned char)a[i]) != ascii_lower((unsigned char)b[i])) {
            return false;
        }
    }
    return true;
}

static bool parse_value(SnbtParser *p, McmeTag **out);

/* ------------------------------------------------------------------
 * Quoted strings
 * ------------------------------------------------------------------ */

/* Reads a quoted string (single or double), resolving SNBT's two escapes.
   Content is standard UTF-8; stored length-counted so an embedded NUL (which
   the reference writes raw) survives. */
static bool parse_quoted(SnbtParser *p, McmeStr *out)
{
    if (at_end(p)) return fail(p, "unexpected end inside string");
    char quote = peek(p);
    p->pos++;

    McmeBuffer buf;
    mcme_buffer_init(&buf);
    bool closed = false;

    while (!at_end(p)) {
        char c = peek(p);
        if (c == quote) { p->pos++; closed = true; break; }

        if (c == '\\' && p->pos + 1 < p->len) {
            char nx = p->text[p->pos + 1];
            /* The reference escapes only `"` and backslash. We also accept
               `\'` and a backslash before the active quote, and pass any other
               backslash sequence through unchanged so nothing is mangled. */
            if (nx == '\\' ) {
                if (!mcme_buffer_append_char(&buf, '\\')) { mcme_buffer_free(&buf); return false; }
                p->pos += 2;
                continue;
            }
            if (nx == '"' || nx == '\'' || nx == quote) {
                if (!mcme_buffer_append_char(&buf, nx)) { mcme_buffer_free(&buf); return false; }
                p->pos += 2;
                continue;
            }
            if (!mcme_buffer_append_char(&buf, '\\')) { mcme_buffer_free(&buf); return false; }
            if (!mcme_buffer_append_char(&buf, nx)) { mcme_buffer_free(&buf); return false; }
            p->pos += 2;
            continue;
        }

        if (!mcme_buffer_append_char(&buf, c)) { mcme_buffer_free(&buf); return false; }
        p->pos++;
    }

    if (!closed) {
        mcme_buffer_free(&buf);
        return fail(p, "unterminated string");
    }

    bool ok = mcme_nbt_str_set(p->arena, out, (const char *)buf.data, buf.len);
    mcme_buffer_free(&buf);
    return ok;
}

/* ------------------------------------------------------------------
 * Numbers
 * ------------------------------------------------------------------ */

/* Builds a numeric tag from a token that may carry a type suffix. */
static bool parse_number_token(SnbtParser *p, const char *tok, McmeTag **out)
{
    size_t len = strlen(tok);
    char suffix = 0;
    size_t nlen = len;

    /* Strip a trailing type suffix. It only counts when something numeric is
       left, so a lone "b" stays a bareword rather than becoming a suffix with
       an empty number. */
    if (len > 1) {
        char last = (char)ascii_lower((unsigned char)tok[len - 1]);
        if (last == 'b' || last == 's' || last == 'l' || last == 'f' || last == 'd') {
            suffix = last;
            nlen = len - 1;
        }
    }

    char num[128];
    if (nlen == 0 || nlen >= sizeof(num)) {
        return fail(p, "malformed number");
    }
    memcpy(num, tok, nlen);
    num[nlen] = '\0';

    McmeTag *t = mcme_nbt_new(p->arena, MCME_TAG_Int);
    if (!t) return false;

    char *end = NULL;

    switch (suffix) {
        case 'f': {
            double d = strtod(num, &end);
            if (end == num) { return fail(p, "malformed float"); }
            t->type = MCME_TAG_Float;
            t->as_float = (float)d;
            break;
        }
        case 'd': {
            double d = strtod(num, &end);
            if (end == num) { return fail(p, "malformed double"); }
            t->type = MCME_TAG_Double;
            t->as_double = d;
            break;
        }
        case 'b': {
            long long v = strtoll(num, &end, 10);
            if (end == num) { return fail(p, "malformed byte"); }
            t->type = MCME_TAG_Byte;
            t->as_byte = (int8_t)v;
            break;
        }
        case 's': {
            long long v = strtoll(num, &end, 10);
            if (end == num) { return fail(p, "malformed short"); }
            t->type = MCME_TAG_Short;
            t->as_short = (int16_t)v;
            break;
        }
        case 'l': {
            long long v = strtoll(num, &end, 10);
            if (end == num) { return fail(p, "malformed long"); }
            t->type = MCME_TAG_Long;
            t->as_long = (int64_t)v;
            break;
        }
        default:
            /* No suffix: a decimal point or exponent means double, else int. */
            if (strchr(num, '.') || strchr(num, 'e') || strchr(num, 'E')) {
                double d = strtod(num, &end);
                if (end == num) { return fail(p, "malformed number"); }
                t->type = MCME_TAG_Double;
                t->as_double = d;
            } else {
                long long v = strtoll(num, &end, 10);
                if (end == num) { return fail(p, "malformed integer"); }
                t->type = MCME_TAG_Int;
                t->as_int = (int32_t)v;
            }
            break;
    }

    *out = t;
    return true;
}

/* Recognises infinity and NaN in every spelling the reference emits, plus the
   bare forms. Returns true if the token was one of them. */
static bool parse_special_float(SnbtParser *p, const char *tok, McmeTag **out)
{
    size_t len = strlen(tok);
    (void)p;

    /* Forms with an explicit f/d suffix. These must be checked against the
       full token first: "inf" itself ends in 'f', so naive suffix stripping
       would mangle it into "in". */
    if (len >= 4) {
        char last = (char)ascii_lower((unsigned char)tok[len - 1]);
        if (last == 'f' || last == 'd') {
            const char *body = tok;
            size_t blen = len - 1;
            bool is_float = (last == 'f');
            bool neg = false;
            const char *b = body;
            size_t n = blen;
            if (n > 0 && (*b == '-' || *b == '+')) { neg = (*b == '-'); b++; n--; }

            if (ieq_n(b, n, "inf")) {
                McmeTag *t = mcme_nbt_new(p->arena, is_float ? MCME_TAG_Float : MCME_TAG_Double);
                if (!t) return false;
                double v = neg ? -HUGE_VAL : HUGE_VAL;
                if (is_float) { t->as_float = (float)v; } else { t->as_double = v; }
                *out = t;
                return true;
            }
            if (ieq_n(b, n, "nan")) {
                McmeTag *t = mcme_nbt_new(p->arena, is_float ? MCME_TAG_Float : MCME_TAG_Double);
                if (!t) return false;
                double v = NAN;
                if (is_float) { t->as_float = (float)v; } else { t->as_double = v; }
                *out = t;
                return true;
            }
        }
    }

    /* Bare inf / nan -> double. */
    {
        const char *b = tok;
        size_t n = len;
        bool neg = false;
        if (n > 0 && (*b == '-' || *b == '+')) { neg = (*b == '-'); b++; n--; }

        if (ieq_n(b, n, "inf")) {
            McmeTag *t = mcme_nbt_new(p->arena, MCME_TAG_Double);
            if (!t) return false;
            t->as_double = neg ? -HUGE_VAL : HUGE_VAL;
            *out = t;
            return true;
        }
        if (ieq_n(b, n, "nan")) {
            McmeTag *t = mcme_nbt_new(p->arena, MCME_TAG_Double);
            if (!t) return false;
            t->as_double = NAN;
            *out = t;
            return true;
        }
    }

    return false;
}

/* ------------------------------------------------------------------
 * Token scanning and dispatch
 * ------------------------------------------------------------------ */

/* Scans a bare (unquoted) run of characters up to a delimiter. */
static bool scan_token(SnbtParser *p, char *buf, size_t cap)
{
    size_t start = p->pos;
    while (!at_end(p)) {
        char c = peek(p);
        if (c == ',' || c == ']' || c == '}' || c == ':' ||
            c == ' ' || c == '\t' || c == '\n' || c == '\r') break;
        p->pos++;
    }
    size_t n = p->pos - start;
    if (n == 0 || n >= cap) {
        return fail(p, "empty or oversized token");
    }
    memcpy(buf, p->text + start, n);
    buf[n] = '\0';
    return true;
}

static bool token_is_number_like(const char *s)
{
    char c = s[0];
    return (c >= '0' && c <= '9') || c == '+' || c == '-' || c == '.';
}

static bool parse_token(SnbtParser *p, const char *tok, McmeTag **out)
{
    size_t len = strlen(tok);

    if (ieq_n(tok, len, "true") || ieq_n(tok, len, "false")) {
        McmeTag *t = mcme_nbt_new(p->arena, MCME_TAG_Byte);
        if (!t) return false;
        t->as_byte = ieq_n(tok, len, "true") ? 1 : 0;
        *out = t;
        return true;
    }

    if (parse_special_float(p, tok, out)) return true;

    if (token_is_number_like(tok)) {
        return parse_number_token(p, tok, out);
    }

    /* Unquoted bareword: a string. */
    McmeTag *t = mcme_nbt_new(p->arena, MCME_TAG_String);
    if (!t) return false;
    if (!mcme_nbt_set_string(p->arena, t, tok, len)) return false;
    *out = t;
    return true;
}

/* ------------------------------------------------------------------
 * Compounds
 * ------------------------------------------------------------------ */

static bool parse_compound(SnbtParser *p, McmeTag **out)
{
    p->pos++;   /* consume '{' */

    /* Compounds are staged, exactly like the binary parser: their length is
       not known until '}', and an arena cannot realloc in place. */
    McmeCompoundEntry *entries = NULL;

    skip_ws(p);
    if (!at_end(p) && peek(p) == '}') {
        p->pos++;
        McmeTag *t = mcme_nbt_new(p->arena, MCME_TAG_Compound);
        bool ok = t && mcme_nbt_compound_seal(p->arena, t, entries, 0);
        arrfree(entries);
        if (!ok) return false;
        *out = t;
        return true;
    }

    for (;;) {
        skip_ws(p);
        if (at_end(p)) { arrfree(entries); return fail(p, "unterminated compound"); }

        McmeStr key;
        char c = peek(p);
        if (c == '"' || c == '\'') {
            if (!parse_quoted(p, &key)) { arrfree(entries); return false; }
        } else {
            char tok[512];
            if (!scan_token(p, tok, sizeof(tok))) { arrfree(entries); return false; }
            if (!mcme_nbt_str_set(p->arena, &key, tok, strlen(tok))) {
                arrfree(entries);
                return false;
            }
        }

        skip_ws(p);
        if (at_end(p) || peek(p) != ':') {
            arrfree(entries);
            return fail(p, "expected ':' after a compound key");
        }
        p->pos++;   /* consume ':' */

        McmeTag *value = NULL;
        if (!parse_value(p, &value)) { arrfree(entries); return false; }

        arrput(entries, ((McmeCompoundEntry){ .name = key, .value = value }));

        skip_ws(p);
        if (!at_end(p) && peek(p) == ',') { p->pos++; continue; }
        if (!at_end(p) && peek(p) == '}') { p->pos++; break; }
        arrfree(entries);
        return fail(p, "expected ',' or '}' in compound");
    }

    McmeTag *t = mcme_nbt_new(p->arena, MCME_TAG_Compound);
    bool ok = t && mcme_nbt_compound_seal(p->arena, t, entries, (size_t)arrlen(entries));
    arrfree(entries);
    if (!ok) return false;
    *out = t;
    return true;
}

/* ------------------------------------------------------------------
 * Lists and arrays
 * ------------------------------------------------------------------ */

static bool parse_array(SnbtParser *p, char marker, McmeTag **out)
{
    McmeTagType type = (marker == 'B') ? MCME_TAG_Byte_Array
                     : (marker == 'I') ? MCME_TAG_Int_Array
                                       : MCME_TAG_Long_Array;
    McmeTag *t = mcme_nbt_new(p->arena, type);
    if (!t) return false;

    int64_t *vals = NULL;   /* staging; converted to the array's native width */

    skip_ws(p);
    if (!at_end(p) && peek(p) == ']') {
        p->pos++;
        arrfree(vals);
        *out = t;
        return true;
    }

    for (;;) {
        McmeTag *v = NULL;
        if (!parse_value(p, &v)) { arrfree(vals); return false; }
        arrput(vals, mcme_nbt_long_or(v, 0));

        skip_ws(p);
        if (!at_end(p) && peek(p) == ',') { p->pos++; continue; }
        if (!at_end(p) && peek(p) == ']') { p->pos++; break; }
        arrfree(vals);
        return fail(p, "expected ',' or ']' in array");
    }

    size_t n = (size_t)arrlen(vals);
    bool ok = true;

    switch (marker) {
        case 'B': {
            int8_t *d = n ? mcme_arena_alloc(p->arena, n) : NULL;
            if (n && !d) ok = false;
            else for (size_t i = 0; ok && i < n; ++i) d[i] = (int8_t)vals[i];
            t->bytes.data = d;
            t->bytes.len = n;
            break;
        }
        case 'I': {
            int32_t *d = n ? mcme_arena_alloc(p->arena, n * sizeof(int32_t)) : NULL;
            if (n && !d) ok = false;
            else for (size_t i = 0; ok && i < n; ++i) d[i] = (int32_t)vals[i];
            t->ints.data = d;
            t->ints.len = n;
            break;
        }
        default: {
            int64_t *d = n ? mcme_arena_alloc(p->arena, n * sizeof(int64_t)) : NULL;
            if (n && !d) ok = false;
            else for (size_t i = 0; ok && i < n; ++i) d[i] = vals[i];
            t->longs.data = d;
            t->longs.len = n;
            break;
        }
    }

    arrfree(vals);
    if (!ok) return false;
    *out = t;
    return true;
}

static bool parse_list(SnbtParser *p, McmeTag **out)
{
    p->pos++;   /* consume '[' */

    skip_ws(p);

    /* Array form: an element-type marker followed by ';'. */
    if (p->pos + 1 < p->len && p->text[p->pos + 1] == ';') {
        char m = (char)ascii_lower((unsigned char)p->text[p->pos]);
        if (m == 'b' || m == 'i' || m == 'l') {
            p->pos += 2;
            return parse_array(p, (char)toupper((unsigned char)m), out);
        }
    }

    /* Plain list. Elements are staged as pointers, then copied into one
       arena-allocated block once the element type is known. */
    McmeTag **items = NULL;

    skip_ws(p);
    if (!at_end(p) && peek(p) == ']') {
        p->pos++;
        /* The reference writes an empty list with element type Byte. */
        McmeTag *t = mcme_nbt_new(p->arena, MCME_TAG_List);
        bool ok = t && mcme_nbt_list_alloc(p->arena, t, MCME_TAG_Byte, 0);
        arrfree(items);
        if (!ok) return false;
        *out = t;
        return true;
    }

    for (;;) {
        skip_ws(p);
        size_t start = p->pos;   /* where this element begins, for diagnostics */

        McmeTag *v = NULL;
        if (!parse_value(p, &v)) { arrfree(items); return false; }

        /* A list must be HOMOGENEOUS: every element needs the same tag type as
           the first. The reference refuses a mismatch rather than coercing it
           ("Expected type IntTag but got StringTag at 4").
           This is not only a parity detail: binary NBT stores a single element
           type for the whole list, so a heterogeneous list would be written
           under a type that does not describe its contents — one element would
           be written in the wrong width and the result could never be read
           back. Rejecting on read is the only safe option. */
        if (arrlen(items) > 0 && items[0]->type != v->type) {
            char msg[128];
            snprintf(msg, sizeof msg, "expected type %s but got %s",
                     mcme_tag_type_name(items[0]->type),
                     mcme_tag_type_name(v->type));
            arrfree(items);
            return fail_at(p, start, msg);
        }

        arrput(items, v);

        skip_ws(p);
        if (!at_end(p) && peek(p) == ',') { p->pos++; continue; }
        if (!at_end(p) && peek(p) == ']') { p->pos++; break; }
        arrfree(items);
        return fail(p, "expected ',' or ']' in list");
    }

    size_t n = (size_t)arrlen(items);
    McmeTagType elem = n ? items[0]->type : MCME_TAG_Byte;

    McmeTag *t = mcme_nbt_new(p->arena, MCME_TAG_List);
    bool ok = t && mcme_nbt_list_alloc(p->arena, t, elem, n);
    if (ok) {
        for (size_t i = 0; i < n; ++i) t->list.items[i] = *items[i];
    }
    arrfree(items);
    if (!ok) return false;
    *out = t;
    return true;
}

/* ------------------------------------------------------------------
 * Value dispatch
 * ------------------------------------------------------------------ */

static bool parse_value(SnbtParser *p, McmeTag **out)
{
    if (p->depth >= MCME_NBT_MAX_DEPTH) {
        return fail(p, "nesting too deep");
    }
    skip_ws(p);
    if (at_end(p)) return fail(p, "unexpected end of input");

    char c = peek(p);
    p->depth++;
    bool ok;

    if (c == '{') {
        ok = parse_compound(p, out);
    } else if (c == '[') {
        ok = parse_list(p, out);
    } else if (c == '"' || c == '\'') {
        McmeStr s;
        if (!parse_quoted(p, &s)) { p->depth--; return false; }
        McmeTag *t = mcme_nbt_new(p->arena, MCME_TAG_String);
        if (!t) { p->depth--; return false; }
        t->string = s;
        *out = t;
        ok = true;
    } else {
        char tok[1024];
        if (!scan_token(p, tok, sizeof(tok))) { p->depth--; return false; }
        ok = parse_token(p, tok, out);
    }

    p->depth--;
    return ok;
}

bool mcme_nbt_parse_snbt(McmeArena *arena, const char *text, size_t len, McmeTag **out)
{
    if (!arena || (!text && len > 0) || !out) {
        mcme_set_error("snbt: null argument");
        return false;
    }

    SnbtParser p = {
        .text = text,
        .len = len,
        .pos = 0,
        .arena = arena,
        .depth = 0,
    };

    if (!parse_value(&p, out)) return false;

    skip_ws(&p);
    if (!at_end(&p)) {
        return fail(&p, "trailing data after the value");
    }
    return true;
}
