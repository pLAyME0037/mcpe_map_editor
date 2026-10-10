/*
 * mcme compression layer.
 *
 * Minecraft stores chunk payloads compressed, and the codec depends on which
 * edition and which version wrote them:
 *
 *   Java Anvil      region chunk payload   raw zlib (mostly) or gzip, chosen by
 *                                          the compression byte in the 5-byte
 *                                          chunk header. See wiki/world.md.
 *   Java level.dat  gzip
 *   Bedrock level.dat gzip
 *   Bedrock subchunk zlib, or raw LZ4 block from 1.18.2 on. Very old worlds
 *                  used snappy; not implemented yet, see below.
 *
 * This header is the ONLY place in mcme allowed to know about zlib or lz4.
 * Nothing else may include <zlib.h> or <lz4.h> — see decisions.md#D8 for why
 * that isolation matters (it is what lets us swap in miniz for a static build
 * without touching any caller).
 *
 * Safety contract, because world files are untrusted input:
 *   Every decompress takes a `max_out` ceiling. An attacker who hands us a
 *   100-byte file that expands to 40 GiB must get a clean error, not an OOM
 *   kill. Callers pass the bound their format implies; MCME_DECOMPRESS_MAX
 *   is the ceiling for callers that genuinely have no better idea.
 */

#ifndef MCME_COMPRESS_H
#define MCME_COMPRESS_H

#include "mcme/core.h"

/* Codec. STORE means the bytes are already the payload, with no framing. */
typedef enum {
    MCME_COMPRESS_STORE       = 0,
    MCME_COMPRESS_GZIP        = 1,
    MCME_COMPRESS_ZLIB        = 2,
    MCME_COMPRESS_LZ4         = 3,
    MCME_COMPRESS_UNKNOWN     = 4,
    /* DEFLATE with neither the zlib nor the gzip header, and no checksum at
       the end. Minecraft's Bedrock LevelDB fork writes its table blocks this
       way and labels them with type byte 4 in the block trailer. Like the LZ4
       block format it has nothing to sniff, so a caller must name it. */
    MCME_COMPRESS_RAW_DEFLATE = 5,
} McmeCompression;

/* Fallback ceiling for callers with no format-implied bound. Generous enough
   for any real chunk, small enough to fail before the OOM killer does. */
#define MCME_DECOMPRESS_MAX ((size_t)256u * 1024u * 1024u)

/* Short name for logs: "store", "gzip", "zlib", "lz4", "unknown", "deflate". */
const char *mcme_compression_name(McmeCompression c);

/* Sniff the codec from leading bytes.
   Returns MCME_COMPRESS_STORE if nothing matched, which is deliberately
   distinct from UNKNOWN: callers that accept uncompressed payloads can treat
   STORE as success, while UNKNOWN always means "refuse". */
McmeCompression mcme_compression_detect(const void *data, size_t len);

/* Does `c` frame its payload in a header we can sniff? gzip and zlib do; the
   LZ4 *block* format Bedrock uses does not (it is headerless), and neither
   does bare DEFLATE, so those can never be auto-detected and must be named
   explicitly by the caller. */
bool mcme_compression_detectable(McmeCompression c);

/*
 * Decompress `src` into `out`, appending.
 * Returns false and sets mcme_last_error() on any failure, including a result
 * larger than `max_out`. `out` is left with whatever was appended before the
 * failure, so a caller that wants transactional behaviour should decompress
 * into a scratch buffer.
 *
 * A zero-length input with MCME_COMPRESS_STORE succeeds and appends nothing.
 * Every other codec rejects a zero-length input, because none of them can
 * legitimately produce a payload from nothing.
 */
bool mcme_decompress(McmeCompression c,
                     const void *src, size_t src_len,
                     McmeBuffer *out, size_t max_out);

/* mcme_decompress with MCME_DECOMPRESS_MAX. */
bool mcme_decompress_default(McmeCompression c,
                             const void *src, size_t src_len,
                             McmeBuffer *out);

/* Detect the codec from the bytes, then decompress. STORE is returned as an
   error here: "I do not recognise this" is not the same as "it is stored", and
   silently copying attacker bytes through would defeat the point of sniffing. */
bool mcme_decompress_auto(const void *src, size_t src_len,
                          McmeBuffer *out, size_t max_out);

/*
 * Compress `src` into `out`, appending. The inverse of mcme_decompress, so
 * round-tripping is mcme_compress(c) then mcme_decompress(c).
 *
 * LZ4 produces the raw block format, matching what Bedrock stores.
 */
bool mcme_compress(McmeCompression c,
                   const void *src, size_t src_len,
                   McmeBuffer *out);

/* Upper bound on the compressed size of `src_len` bytes with codec `c`.
   Used to size the output buffer in one allocation. Never under-reports. */
size_t mcme_compress_bound(McmeCompression c, size_t src_len);

#endif /* MCME_COMPRESS_H */
