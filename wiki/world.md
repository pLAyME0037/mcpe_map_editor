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

The store is read. `include/mcme/leveldb.h` opens the `db/` directory read-only
and enumerates every live key, merging log records, SSTables and the MANIFEST's
live set by internal key. Details of the format and of the four places it
diverges from upstream LevelDB are in `decisions.md#D9`; what follows is the
key layout the chunk layer will build on.

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

Compression inside chunk *payloads* (as opposed to inside table blocks) is
zlib, or raw LZ4 block from 1.18.2 on. The LZ4 *block* format has no header,
which is why `mcme_compression_detectable` returns false for it and callers
must name the codec explicitly. Table blocks are a separate matter: they are
bare DEFLATE, labelled type 4, which is Minecraft's own addition to the format.

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
| `tests/golden/worlds/bedrock-tables` | `tools/ref_leveldb_compact.py` (amulet-leveldb 1.0.7) | LevelDB: tables + log |
| `tests/golden/worlds/bedrock-forge` | `tools/ref_leveldb_table.py` (forged, then read back by the binding) | LevelDB: many small blocks |
| `tests/golden/leveldb` | `tools/ref_leveldb.py` (live and raw-log dumps) | LevelDB: key/value parity |

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

Regenerate with:

```sh
venv/bin/python tools/ref_worlds.py    --out tests/golden/worlds
venv/bin/python tools/ref_region.py    --world tests/golden/worlds/java --out tests/golden/world
python3         tools/ref_compress.py  --out tests/golden/compress
venv/bin/python tools/ref_leveldb_compact.py   # bedrock-tables store
venv/bin/python tools/ref_leveldb_table.py \
    --batch-out tests/golden/leveldb/bedrock-forge-batch.txt   # bedrock-forge (forged)
venv/bin/python tools/ref_leveldb.py --batch-out tests/golden/leveldb/bedrock-batch.txt
```

Note `ref_leveldb_table.py`'s `--batch-out` default is empty: without it the
store and its live dump are regenerated but the raw-log dump is not, and a
stale batch file describes tombstones the store no longer has.

Tests skip rather than fail when the golden data is absent, so `mcme selftest`
stays runnable from any directory.
