/*
 * Modified UTF-8 (Java's DataOutputStream.writeUTF / readUTF encoding).
 *
 * This is NOT standard UTF-8. It is what Java's NBT string payloads use, and
 * the differences are exactly the cases that break a naive port:
 *
 *   1. U+0000 is encoded as the two-byte sequence 0xC0 0x80, never as a bare
 *      0x00. A bare NUL would be indistinguishable from a terminator.
 *   2. Characters outside the Basic Multilingual Plane (U+10000 and above) are
 *      encoded as a UTF-16 surrogate PAIR, and each surrogate is written as a
 *      three-byte sequence. So one supplementary character is SIX bytes here
 *      where standard UTF-8 would use four.
 *
 * Everything else matches UTF-8: 1 byte for U+0001..U+007F, 2 bytes for
 * U+0080..U+07FF, 3 bytes for U+0800..U+FFFF.
 *
 * Reference: java.io.DataOutputStream.writeUTF, and the Java "Data Input and
 * Output" spec section on modified UTF-8.
 *
 * Inside the NBT tree a string is held as standard UTF-8 (the same form Python
 * uses, since the reference is Python), and converted to and from modified
 * UTF-8 at the binary boundary: mcme_mutf8_decode() on read, mcme_mutf8_encode()
 * on write. SNBT output then needs no conversion at all, because SNBT is plain
 * UTF-8 text.
 */

#ifndef MCME_MUTF8_H
#define MCME_MUTF8_H

#include "mcme/core.h"

/*
 * Appends the modified-UTF-8 encoding of the UTF-8/WTF-8 input `utf8`
 * (length `utf8_len`) to `out`.
 *
 * Returns false, leaving `out` unchanged, if the input is malformed (bad lead
 * byte, truncated sequence, overlong encoding, or out of range).
 *
 * A lone surrogate is ACCEPTED and encoded as its three-byte sequence, even
 * though strict UTF-8 forbids it. mcme_mutf8_decode() preserves lone surrogates
 * for exactly this reason: a file we could read must be a file we can write
 * back, or an editor silently drops data on save. mcme_mutf8_cp_len() remains
 * strict, because it answers "is this a code point", which a surrogate is not.
 */
bool mcme_mutf8_encode(const char *utf8, size_t utf8_len, McmeBuffer *out);

/*
 * Appends the standard-UTF-8 decoding of the string `mutf8` (length
 * `mutf8_len`) to `out`.
 *
 * The input may be written either way — modified UTF-8, which Java's writer
 * emits, or standard UTF-8, which Bedrock's C++ writer emits for code points
 * above the BMP (an emoji in an item name arrives as a real four-byte
 * sequence). One decoder reads both editions' files; only what neither
 * encoding can spell — overlong forms, continuation bytes standing as leads,
 * code points past U+10FFFF — is rejected. Rejecting the four-byte spelling
 * cost whole records: measured, three records in one real world, 621
 * documents behind them.
 *
 * Valid surrogate pairs are combined into a single code point and emitted as
 * four-byte UTF-8. A LONE surrogate is preserved as a three-byte sequence
 * (WTF-8) rather than replaced with U+FFFD, so that parsing a file and writing
 * it back reproduces the original bytes.
 *
 * Returns false on a truncated or malformed sequence.
 */
bool mcme_mutf8_decode(const char *mutf8, size_t mutf8_len, McmeBuffer *out);

/*
 * Exact byte length of the modified-UTF-8 encoding of `utf8`, so a buffer can
 * be sized before encoding. Returns 0 on invalid UTF-8.
 */
size_t mcme_mutf8_encoded_len(const char *utf8, size_t utf8_len);

/*
 * Exact byte length of the standard-UTF-8 decoding of `mutf8`, so a buffer can
 * be sized before decoding. Returns 0 on malformed input.
 */
size_t mcme_mutf8_decoded_len(const char *mutf8, size_t mutf8_len);

/*
 * Validates that `mutf8` (length `len`) is well-formed modified UTF-8.
 * Used before trusting a length-prefixed string read out of an untrusted file.
 */
bool mcme_mutf8_valid(const char *mutf8, size_t len);

/*
 * Validates that `s` (length `len`) is well-formed standard UTF-8, which is the
 * form NBT strings are held in inside the tree.
 *
 * Lone surrogates (WTF-8) are allowed, matching mcme_mutf8_decode(), so that an
 * odd file still round-trips.
 *
 * This is the exact condition under which mcme_mutf8_encode() succeeds: both
 * are built on the same decoder, so "valid" and "encodable" cannot drift apart.
 * That matters because a string we accept must be a string we can later write
 * into a world — the SNBT parser runs this on every string and key so a bad
 * input is rejected at read time instead of surfacing as a failed save.
 */
bool mcme_utf8_valid(const char *s, size_t len);

/*
 * Length in bytes of the modified-UTF-8 encoding of a single Unicode code
 * point, or 0 if `cp` is not a valid code point.
 */
size_t mcme_mutf8_cp_len(uint32_t cp);

#endif /* MCME_MUTF8_H */
