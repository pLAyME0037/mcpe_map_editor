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

The store is read and, through a writable handle, appended to — and flushed.
`include/mcme/leveldb.h` opens the `db/` directory and enumerates every live
key, merging log records, SSTables and the MANIFEST's live set by internal
key. Details of the format and of the four places it diverges from upstream
LevelDB are in `decisions.md#D9`; what follows is the key layout the chunk
layer will build on.

A writable handle also compacts: past 4 MiB of log the next write rewrites the
merged view as one new level-0 SSTable, records the swap in the MANIFEST, and
removes the old tables and logs — the same operation `mcme db compact` runs by
hand. The view is preserved exactly, tombstones included, so compaction never
changes what a lookup answers; it only changes the store's shape. The details
and the crash-ordering argument are in `decisions.md#D11`.

```
x (4 LE)  z (4 LE)  [dimension (4 LE)]  tag (1 byte)  [subchunk index]
```

The dimension field is **omitted entirely when it is 0** (the overworld), so
overworld keys are 9 bytes and other dimensions are 13. Chunk markers are
`0x2c` (pre-1.18 subchunk), `0x2f` (1.18+ subchunk) and `0x76` (chunk version);
`0x2d` (biome/heightmap, 768 bytes) rides along with a chunk but never marks
one. `src/world/bedrock_keys.c` holds the tag set and the key decoder;
`mcme info` and `mcme chunks` count distinct marked coordinates through it
(see [cli.md](cli.md)).

Chunk *payload* values are stored **uncompressed** — the subchunk
bytes go into the LevelDB value verbatim. This was long recorded
here as zlib, or raw LZ4 block from 1.18.2 on, and every
implementation this tree is validated against disproves it:
amulet-core and mcbe-leveldb both hand the value straight to the
subchunk parser with no decompression anywhere in the path, and
amulet-leveldb — built from Minecraft's own LevelDB fork — stores
`put` values byte for byte. The fork's compression lives *inside
the database files*: table blocks are bare DEFLATE, labelled type
4, which is Minecraft's own addition to the format and which
`src/world/leveldb/table.c` reads.

### Subchunk payloads

`include/mcme/bedrock_chunk.h`, `src/world/bedrock_chunk.c`. The value under
a subchunk key **is** a small binary wrapper around one or more
*paletted storage* layers:

```
byte 0        subchunk version: 1, 8 or 9 (0-7 are the legacy pre-palette
              format, refused by name)
byte 1        storage-layer count        (version 1 omits this: it is 1)
byte 2        signed subchunk Y index    (version 9 only, 1.18.2+)
```

then, per storage layer:

```
byte          header = (bits_per_index << 1) | runtime_flag
bits > 0:     ceil(4096 / floor(32/bits)) 32-bit little-endian words, each
              holding floor(32/bits) indices packed from the least-
              significant bit upward, padding bits above the last index;
              then a 32-bit little-endian palette length; then that many
              little-endian NBT compounds, concatenated, no framing
bits == 0:    no words AND no length field — a single NBT compound follows
              the header directly and fills the layer
```

Index `i` addresses block `(x, z, y) = (i>>8, (i>>4)&15, i&15)`: XZY order
with Y fastest, unlike Java's YZX. Valid widths are 0,1,2,3,4,5,6,8,16 — the
game's enum jumps 6 → 8 → 16 and never writes the gaps. Header bit 0
distinguishes persistent (disk, NBT) from runtime (network, session-relative)
palettes and must be 0 on disk. A layer count of 0 is the legal all-air
marker and carries no layers.

Three independent sources agree on this shape — minecraft.wiki, amulet-core
1.9.50, and mcbe-leveldb 1.23.0 — including on the one detail the wiki text
can be read against: at bits == 0 there is **no palette length field**.
That is pinned by the `v8-bpb0` fixture, because amulet's *encoder* floors
at one bit per index and can never emit the shape, while both battle-tested
*decoders* read it without a length.

The decoder is strict on purpose, and its refusals are documented in the
header: legacy versions, widths outside the enum, the 127 "copy palette"
header, runtime palettes, empty palettes, truncated words and trailing bytes
all fail with a message naming what was found. Round-trip fidelity — decode
then encode, byte-identical — is the property this codec exists for, and a
decoder that tolerates a tail or guesses a width cannot provide it. An
editor that guesses here writes the guess back into someone's world.

Encoding (`mcme_bedrock_subchunk_encode()`) is the inverse: for every
payload the decoder accepts it reproduces the bytes exactly. That holds
because the encoder writes what the model *records* rather than re-deriving
it — `bits` is the stored width (a two-entry palette stored at width 16
round-trips as width 16), palette NBT is forced little-endian, padding bits
come out zero — and it refuses any model the wrapper cannot carry, before
appending a byte. The one documented non-inversion is padding: the model
carries block indices, not the unused high bits of each word, so garbage
there would come back zero; every writer seen here stores zeros anyway.

### Writing one block

`include/mcme/bedrock_write.h`, `src/world/bedrock_write.c`, plus `mcme_bedrock_subchunk_set()` in
`src/world/bedrock_chunk.c`. `mcme_bedrock_set_block(world_db, x, y, z,
dimension, entry)` is the first edit the store layer carries: it builds the
subchunk key, opens the store with the exclusive LOCK, decodes the stored
payload, sets the block, re-encodes and writes the value back. The
subchunk must already exist — creating chunks (the version record, the
heightmap, the records a real save also touches) is a larger operation,
and a missing subchunk is refused, not created.

Setting a block is find-or-add on the layer-zero palette: the new entry
is serialised and compared byte for byte against the stored entries, and
an identical block reuses its index. `mcme_bedrock_block_entry()` builds
the entry the way the reference writes one — `name`, `states`, `version`
in that order, with 17694720 ("1, 14, 0, 0"), the version amulet
assigns a block that arrives without one.

The stored width only ever grows, and only when the edit forces it: a
no-op edit leaves every byte of the payload alone, and a layer stored
wider than its palette needs (real worlds contain such layers) stays
that way, because narrowing would rewrite indices the edit did not
touch. The compact bits==0 form keeps its width until a second entry
arrives. Palette entries the indices no longer reference are kept: the
reference prunes them on its own saves, but dropping them here would
rewrite indices this edit did not touch.

The golden fixture `tests/golden/worlds/bedrock-chunks` is a store
seeded by `tools/ref_bedrock_store.py`: eight subchunk payloads — one
per interesting shape — under the keys their coordinates dictate, put
through amulet-leveldb itself. The selftest edits copies of it and checks
the store's answer against the original fixture payloads: a new block
joining a 16-entry palette (width 4 → 5), an index reuse, a no-op coming
back byte-identical, the fluid layer of a waterlogged subchunk untouched,
the compact form growing to width 1, both version-9 wrappers and a
non-zero dimension surviving the edit, and the neighbouring chunk
unchanged, byte for byte.

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
| `tests/golden/worlds/java` | `tools/ref_worlds.py` (amulet-core 1.9.50) | `mcme info`, region reader, `mcme region` |
| `tests/golden/worlds/bedrock` | `tools/ref_worlds.py` | `mcme info` detection |
| `tests/golden/world` | `tools/ref_region.py` (amulet_nbt 2.1.8) | chunk payload parity |
| `tests/golden/compress` | `tools/ref_compress.py` (Python `zlib`, `lz4.block`) | decompression parity |
| `tests/golden/worlds/bedrock-tables` | `tools/ref_leveldb_compact.py` (amulet-leveldb 1.0.7) | LevelDB: tables + log |
| `tests/golden/worlds/bedrock-forge` | `tools/ref_leveldb_table.py` (forged, then read back by the binding) | LevelDB: many small blocks |
| `tests/golden/leveldb` | `tools/ref_leveldb.py` (live and raw-log dumps) | LevelDB: key/value parity |
| `tests/golden/subchunks` | `tools/ref_bedrock_subchunk.py` (amulet 0.10.64 encoder, read back by its decoder) | subchunk payload decode |
| `tests/golden/worlds/bedrock-chunks` | `tools/ref_bedrock_store.py` (the golden payloads, put under their keys) | chunk write, read, stats, height |
| `tests/golden/worlds/bedrock-entities` | `tools/ref_bedrock_entities.py` (amulet_nbt documents, read back by amulet's `_unpack_nbt_list`) | block entity read/write |

The two LevelDB stores written by Minecraft's own library differ in one way
that matters. `bedrock` is a world whose contents never left the memtable, so
it is one log file. `bedrock-tables` is the same world after four thousand keys
and a compaction, so it is tables *and* a log, with tombstones that have to be
applied across the two — the case where ignoring either half silently returns a
key the reference says is gone.

`bedrock-forge` is neither. Minecraft's fork writes ~100 KiB data blocks, so a
real store of any manageable size has exactly one block and one index entry,
and no amount of realistic data exercises the index walk's second iteration.
That fixture is written byte by byte by `tools/ref_leveldb_table.py`: eight
blocks, a restart point every eight entries, and blocks alternating between
stored and bare-DEFLATE. The generator reads its own output back with the
reference library and refuses to dump golden data unless the real library
accepts the forgery.

Its keys are designed for the chunk inventory rather than for realism: nine
chunks across three dimensions (overworld, nether, end), several marker keys
per chunk, negative coordinates in the last slot of a region, and two chunks
written only under tag 0x2f — the subchunk prefix Amulet 0.10.64 does not
know. Amulet counts 7 chunks in it; mcme counts 9. Both numbers are pinned in
the golden dump's header, and `src/world/selftest.c` asserts mcme's against
the store. That pair exists because "the editor sees an empty world" and "the
world is empty" must be distinguishable without reading the format spec.

For each chunk, `src/world/selftest.c` asserts the same thing Phase 2 did for
NBT documents: parse the payload, re-serialise it, and require the bytes to
match the reference's own output exactly — for binary **and** for compact SNBT.
Proving we land on *some* document is not enough; we have to land on *the*
document the reference saw.

The subchunk fixtures are held to the same standard, with one addition: each
case ships the 4096 indices of every layer as raw little-endian uint16 in
disk order, and the test compares **every index**, not a digest, plus every
palette entry's compact SNBT. Each case also round-trips: decode → encode
must reproduce the payload bytes themselves, compared against the `.bin`
fixture, not against a re-parse. The width range is not decoration — index
patterns at small palettes are symmetric under a z/y index transposition, so
only the wide cases catch that class of bug. That is exactly how the
generator's own confusion between amulet's [x][y][z] encoder arrays and the
disk's XZY order was found: bits 1 through 4 passed, bits 5 and up did not,
and the C decoder turned out to be right all along.

Regenerate with:

```sh
venv/bin/python tools/ref_worlds.py    --out tests/golden/worlds
venv/bin/python tools/ref_region.py    --world tests/golden/worlds/java --out tests/golden/world
python3         tools/ref_compress.py  --out tests/golden/compress
venv/bin/python tools/ref_leveldb_compact.py   # bedrock-tables store
venv/bin/python tools/ref_leveldb_table.py \
    --batch-out tests/golden/leveldb/bedrock-forge-batch.txt   # bedrock-forge (forged)
venv/bin/python tools/ref_leveldb.py --batch-out tests/golden/leveldb/bedrock-batch.txt
venv/bin/python tools/ref_bedrock_subchunk.py --out tests/golden/subchunks
venv/bin/python tools/ref_bedrock_store.py \
    --subchunks tests/golden/subchunks \
    --out tests/golden/worlds/bedrock-chunks/db   # chunk store fixture
venv/bin/python tools/ref_bedrock_entities.py \
    --subchunks tests/golden/subchunks \
    --out tests/golden/worlds/bedrock-entities/db # block entity fixture
```

Note `ref_leveldb_table.py`'s `--batch-out` default is empty: without it the
store and its live dump are regenerated but the raw-log dump is not, and a
stale batch file describes tombstones the store no longer has.

Tests skip rather than fail when the golden data is absent, so `mcme selftest`
stays runnable from any directory.
