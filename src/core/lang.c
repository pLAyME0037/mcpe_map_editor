#include "mcme/core.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * .lang parser.
 *
 * Reads the original project's translation files unmodified (assets/lang/).
 * Format, from amulet_map_editor/api/lang.py:
 *
 *     # a comment
 *     some.key=value with spaces
 *     multi.line=first\nsecond
 *
 * - keys match [a-z.]+  (enforced loosely: we accept anything without '=' or
 *   leading '#', so future keys do not silently vanish)
 * - '\n' in a value becomes a real newline
 * - lines are UTF-8; we never assume ASCII when copying bytes
 * - a '#' at the very start of a line is a comment; '#' mid-value is literal
 *
 * Entries are stored in one flat array with owned string storage. A language
 * file has a few hundred keys and is loaded once at startup, so a linear scan
 * on lookup is the right amount of machinery (wiki/simplexity.md rule 5).
 */

typedef struct StringChunk {
    struct StringChunk *next;
    size_t used;
    size_t cap;
    char   data[];
} StringChunk;

typedef struct {
    StringChunk *head;   /* newest first */
} LangStrings;

/* Intern a string, returning a pointer that stays valid until the whole
   LangStrings chain is freed.

   The chunked chain matters: a naive realloc-growing buffer would MOVE, which
   invalidates every pointer already handed out to an entry. That is a
   use-after-free ASan catches on the first real language file (en.lang is
   22 KB, so it reallocs many times) but not on a tiny test fixture that fits
   in one block. Fixed-size chunks never move, so returned pointers stay good. */
#define STRING_CHUNK_SIZE (16u * 1024u)

static char *strings_intern(LangStrings *ls, const char *s, size_t n)
{
    StringChunk *c = ls->head;

    /* An over-long string gets its own dedicated chunk rather than being
       split, so it is still contiguous. */
    if (n + 1 > STRING_CHUNK_SIZE) {
        c = malloc(sizeof(StringChunk) + n + 1);
        if (!c) return NULL;
        c->used = n + 1;
        c->cap = n + 1;
        c->next = ls->head;
        ls->head = c;
        memcpy(c->data, s, n);
        c->data[n] = '\0';
        return c->data;
    }

    if (!c || c->cap - c->used < n + 1) {
        c = malloc(sizeof(StringChunk) + STRING_CHUNK_SIZE);
        if (!c) return NULL;
        c->used = 0;
        c->cap = STRING_CHUNK_SIZE;
        c->next = ls->head;
        ls->head = c;
    }

    char *p = c->data + c->used;
    memcpy(p, s, n);
    p[n] = '\0';
    c->used += n + 1;
    return p;
}

static void strings_free(LangStrings *ls)
{
    StringChunk *c = ls->head;
    while (c) {
        StringChunk *next = c->next;
        free(c);
        c = next;
    }
    ls->head = NULL;
}

/* Decode the two-character escape sequences a .lang value may contain.
   In practice only '\n' appears; the rest are handled so a malformed file
   cannot smuggle a raw control byte into the UI. */
static size_t unescape(const char *src, size_t len, char *out, size_t out_cap)
{
    size_t o = 0;
    for (size_t i = 0; i < len && o + 1 < out_cap; ++i) {
        if (src[i] != '\\' || i + 1 >= len) {
            out[o++] = src[i];
            continue;
        }
        char next = src[++i];
        switch (next) {
            case 'n':  out[o++] = '\n'; break;
            case 't':  out[o++] = '\t'; break;
            case '\\': out[o++] = '\\'; break;
            default:
                /* Unknown escape: keep the backslash and the char, verbatim. */
                if (o + 2 < out_cap) { out[o++] = '\\'; out[o++] = next; }
                break;
        }
    }
    out[o] = '\0';
    return o;
}

bool mcme_lang_load_file(McmeLang *lang, const char *id, const char *path)
{
    if (!lang || !id || !path) {
        mcme_set_error("lang: null argument");
        return false;
    }
    memset(lang, 0, sizeof(*lang));

    char *text = mcme_fs_read_text(path);
    if (!text) return false;

    lang->id = strdup(id);
    if (!lang->id) {
        free(text);
        mcme_set_error("lang: out of memory");
        return false;
    }

    LangStrings strings = { 0 };
    size_t text_len = strlen(text);

    /* First pass: count lines so we can reserve exactly once. */
    size_t line_count = 0;
    for (size_t i = 0; i < text_len; ++i) {
        if (text[i] == '\n') line_count++;
    }
    line_count += 2;

    McmeLangEntry *entries = calloc(line_count, sizeof(McmeLangEntry));
    if (!entries) {
        strings_free(&strings);
        free(lang->id);
        free(text);
        mcme_set_error("lang: out of memory");
        return false;
    }

    size_t entry_count = 0;

    char *cursor = text;
    while (*cursor) {
        /* Find end of line. */
        char *nl = strchr(cursor, '\n');
        size_t line_len = nl ? (size_t)(nl - cursor) : strlen(cursor);

        /* Trim a trailing '\r' so CRLF files parse identically. */
        if (line_len > 0 && cursor[line_len - 1] == '\r') line_len--;

        /* Skip leading whitespace. */
        size_t start = 0;
        while (start < line_len && (cursor[start] == ' ' || cursor[start] == '\t')) start++;

        /* Blank or comment line. */
        if (start >= line_len || cursor[start] == '#') goto next_line;

        {
            char *eq = memchr(cursor + start, '=', line_len - start);
            if (!eq) goto next_line;   /* not a key=value line; ignore silently */

            size_t key_len = (size_t)(eq - (cursor + start));
            /* Trim trailing space from the key. */
            while (key_len > 0 && (cursor[start + key_len - 1] == ' '
                                || cursor[start + key_len - 1] == '\t')) {
                key_len--;
            }
            if (key_len == 0) goto next_line;

            const char *raw_value = eq + 1;
            size_t raw_len = line_len - (size_t)(raw_value - cursor);

            /* Trim whitespace around the value, mirroring the key. This must
               happen BEFORE unescaping, so a trailing "\n" escape is not
               mistaken for trailing whitespace. */
            size_t vstart = 0;
            while (vstart < raw_len && (raw_value[vstart] == ' '
                                     || raw_value[vstart] == '\t')) {
                vstart++;
            }
            size_t vend = raw_len;
            while (vend > vstart && (raw_value[vend - 1] == ' '
                                  || raw_value[vend - 1] == '\t')) {
                vend--;
            }
            raw_value += vstart;
            raw_len = vend - vstart;

            /* Unescape into a scratch buffer, then intern. */
            char scratch[4096];
            size_t value_len = unescape(raw_value, raw_len, scratch, sizeof(scratch));

            char *key = strings_intern(&strings, cursor + start, key_len);
            char *value = strings_intern(&strings, scratch, value_len);
            if (!key || !value) {
                free(entries);
                strings_free(&strings);
                free(lang->id);
                free(text);
                mcme_set_error("lang: out of memory");
                return false;
            }

            entries[entry_count].key = key;
            entries[entry_count].value = value;
            entry_count++;
        }

    next_line:
        if (!nl) break;
        cursor = nl + 1;
    }

    free(text);

    lang->entries = entries;
    lang->entry_count = entry_count;
    /* Take ownership of the string chunk chain. The keys and values point
       into those chunks, so the lang object must keep them alive until
       mcme_lang_free(). Chunks are used instead of one growing buffer
       precisely because a realloc'd buffer would move and dangle every
       pointer already handed to an entry. */
    lang->string_chunks = strings.head;
    return true;
}

const char *mcme_lang_get(const McmeLang *lang, const char *key)
{
    if (!lang || !key) return NULL;
    for (size_t i = 0; i < lang->entry_count; ++i) {
        if (strcmp(lang->entries[i].key, key) == 0) {
            return lang->entries[i].value;
        }
    }
    return NULL;
}

void mcme_lang_free(McmeLang *lang)
{
    if (!lang) return;
    free(lang->id);
    free(lang->entries);
    /* The entries point into these chunks, so release them after. */
    {
        StringChunk *c = (StringChunk *)lang->string_chunks;
        while (c) {
            StringChunk *next = c->next;
            free(c);
            c = next;
        }
    }
    lang->id = NULL;
    lang->entries = NULL;
    lang->string_chunks = NULL;
    lang->entry_count = 0;
}
