#include "mcme/mutf8.h"

#include <string.h>

/*
 * Modified UTF-8 codec — with standard UTF-8's spellings accepted
 * beside it, because the two editions write their strings
 * differently and one decoder reads both files:
 *
 *   Java (modified UTF-8):
 *     U+0000            -> C0 80        (never a bare 00)
 *     U+10000 and above -> a UTF-16 surrogate pair, each half
 *                          written as three bytes, SIX total where
 *                          standard UTF-8 would use four.
 *
 *   Bedrock (standard UTF-8): U+10000 and above is a real
 *     four-byte sequence — an emoji in an item's name arrives that
 *     way, and a decoder that rejected it lost the whole record
 *     that held it (measured: three records in one real world,
 *     621 documents behind them).
 *
 * Decode therefore accepts each encoding's own spellings and
 * rejects only what neither allows: overlong forms, continuation
 * bytes standing as leads, code points past U+10FFFF. A record's
 * strings decode or the record is refused — the same
 * all-or-nothing the record read has always had, now applied to
 * text the game actually wrote.
 *
 * On decode, a valid surrogate pair is combined into one code point
 * and emitted as four-byte UTF-8. A LONE surrogate is preserved as
 * a three-byte sequence rather than replaced with U+FFFD, so that
 * reading a file and writing it back reproduces the original bytes
 * even if the file was odd. Code points decoded from a four-byte
 * standard-UTF-8 sequence are written back in modified UTF-8's
 * surrogate spelling when encoded — Java's reader wants that; the
 * record-splice paths keep every document they do not rewrite as
 * the bytes it arrived as.
 */

/* ------------------------------------------------------------------
 * Standard UTF-8 primitives, used on the input side of encode and the
 * output side of decode.
 * ------------------------------------------------------------------ */

/* Decodes one code point from UTF-8 input. Returns the number of bytes
   consumed, or 0 if the input is malformed (truncated, overlong, bad lead
   byte, or an encoded surrogate, which standard UTF-8 must never contain). */
static size_t utf8_decode_one(const uint8_t *p, const uint8_t *end, uint32_t *out_cp)
{
    if (p >= end) return 0;
    uint8_t b0 = p[0];

    if (b0 < 0x80) { *out_cp = b0; return 1; }

    if ((b0 & 0xE0) == 0xC0) {
        if (end - p < 2) return 0;
        uint8_t b1 = p[1];
        if ((b1 & 0xC0) != 0x80) return 0;
        uint32_t cp = ((uint32_t)(b0 & 0x1F) << 6) | (uint32_t)(b1 & 0x3F);
        if (cp < 0x80) return 0;                     /* overlong */
        *out_cp = cp;
        return 2;
    }

    if ((b0 & 0xF0) == 0xE0) {
        if (end - p < 3) return 0;
        uint8_t b1 = p[1], b2 = p[2];
        if ((b1 & 0xC0) != 0x80 || (b2 & 0xC0) != 0x80) return 0;
        uint32_t cp = ((uint32_t)(b0 & 0x0F) << 12)
                    | ((uint32_t)(b1 & 0x3F) << 6)
                    |  (uint32_t)(b2 & 0x3F);
        if (cp < 0x800) return 0;                    /* overlong */
        /* Encoded surrogates are deliberately ACCEPTED here, unlike strict
           UTF-8. mcme_mutf8_decode() preserves a lone surrogate as these exact
           three bytes so that reading a file and writing it back is lossless,
           so the encoder has to be able to read them again. Rejecting them
           here would mean parsing a hostile-but-readable file and then being
           unable to save it — data loss in an editor, which is worse than
           tolerating an odd string. */
        *out_cp = cp;
        return 3;
    }

    if ((b0 & 0xF8) == 0xF0) {
        if (end - p < 4) return 0;
        uint8_t b1 = p[1], b2 = p[2], b3 = p[3];
        if ((b1 & 0xC0) != 0x80 || (b2 & 0xC0) != 0x80 || (b3 & 0xC0) != 0x80) return 0;
        uint32_t cp = ((uint32_t)(b0 & 0x07) << 18)
                    | ((uint32_t)(b1 & 0x3F) << 12)
                    | ((uint32_t)(b2 & 0x3F) << 6)
                    |  (uint32_t)(b3 & 0x3F);
        if (cp < 0x10000) return 0;                  /* overlong */
        if (cp > 0x10FFFF) return 0;                 /* out of range */
        *out_cp = cp;
        return 4;
    }

    return 0;   /* continuation byte or a five-byte-or-longer lead */
}

/* Writes one code point as standard UTF-8. Returns bytes written. `out` must
   have room for 4 bytes. Only called with a validated code point. */
static size_t utf8_encode_one(uint32_t cp, uint8_t *out)
{
    if (cp < 0x80) {
        out[0] = (uint8_t)cp;
        return 1;
    }
    if (cp < 0x800) {
        out[0] = (uint8_t)(0xC0 | (cp >> 6));
        out[1] = (uint8_t)(0x80 | (cp & 0x3F));
        return 2;
    }
    if (cp < 0x10000) {
        out[0] = (uint8_t)(0xE0 | (cp >> 12));
        out[1] = (uint8_t)(0x80 | ((cp >> 6) & 0x3F));
        out[2] = (uint8_t)(0x80 | (cp & 0x3F));
        return 3;
    }
    out[0] = (uint8_t)(0xF0 | (cp >> 18));
    out[1] = (uint8_t)(0x80 | ((cp >> 12) & 0x3F));
    out[2] = (uint8_t)(0x80 | ((cp >> 6) & 0x3F));
    out[3] = (uint8_t)(0x80 | (cp & 0x3F));
    return 4;
}

/* Writes one code point as MODIFIED UTF-8. Returns bytes written. `out` must
   have room for 6 bytes. Only called with a validated code point. */
static size_t mutf8_encode_one(uint32_t cp, uint8_t *out)
{
    if (cp == 0) {
        /* The one case where modified UTF-8 differs for the low range. */
        out[0] = 0xC0;
        out[1] = 0x80;
        return 2;
    }
    if (cp < 0x80) {
        out[0] = (uint8_t)cp;
        return 1;
    }
    if (cp < 0x800) {
        out[0] = (uint8_t)(0xC0 | (cp >> 6));
        out[1] = (uint8_t)(0x80 | (cp & 0x3F));
        return 2;
    }
    if (cp < 0x10000) {
        out[0] = (uint8_t)(0xE0 | (cp >> 12));
        out[1] = (uint8_t)(0x80 | ((cp >> 6) & 0x3F));
        out[2] = (uint8_t)(0x80 | (cp & 0x3F));
        return 3;
    }

    /* Supplementary plane: split into a surrogate pair and write each half as
       a three-byte sequence. Six bytes where UTF-8 would use four. */
    uint32_t v = cp - 0x10000;
    uint32_t hi = 0xD800 + (v >> 10);
    uint32_t lo = 0xDC00 + (v & 0x3FF);

    out[0] = (uint8_t)(0xE0 | (hi >> 12));
    out[1] = (uint8_t)(0x80 | ((hi >> 6) & 0x3F));
    out[2] = (uint8_t)(0x80 | (hi & 0x3F));
    out[3] = (uint8_t)(0xE0 | (lo >> 12));
    out[4] = (uint8_t)(0x80 | ((lo >> 6) & 0x3F));
    out[5] = (uint8_t)(0x80 | (lo & 0x3F));
    return 6;
}

/* Modified UTF-8 length of one code point. */
static size_t mutf8_cp_len(uint32_t cp)
{
    if (cp == 0) return 2;
    if (cp < 0x80) return 1;
    if (cp < 0x800) return 2;
    if (cp < 0x10000) return 3;
    return 6;
}

size_t mcme_mutf8_cp_len(uint32_t cp)
{
    if (cp > 0x10FFFF) return 0;
    if (cp >= 0xD800 && cp <= 0xDFFF) return 0;   /* surrogates are not code points */
    return mutf8_cp_len(cp);
}

/* ------------------------------------------------------------------
 * Public API
 * ------------------------------------------------------------------ */

bool mcme_mutf8_encode(const char *utf8, size_t utf8_len, McmeBuffer *out)
{
    const uint8_t *p = (const uint8_t *)utf8;
    const uint8_t *end = p + utf8_len;
    const uint8_t *start = p;

    while (p < end) {
        uint32_t cp;
        size_t n = utf8_decode_one(p, end, &cp);
        if (n == 0) {
            mcme_set_error("mutf8: invalid UTF-8 at byte %zu",
                           (size_t)(p - start));
            return false;
        }
        p += n;

        uint8_t buf[6];
        if (!mcme_buffer_append(out, buf, mutf8_encode_one(cp, buf))) {
            return false;
        }
    }
    return true;
}

bool mcme_mutf8_decode(const char *mutf8, size_t mutf8_len, McmeBuffer *out)
{
    const uint8_t *p = (const uint8_t *)mutf8;
    const uint8_t *end = p + mutf8_len;
    const uint8_t *start = p;

    while (p < end) {
        uint8_t b0 = p[0];

        /* Plain ASCII, including a bare 0x00. A bare NUL is a non-canonical
           spelling of U+0000 (canonical modified UTF-8 is C0 80) but some
           writers emit it; accept it rather than reject a readable file. */
        if (b0 < 0x80) {
            if (!mcme_buffer_append_u8(out, b0)) return false;
            p++;
            continue;
        }

        if ((b0 & 0xE0) == 0xC0) {
            if (end - p < 2) {
                mcme_set_error("mutf8: truncated two-byte sequence at byte %zu",
                               (size_t)(p - start));
                return false;
            }
            uint8_t b1 = p[1];
            if ((b1 & 0xC0) != 0x80) {
                mcme_set_error("mutf8: bad continuation at byte %zu", (size_t)(p - start));
                return false;
            }
            uint32_t cp = ((uint32_t)(b0 & 0x1F) << 6) | (uint32_t)(b1 & 0x3F);
            /* C0 80 is the canonical U+0000. Any other sub-0x80 result is an
               overlong encoding and is rejected. */
            if (cp < 0x80 && !(b0 == 0xC0 && b1 == 0x80)) {
                mcme_set_error("mutf8: overlong encoding at byte %zu", (size_t)(p - start));
                return false;
            }
            uint8_t buf[4];
            if (!mcme_buffer_append(out, buf, utf8_encode_one(cp, buf))) return false;
            p += 2;
            continue;
        }

        if ((b0 & 0xF0) == 0xE0) {
            if (end - p < 3) {
                mcme_set_error("mutf8: truncated three-byte sequence at byte %zu",
                               (size_t)(p - start));
                return false;
            }
            uint8_t b1 = p[1], b2 = p[2];
            if ((b1 & 0xC0) != 0x80 || (b2 & 0xC0) != 0x80) {
                mcme_set_error("mutf8: bad continuation at byte %zu", (size_t)(p - start));
                return false;
            }
            uint32_t cp = ((uint32_t)(b0 & 0x0F) << 12)
                        | ((uint32_t)(b1 & 0x3F) << 6)
                        |  (uint32_t)(b2 & 0x3F);
            if (cp < 0x800) {
                mcme_set_error("mutf8: overlong encoding at byte %zu", (size_t)(p - start));
                return false;
            }

            /* A high surrogate followed by a low surrogate is one supplementary
               character; combine them. Anything else stands alone. */
            if (cp >= 0xD800 && cp <= 0xDBFF && end - p >= 6) {
                uint8_t n0 = p[3], n1 = p[4], n2 = p[5];
                if ((n0 & 0xF0) == 0xE0 && (n1 & 0xC0) == 0x80 && (n2 & 0xC0) == 0x80) {
                    uint32_t low = ((uint32_t)(n0 & 0x0F) << 12)
                                 | ((uint32_t)(n1 & 0x3F) << 6)
                                 |  (uint32_t)(n2 & 0x3F);
                    if (low >= 0xDC00 && low <= 0xDFFF) {
                        uint32_t combined = 0x10000u
                                          + ((cp - 0xD800u) << 10)
                                          + (low - 0xDC00u);
                        uint8_t buf[4];
                        if (!mcme_buffer_append(out, buf, utf8_encode_one(combined, buf))) {
                            return false;
                        }
                        p += 6;
                        continue;
                    }
                }
            }

            /* Lone surrogate. Emit it as a three-byte sequence (WTF-8) so a
               round trip preserves the original bytes. */
            uint8_t buf[4];
            if (!mcme_buffer_append(out, buf, utf8_encode_one(cp, buf))) return false;
            p += 3;
            continue;
        }

        /* Standard UTF-8's four-byte sequence: the game's spelling
           for a code point above the BMP, an emoji in an item name
           for instance. Accepted beside modified UTF-8's surrogate
           pair — one decoder, both editions' writers. */
        if ((b0 & 0xF8) == 0xF0) {
            if (end - p < 4) {
                mcme_set_error("mutf8: truncated four-byte sequence at byte %zu",
                               (size_t)(p - start));
                return false;
            }
            uint8_t b1 = p[1], b2 = p[2], b3 = p[3];
            if ((b1 & 0xC0) != 0x80 || (b2 & 0xC0) != 0x80
                    || (b3 & 0xC0) != 0x80) {
                mcme_set_error("mutf8: bad continuation at byte %zu",
                               (size_t)(p - start));
                return false;
            }
            uint32_t cp = ((uint32_t)(b0 & 0x07) << 18)
                        | ((uint32_t)(b1 & 0x3F) << 12)
                        | ((uint32_t)(b2 & 0x3F) << 6)
                        |  (uint32_t)(b3 & 0x3F);
            if (cp < 0x10000) {
                mcme_set_error("mutf8: overlong encoding at byte %zu",
                               (size_t)(p - start));
                return false;
            }
            if (cp > 0x10FFFF) {
                mcme_set_error("mutf8: code point past U+10FFFF at byte %zu",
                               (size_t)(p - start));
                return false;
            }
            uint8_t buf[4];
            if (!mcme_buffer_append(out, buf, utf8_encode_one(cp, buf))) {
                return false;
            }
            p += 4;
            continue;
        }

        /* Nor these: neither modified UTF-8 nor standard UTF-8 can
           spell a byte this way. */
        mcme_set_error("mutf8: byte 0x%02X is neither UTF-8 nor modified "
                       "UTF-8 at byte %zu",
                       b0, (size_t)(p - start));
        return false;
    }
    return true;
}

size_t mcme_mutf8_encoded_len(const char *utf8, size_t utf8_len)
{
    const uint8_t *p = (const uint8_t *)utf8;
    const uint8_t *end = p + utf8_len;
    size_t total = 0;

    while (p < end) {
        uint32_t cp;
        size_t n = utf8_decode_one(p, end, &cp);
        if (n == 0) return 0;   /* invalid input */
        p += n;
        total += mutf8_cp_len(cp);
    }
    return total;
}

size_t mcme_mutf8_decoded_len(const char *mutf8, size_t mutf8_len)
{
    const uint8_t *p = (const uint8_t *)mutf8;
    const uint8_t *end = p + mutf8_len;
    size_t total = 0;

    while (p < end) {
        uint8_t b0 = p[0];
        if (b0 < 0x80) {
            total += 1;
            p++;
            continue;
        }
        if ((b0 & 0xE0) == 0xC0) {
            if (end - p < 2) return 0;
            uint8_t b1 = p[1];
            if ((b1 & 0xC0) != 0x80) return 0;
            uint32_t cp = ((uint32_t)(b0 & 0x1F) << 6) | (uint32_t)(b1 & 0x3F);
            if (cp < 0x80 && !(b0 == 0xC0 && b1 == 0x80)) return 0;
            uint8_t scratch[4];
            total += utf8_encode_one(cp, scratch);
            p += 2;
            continue;
        }
        if ((b0 & 0xF0) == 0xE0) {
            if (end - p < 3) return 0;
            uint8_t b1 = p[1], b2 = p[2];
            if ((b1 & 0xC0) != 0x80 || (b2 & 0xC0) != 0x80) return 0;
            uint32_t cp = ((uint32_t)(b0 & 0x0F) << 12)
                        | ((uint32_t)(b1 & 0x3F) << 6)
                        |  (uint32_t)(b2 & 0x3F);
            if (cp < 0x800) return 0;
            if (cp >= 0xD800 && cp <= 0xDBFF && end - p >= 6) {
                uint8_t n0 = p[3], n1 = p[4], n2 = p[5];
                if ((n0 & 0xF0) == 0xE0 && (n1 & 0xC0) == 0x80 && (n2 & 0xC0) == 0x80) {
                    uint32_t low = ((uint32_t)(n0 & 0x0F) << 12)
                                 | ((uint32_t)(n1 & 0x3F) << 6)
                                 |  (uint32_t)(n2 & 0x3F);
                    if (low >= 0xDC00 && low <= 0xDFFF) {
                        total += 4;   /* combined into one four-byte code point */
                        p += 6;
                        continue;
                    }
                }
            }
            total += 3;
            p += 3;
            continue;
        }
        if ((b0 & 0xF8) == 0xF0) {
            /* standard UTF-8's four-byte sequence (see decode) */
            if (end - p < 4) return 0;
            uint8_t b1 = p[1], b2 = p[2], b3 = p[3];
            if ((b1 & 0xC0) != 0x80 || (b2 & 0xC0) != 0x80
                    || (b3 & 0xC0) != 0x80) return 0;
            uint32_t cp = ((uint32_t)(b0 & 0x07) << 18)
                        | ((uint32_t)(b1 & 0x3F) << 12)
                        | ((uint32_t)(b2 & 0x3F) << 6)
                        |  (uint32_t)(b3 & 0x3F);
            if (cp < 0x10000 || cp > 0x10FFFF) return 0;
            total += 4;
            p += 4;
            continue;
        }
        return 0;   /* neither encoding spells a byte this way */
    }
    return total;
}

bool mcme_mutf8_valid(const char *mutf8, size_t len)
{
    const uint8_t *p = (const uint8_t *)mutf8;
    const uint8_t *end = p + len;

    while (p < end) {
        uint8_t b0 = p[0];
        if (b0 < 0x80) { p++; continue; }

        if ((b0 & 0xE0) == 0xC0) {
            if (end - p < 2) return false;
            uint8_t b1 = p[1];
            if ((b1 & 0xC0) != 0x80) return false;
            uint32_t cp = ((uint32_t)(b0 & 0x1F) << 6) | (uint32_t)(b1 & 0x3F);
            if (cp < 0x80 && !(b0 == 0xC0 && b1 == 0x80)) return false;
            p += 2;
            continue;
        }

        if ((b0 & 0xF0) == 0xE0) {
            if (end - p < 3) return false;
            uint8_t b1 = p[1], b2 = p[2];
            if ((b1 & 0xC0) != 0x80 || (b2 & 0xC0) != 0x80) return false;
            uint32_t cp = ((uint32_t)(b0 & 0x0F) << 12)
                        | ((uint32_t)(b1 & 0x3F) << 6)
                        |  (uint32_t)(b2 & 0x3F);
            if (cp < 0x800) return false;
            p += 3;
            continue;
        }

        if ((b0 & 0xF8) == 0xF0) {
            /* standard UTF-8's four-byte sequence (see decode) */
            if (end - p < 4) return false;
            uint8_t b1 = p[1], b2 = p[2], b3 = p[3];
            if ((b1 & 0xC0) != 0x80 || (b2 & 0xC0) != 0x80
                    || (b3 & 0xC0) != 0x80) return false;
            uint32_t cp = ((uint32_t)(b0 & 0x07) << 18)
                        | ((uint32_t)(b1 & 0x3F) << 12)
                        | ((uint32_t)(b2 & 0x3F) << 6)
                        |  (uint32_t)(b3 & 0x3F);
            if (cp < 0x10000 || cp > 0x10FFFF) return false;
            p += 4;
            continue;
        }

        return false;   /* neither encoding spells a byte this way */
    }
    return true;
}

bool mcme_utf8_valid(const char *s, size_t len)
{
    /* Reuse utf8_decode_one() rather than writing a second validator, so this
       and mcme_mutf8_encode() agree on every byte by construction. It already
       rejects truncated, overlong, out-of-range and bad-continuation input, and
       already accepts lone surrogates for the round-trip reason explained
       above. */
    const uint8_t *p = (const uint8_t *)s;
    const uint8_t *end = p + len;

    while (p < end) {
        uint32_t cp;
        size_t n = utf8_decode_one(p, end, &cp);
        if (n == 0) return false;
        p += n;
    }
    return true;
}
