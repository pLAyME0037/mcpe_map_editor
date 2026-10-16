# Porting parity

This page tracks the differences between mcme and the original Amulet Map Editor
when reading/writing Minecraft world formats. It is linked from
[README.md](README.md) and [cli.md](cli.md).

## Java edition

- Region file format: mcme reads `.mca` files with Anvil layout (location table,
  timestamps, chunk payloads). Original uses same format.
- `level.dat` framing: mcme accepts gzip-compressed NBT without explicit header.
  Original frames with `1f 8b` magic bytes.
- Chunk serialization: mcme uses paletted storage with width enum {0,1,2,3,4,5,6,8,16}.
  Original uses similar but may differ in padding and endianness.

## Bedrock edition

- LevelDB store: mcme implements a C11-compatible LevelDB reader/writer with log
  append + compaction. Original uses LevelDB with snappy compression.
- Chunk keys: mcme uses `x(4LE) z(4LE) [dim(4LE)] tag(1) [subchunkIdx]`. Original
  uses similar key layout but may differ in dimension encoding.
- Subchunk payloads: mcme supports versions 1, 8, 9 with bits-per-index encoding.
  Original uses same palette-based storage but may differ in header flags.

## Format divergences (recorded in decisions.md)

- D8: System zlib instead of vendored miniz for gzip/zlib decompression
- D9: Pure-C LevelDB implementation behind `mc_leveldb.h`
- D10: Log-append only writes, no rotation; compaction at 4 MiB
- D11: Full SSTable compaction, one level-0 table

## Known gaps

- Gzip handling in NBT parser (Phase 3)
- Region/chunk NBT layer not yet built
- JSON output type flattening
- `.lang` list hardcoded in main.c
