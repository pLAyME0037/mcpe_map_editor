# World containers

How mcme reads a Minecraft world directory. Phase 3 of the roadmap.

Read this before touching anything under `src/world/`.

---

## What a "world" is

A world is a **directory**, not a file, and which edition it is decides
everything downstream. mcme never guesses from file contents alone when a
directory marker will do.

| | Java | Bedrock |
|---|---|---|
| Marker | `region/` or `session.lock` | `db/` (a LevelDB store) |
| `level.dat` | gzip → big-endian NBT | 8-byte header → [gzip] → little-endian NBT |
| Chunk unit | region file, 32×32 chunks | one LevelDB key per subchunk |
| NBT byte order | big-endian | little-endian |

`mcme_world_probe()` implements exactly this, in this order:

1. `db/` present → **Bedrock**. Unambiguous; Java has nothing like it.
2. Else `region/` or `session.lock` → **Java**.
3. Else fall back to how `level.dat` is framed (see below).
4. Else `MCME_WORLD_UNKNOWN`. **Not an error** — `mcme info` exits 2 so a
   script can tell "not a world" from "could not read it".

### `level.dat` framing

This is the most reliable discriminator when no directory marker exists, and
the most commonly misunderstood part of the two formats.

```
Java     1f 8b ....            gzip straight away, no header
Bedrock  09 00 00 00 ae 00 00 00  <NBT, possibly gzipped>
         ^^^^^^^^^^^ ^^^^^^^^^^^
         storage     payload length
         version     (advisory; often wrong, never trusted)
```

Both are reported through `McmeLevelDatFrame` so a mis-detected world can be
diagnosed: `mcme info --json` prints `level_dat_frame`.

Two details that cost time to discover:

- **The Bedrock length field is not reliable.** Treat the 8-byte header as
  "skip 8 bytes", not "read N bytes".
- **Minecraft gzips the Bedrock payload; amulet's writer does not.** Both are
  accepted. Real worlds hit the gzipped path, so testing only against
  amulet-generated worlds would leave it unexercised.

### Level compound location

Java nests everything under a `Data` child. Bedrock usually does too, but a
freshly generated Bedrock `level.dat` puts `LevelName` and friends at the root
with no wrapper. `src/world/world.c` looks in both, and reports which fields it
found rather than assuming a shape.

---

## Java Anvil regions

`include/mcme/region.h`, `src/world/region.c`.

```
bytes     0 ..  4095   location table, 1024 × 4 bytes:
                       3 bytes sector offset (big-endian, 4 KiB sectors)
                       1 byte  sector count
bytes  4096 ..  8191   timestamps, 1024 × 4 bytes, big-endian, unix seconds
bytes  8192 ..         chunk payloads, each on a sector boundary
```

Chunk `(cx, cz)` lives at slot `(cx & 31) + (cz & 31) * 32`, and belongs to the
file `r.<floor(cx/32)>.<floor(cz/32)>.mca`. The floor division is what makes
chunk `(-1, -1)` land in `r.-1.-1.mca` at slot `(31, 31)`.

A slot whose sector offset is **zero** means "no chunk here". That is how a
region advertises holes, and it is why an all-zero header is a valid empty
region.

### Chunk payload

```
4 bytes  length, big-endian; counts the compression byte plus the data
1 byte   compression: 1 = gzip, 2 = zlib, 3 = stored, 127 = external
N bytes  the compressed NBT
```

`length` includes the compression byte, so the data is `length - 1` bytes.
`length == 0` is therefore never legal and is rejected rather than underflowed.

### Sector counts are not validated

Real-world region files routinely carry stale sector counts. mcme validates the
**offset** — it must point inside the file, and inside by at least 5 bytes so
the chunk header is readable — and ignores the count. Refusing files over a bad
count would reject worlds that every other editor opens fine.

Compression byte 127 (external file) is **not supported**. It reports what it
found and fails, rather than returning bytes that are not NBT.

---

## Bedrock LevelDB

Detection only, so far. `db/` presence is reported by `mcme info`, but the store
is not opened and no chunk inventory is printed — printing a `chunks: 0` for a
world that has chunks would be worse than saying the reader is missing.

The key layout, for when it lands:

```
x (4 LE)  z (4 LE)  [dimension (4 LE)]  tag (1 byte)  [subchunk index]
```

The dimension field is **omitted entirely when it is 0** (the overworld), so
overworld keys are 9 bytes and other dimensions are 13. Chunk tags seen in the
wild so far in the reference fixture: `0x2d` (biome/heightmap, 768 bytes),
`0x36` (chunk version), `0x76`.

Compression inside those payloads is zlib, or raw LZ4 block from 1.18.2 on.
The LZ4 *block* format has no header, which is why `mcme_compression_detectable`
returns false for it and callers must name the codec explicitly.

---

## Safety

Every world file here is untrusted input. The rules the code follows:

- `mcme_fs_read` refuses anything that is not a **regular file**. `fopen`
  succeeds on directories and on Linux `ftell` then reports `LONG_MAX`, which
  reached `malloc` as 2^63 bytes. It also stops us blocking forever on a FIFO.
- Every decompression takes a `max_out` ceiling. Anvil chunks cap at
  `MCME_REGION_CHUNK_MAX` (32 MiB), far above any real chunk.
- Every read is bounds-checked against the file size.
- A corrupt region file is **skipped with a warning**, not fatal: one damaged
  file out of hundreds should not make a world unopenable.

---

## Testing

Golden data, all generated by the reference implementation rather than by us:

| Directory | Produced by | Used by |
|---|---|---|
| `tests/golden/worlds/java` | `tools/ref_worlds.py` (amulet-core 1.9.50) | `mcme info`, region reader |
| `tests/golden/worlds/bedrock` | `tools/ref_worlds.py` | `mcme info` detection |
| `tests/golden/world` | `tools/ref_region.py` (amulet_nbt 2.1.8) | chunk payload parity |
| `tests/golden/compress` | `tools/ref_compress.py` (Python `zlib`, `lz4.block`) | decompression parity |

The chunk coordinates in the fixture are chosen deliberately: `(0,0)`, `(1,0)`,
`(0,1)`, `(31,31)` and `(-1,-1)`. They span two region files and put a chunk in
the last slot of each, so both the sharding rule and the slot arithmetic are
exercised rather than trivially satisfied.

For each chunk, `src/world/selftest.c` asserts the same thing Phase 2 did for
NBT documents: parse the payload, re-serialise it, and require the bytes to
match the reference's own output exactly — for binary **and** for compact SNBT.
Proving we land on *some* document is not enough; we have to land on *the*
document the reference saw.

Regenerate with:

```sh
venv/bin/python tools/ref_worlds.py    --out tests/golden/worlds
venv/bin/python tools/ref_region.py    --world tests/golden/worlds/java --out tests/golden/world
python3         tools/ref_compress.py  --out tests/golden/compress
```

Tests skip rather than fail when the golden data is absent, so `mcme selftest`
stays runnable from any directory.
