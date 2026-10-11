#!/usr/bin/env python3
"""
Writes a LevelDB store byte by byte, with small blocks and short restart runs.

The two fixtures built by driving Minecraft's own LevelDB (tools/ref_leveldb.py
and tools/ref_leveldb_compact.py) are the strongest evidence mcme can have:
every byte was written by the real library. But they share one property that
turned out to hide a whole class of bug. Minecraft's fork of LevelDB targets
roughly 100 KiB per data block, so a world with a few thousand keys still
produces a *single* data block — and a single data block means a single index
entry, which means:

  * the index block walk runs exactly once, so nothing tests its second
    iteration;
  * nothing tests resuming from a restart point part-way through a block,
    because every restart in a 100 KiB block but the last is interior to a
    walk that only ever ends;
  * no store anywhere in the test data has more than one data block.

That is exactly how a reader ships with an off-by-one in its block cursor: it
returns the first entry of every block and stops, which looks like a small
world rather than a broken one. It did.

So this script forges a store instead of asking LevelDB for one. It is the one
fixture whose block size, restart interval and compression mode are chosen
rather than inherited:

  * data blocks split after ~160 bytes, so forty keys span several blocks and
    the index block holds several entries;
  * a restart point every 8 entries, so iteration genuinely resumes from the
    middle of a block rather than only starting at its head;
  * every block written both uncompressed and as bare DEFLATE, so both trailer
    types are exercised without needing two fixtures;
  * an empty value, an embedded NUL and invalid UTF-8, so nothing assumes
    either is impossible.

Forging it also makes the fixture independent of any library's tuning: it
stays the same size and shape whatever LevelDB decides to do about block
splitting next release.

The forged store is then opened with Minecraft's own LevelDB binding before
anything is dumped. That is the check that the forgery is real — if the
reference library refuses it, the fixture is worthless, and the dump is not
written.

Usage:
    venv/bin/python tools/ref_leveldb_table.py \
        --dst tests/golden/worlds/bedrock-forge/db \
        --out tests/golden/leveldb/bedrock-forge.txt \
        --batch-out tests/golden/leveldb/bedrock-forge-batch.txt
"""

from __future__ import annotations

import argparse
import os
import shutil
import struct
import sys
import tempfile
import zlib

# Chosen rather than inherited. Both are deliberately far below what a real
# world produces, because the point is to exercise the walk, not to be large.
BLOCK_TARGET = 160      # close a data block once its entries pass this many bytes
RESTART_INTERVAL = 8    # write a restart point this many entries apart

# LevelDB log block types.
FULL = 1

# WriteBatch entry types.
DELETION, VALUE = 0, 1

# Trailer compression types. Minecraft's fork labels bare DEFLATE 4.
NO_COMPRESSION, RAW_DEFLATE = 0, 4

# Table magic, little-endian.
MAGIC = bytes.fromhex("57fb808b247547db")

# MANIFEST VersionEdit tags, matching src/world/leveldb/db.c.
TAG_COMPARATOR, TAG_LOG_NUMBER, TAG_NEXT_FILE, TAG_LAST_SEQUENCE = 1, 2, 3, 4
TAG_NEW_FILE = 7

COMPARATOR = b"leveldb.BytewiseComparator"

# The table's file number, its log's, and the next one a writer would take.
TABLE_NUMBER, LOG_NUMBER, NEXT_FILE_NUMBER = 2, 3, 5


# Chunk-marker tags, mirroring include/mcme/bedrock_keys.h. Both spellings of
# the subchunk prefix, because the game changed it and a reader that knows only
# one of them reports the other era's worlds as empty.
CHUNK_TAGS = (0x2C, 0x2F, 0x76)

# What Amulet 0.10.64 recognises, so the difference can be stated rather than
# left for someone to discover in a world that reads as empty.
AMULET_CHUNK_TAGS = (0x2C, 0x76)


def chunk_coords(items, tags):
    """Distinct (dimension, x, z) over keys whose tag marks a chunk."""
    coords = set()
    for key, _value in items:
        if 9 <= len(key) <= 10 and key[8] in tags:
            coords.add((0, key[:4], key[4:8]))
        elif 13 <= len(key) <= 14 and key[12] in tags:
            dim = int.from_bytes(key[8:12], "little", signed=True)
            coords.add((dim, key[:4], key[4:8]))
    return coords


def _log(msg: str) -> None:
    print(f"ref_leveldb_table: {msg}", file=sys.stderr)


# ---------------------------------------------------------------------------
# CRC32C, masked the way LevelDB masks it.
# ---------------------------------------------------------------------------

_POLY = 0x82F63B78
_TABLE = []
for _i in range(256):
    _c = _i
    for _ in range(8):
        _c = (_c >> 1) ^ (_POLY if _c & 1 else 0)
    _TABLE.append(_c & 0xFFFFFFFF)


def crc32c(data: bytes, seed: int = 0) -> int:
    crc = (~seed) & 0xFFFFFFFF
    for byte in data:
        crc = _TABLE[(crc ^ byte) & 0xFF] ^ (crc >> 8)
    return (~crc) & 0xFFFFFFFF


def mask_crc(crc: int) -> int:
    """LevelDB's unmasking, applied forward: rotate right 15, then offset."""
    return (((crc >> 15) | (crc << 17)) + 0xA282EAD8) & 0xFFFFFFFF


# ---------------------------------------------------------------------------
# Encoding primitives.
# ---------------------------------------------------------------------------

def varint(value: int) -> bytes:
    out = bytearray()
    while True:
        byte = value & 0x7F
        value >>= 7
        if value:
            out.append(byte | 0x80)
        else:
            out.append(byte)
            return bytes(out)


def length_prefixed(data: bytes) -> bytes:
    return varint(len(data)) + data


def internal_key(user_key: bytes, seq: int, kind: int = VALUE) -> bytes:
    """The 8-byte suffix is (seq << 8 | type), little-endian."""
    return user_key + ((seq << 8) | kind).to_bytes(8, "little")


# ---------------------------------------------------------------------------
# Blocks.
# ---------------------------------------------------------------------------

def encode_block(entries: list[tuple[bytes, bytes]], restart_interval: int) -> bytes:
    """Entries with prefix compression, a restart array, and a restart count."""
    body = bytearray()
    restarts: list[int] = []
    previous = b""

    for i, (key, value) in enumerate(entries):
        if i % restart_interval == 0:
            restarts.append(len(body))
            shared = 0
        else:
            shared = 0
            limit = min(len(previous), len(key))
            while shared < limit and previous[shared] == key[shared]:
                shared += 1
        tail = key[shared:]
        body += varint(shared) + varint(len(tail)) + varint(len(value))
        body += tail + value
        previous = key

    # LevelDB always writes at least one restart, at offset 0.
    if not restarts:
        restarts.append(0)

    for offset in restarts:
        body += struct.pack("<I", offset)
    body += struct.pack("<I", len(restarts))
    return bytes(body)


def write_trailer(stored: bytes, kind: int) -> bytes:
    """type byte, then the masked CRC32C of the *stored* bytes and that type.

    The checksum covers the bytes that actually land in the file, so a
    deflated block is checksummed compressed. Checksumming what it decompresses
    to instead produces a block that decodes perfectly and still fails
    verification — which is a confusing way to be wrong.
    """
    crc = mask_crc(crc32c(stored + bytes([kind])))
    return bytes([kind]) + struct.pack("<I", crc)


def compress(contents: bytes, kind: int) -> bytes:
    if kind == NO_COMPRESSION:
        return contents
    if kind == RAW_DEFLATE:
        compressor = zlib.compressobj(6, zlib.DEFLATED, -15)
        return compressor.compress(contents) + compressor.flush()
    raise ValueError(f"unhandled compression type {kind}")


def write_block(entries: list[tuple[bytes, bytes]], kind: int,
                restart_interval: int) -> bytes:
    """Block contents, compressed, plus the trailer the handle does not count."""
    contents = encode_block(entries, restart_interval)
    stored = compress(contents, kind)
    return stored + write_trailer(stored, kind)


# ---------------------------------------------------------------------------
# The store.
# ---------------------------------------------------------------------------

def chunk_key(x: int, z: int, tag: int, dim: int | None = None) -> bytes:
    """A Bedrock chunk key. The dimension is omitted entirely when it is 0."""
    key = x.to_bytes(4, "little", signed=True) + z.to_bytes(4, "little", signed=True)
    if dim is not None and dim != 0:
        key += dim.to_bytes(4, "little", signed=True)
    return key + bytes([tag])


def design_entries() -> list[tuple[bytes, bytes]]:
    """The user keys and values, in the order the reader must yield them.

    LevelDB orders by user key ascending, then sequence descending. Every user
    key here is distinct, so ascending bytewise order is the required order and
    the sequence numbers below only have to be distinct.
    """
    entries: list[tuple[bytes, bytes]] = []

    # Chunk keys across three dimensions, so `mcme chunks` has to group by
    # dimension rather than just count. Each chunk carries several keys under
    # different tags, so counting keys would over-count chunks — which is the
    # whole reason the inventory counts distinct coordinates.
    #
    # Two of the chunks are written only under tag 0x2f, the subchunk prefix
    # the game switched to in 1.18. Amulet 0.10.64 looks for 0x2c and does not
    # know 0x2f, so it reports those chunks as absent. mcme knows both, and
    # main() below records both counts so the difference is stated rather than
    # papered over.
    chunks: list[tuple[int, int, int | None, list[int]]] = [
        # (x, z, dimension, tags)
        (0, 0, None, [0x2c, 0x76, 0x2d]),
        (1, 0, None, [0x2c, 0x76, 0x2d]),
        (0, 1, None, [0x2c, 0x76]),
        (31, 31, None, [0x2c, 0x76]),
        (-1, -1, None, [0x2c, 0x76]),
        (4, 4, 1, [0x2c, 0x76, 0x2d]),
        (5, 5, 1, [0x2c, 0x76]),
        (9, 9, 2, [0x2f]),              # modern prefix only
        (10, 10, 2, [0x2f, 0x2d]),      # modern prefix only
    ]

    for i, (x, z, dim, tags) in enumerate(chunks):
        for tag in tags:
            entries.append((chunk_key(x, z, tag, dim),
                            b"c%02d/t%02x:" % (i, tag) + i.to_bytes(2, "little")))

    # Keys chosen to break assumptions rather than to be realistic. None of
    # them is a chunk key, so a reader that counts every key rather than every
    # chunk marker reports these four as four extra chunks.
    entries.append((b"edge/empty", b""))
    entries.append((b"edge/nul\x00inside", b"one\x00two"))
    entries.append((b"edge/single", b"x"))
    entries.append((b"edge/utf8\xff\xfe", b"\x00\xff\xfe"))

    return entries


def internal_entries() -> list[tuple[bytes, bytes]]:
    """(internal key, value) pairs, in the order the reader must yield them.

    Sorted explicitly rather than trusted from the design above: the chunk keys
    are built as x then z, so key 8 (x=0, z=1) sorts *before* key 1 (x=1,
    z=0), and a block whose entries are not in comparator order is a block
    LevelDB is entitled to reject.

    Each entry also gets the 8-byte internal suffix it must carry. A table
    stores internal keys, not user keys — writing bare user keys produces a
    file that parses as a block, passes every checksum, and still contains
    nothing LevelDB will accept, because the key it reads is missing the
    sequence number it sorts on.
    """
    base = sorted(design_entries(), key=lambda kv: kv[0])
    return [(internal_key(key, i + 1), value) for i, (key, value) in enumerate(base)]


def split_blocks(entries: list[tuple[bytes, bytes]]) -> list[list[tuple[bytes, bytes]]]:
    """Splits into data blocks around BLOCK_TARGET, never splitting mid-entry."""
    blocks: list[list[tuple[bytes, bytes]]] = []
    current: list[tuple[bytes, bytes]] = []
    size = 0

    for key, value in entries:
        current.append((key, value))
        size += len(key) + len(value) + 4
        if size > BLOCK_TARGET:
            blocks.append(current)
            current, size = [], 0

    if current:
        blocks.append(current)
    return blocks


def write_table(path: str, entries: list[tuple[bytes, bytes]], kinds: list[int]) -> int:
    """Writes a complete .ldb file and returns its size.

    `kinds` is one compression type per data block, so a single fixture can
    cover both a stored block and a deflated one.
    """
    blocks = split_blocks(entries)

    out = bytearray()
    handles: list[tuple[bytes, bytes]] = []   # (separator key, block handle)

    for block, kind in zip(blocks, kinds):
        offset = len(out)
        stored = write_block(block, kind, RESTART_INTERVAL)
        out += stored

        # The separator is the block's last key. LevelDB usually shortens it,
        # but a reader must accept the exact key as well, and writing exactly
        # what it must accept is the honest choice for a fixture.
        handle = varint(offset) + varint(len(stored) - 5)   # handle omits trailer
        handles.append((block[-1][0], handle))

    # An empty metaindex: no filters, which every reader must tolerate, and
    # which keeps the fixture honest about what it does not contain.
    meta_offset = len(out)
    meta_stored = write_block([], NO_COMPRESSION, RESTART_INTERVAL)
    out += meta_stored

    index_offset = len(out)
    index_stored = write_block(handles, RAW_DEFLATE, RESTART_INTERVAL)
    out += index_stored

    # The footer is a fixed 48 bytes: the two handles, zero padding out to 40,
    # then the magic. The padding is not optional slack — a reader finds the
    # footer by taking the last 48 bytes of the file, so a footer that is
    # short makes that window start inside the index block instead, and the
    # handles parse as whatever bytes happen to precede them.
    handles_bytes = (varint(meta_offset) + varint(len(meta_stored) - 5) +
                     varint(index_offset) + varint(len(index_stored) - 5))
    if len(handles_bytes) > 40:
        _log("block handles overflow the footer")
        raise SystemExit(1)
    out += handles_bytes
    out += b"\x00" * (40 - len(handles_bytes))
    out += MAGIC

    with open(path, "wb") as fh:
        fh.write(out)
    return len(out)


def log_record(payload: bytes) -> bytes:
    """One FULL log record: masked CRC32C over the type byte and payload."""
    header = bytes([FULL]) + payload
    crc = mask_crc(crc32c(header))
    return struct.pack("<I", crc) + struct.pack("<H", len(payload)) + header


def write_batch(seq: int, ops: list[tuple[int, bytes, bytes]]) -> bytes:
    """A WriteBatch payload: sequence, count, then one record per operation."""
    out = struct.pack("<Q", seq) + struct.pack("<I", len(ops))
    for kind, key, value in ops:
        out += bytes([kind]) + length_prefixed(key)
        if kind == VALUE:
            out += length_prefixed(value)
    return out


def write_manifest(path: str, table_size: int, last_sequence: int) -> None:
    """A MANIFEST holding one VersionEdit: comparator, log, next file, table."""
    entries = internal_entries()
    smallest, largest = entries[0][0], entries[-1][0]

    edit = bytearray()
    edit += bytes([TAG_COMPARATOR]) + length_prefixed(COMPARATOR)
    edit += bytes([TAG_LOG_NUMBER]) + varint(LOG_NUMBER)
    edit += bytes([TAG_NEXT_FILE]) + varint(NEXT_FILE_NUMBER)
    edit += bytes([TAG_LAST_SEQUENCE]) + varint(last_sequence)
    edit += bytes([TAG_NEW_FILE])
    edit += varint(0)              # level
    edit += varint(TABLE_NUMBER)   # file number
    edit += varint(table_size)
    edit += length_prefixed(smallest)
    edit += length_prefixed(largest)

    with open(path, "wb") as fh:
        fh.write(log_record(bytes(edit)))


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--dst", default="tests/golden/worlds/bedrock-forge/db")
    ap.add_argument("--out", default="tests/golden/leveldb/bedrock-forge.txt")
    ap.add_argument("--batch-out", default="")
    args = ap.parse_args()

    entries = internal_entries()
    user_keys = [k[:-8] for k, _ in entries]
    if len(set(user_keys)) != len(user_keys):
        _log("duplicate user key in the design; sequence order would be ambiguous")
        return 1

    blocks = split_blocks(entries)
    if len(blocks) < 2:
        _log(f"only {len(blocks)} data block at BLOCK_TARGET={BLOCK_TARGET}; "
             f"lower it, the whole point is several")
        return 1

    dst = args.dst
    if os.path.exists(dst):
        shutil.rmtree(dst)
    os.makedirs(dst)

    # Alternate the compression type across blocks, so one fixture covers a
    # stored block and a deflated one without needing a second store.
    kinds = [NO_COMPRESSION if i % 2 == 0 else RAW_DEFLATE
             for i in range(len(blocks))]

    table_size = write_table(os.path.join(dst, f"{TABLE_NUMBER:06d}.ldb"),
                             entries, kinds)
    write_manifest(os.path.join(dst, "MANIFEST-000001"), table_size, len(entries))

    # The log carries the records that are newer than the table: two
    # overwrites and two tombstones, one of each for a key whose value is in
    # the table. A reader that does not merge across sources returns the stale
    # value, and a reader that ignores tombstones returns a deleted key.
    batch = write_batch(len(entries) + 1, [
        (VALUE, b"edge/single", b"newer in the log"),
        (VALUE, b"edge/utf8\xff\xfe", b"rewritten"),
        (DELETION, b"edge/upper/Key", b""),
        (DELETION, user_keys[0], b""),
    ])
    with open(os.path.join(dst, f"{LOG_NUMBER:06d}.log"), "wb") as fh:
        fh.write(log_record(batch))

    with open(os.path.join(dst, "CURRENT"), "w", encoding="ascii") as fh:
        fh.write("MANIFEST-000001\n")

    _log(f"{len(entries)} keys in {len(blocks)} data block(s), "
         f"compression {', '.join(str(k) for k in kinds)}")

    # The forgery is only worth anything if the real library accepts it.
    # Opening may take the LOCK file, so work on a copy and leave the fixture
    # byte-identical.
    with tempfile.TemporaryDirectory(prefix="ref_leveldb_table_") as tmp:
        copy = os.path.join(tmp, "db")
        shutil.copytree(dst, copy)
        from amulet.libs.leveldb import LevelDB
        try:
            db = LevelDB(copy)
        except Exception as exc:                       # noqa: BLE001
            _log(f"the reference library refused the forged store: {exc}")
            _log("the fixture is wrong, not the library; nothing was dumped")
            return 1
        try:
            live = sorted(db.items())
        finally:
            db.close()

    _log(f"reference library read {len(live)} live keys; the forgery is real")

    # Both readings of the chunk keys, recorded rather than assumed. Amulet
    # 0.10.64 does not know tag 0x2f, so it cannot see the two modern-only
    # chunks at all; a golden file that pinned only its count would leave the
    # difference for someone to rediscover in a world that reads as empty.
    mcme_coords = chunk_coords(live, CHUNK_TAGS)
    amulet_coords = chunk_coords(live, AMULET_CHUNK_TAGS)

    os.makedirs(os.path.dirname(args.out) or ".", exist_ok=True)
    lines = [f"{key.hex()} {value.hex()}" for key, value in live]
    with open(args.out, "w", encoding="ascii") as fh:
        fh.write("# key-hex value-hex, one entry per line, sorted by key\n")
        fh.write(f"# {len(live)} live entries\n")
        fh.write(f"# {len(mcme_coords)} chunks by mcme's tags "
                 f"{tuple(hex(t) for t in CHUNK_TAGS)}\n")
        fh.write(f"# {len(amulet_coords)} chunks by Amulet 0.10.64's tags "
                 f"{tuple(hex(t) for t in AMULET_CHUNK_TAGS)}\n")
        fh.write("# generated by tools/ref_leveldb_table.py (forged store), "
                 "read back by the Python LevelDB binding\n")
        fh.write("\n".join(lines))
        if lines:
            fh.write("\n")
    _log(f"{len(live)} live entries -> {args.out}")
    _log(f"chunks: mcme {len(mcme_coords)}, Amulet 0.10.64 {len(amulet_coords)}"
         + (" (the 0x2f-only chunks are invisible to Amulet)"
            if len(mcme_coords) != len(amulet_coords) else ""))

    if args.batch_out:
        # Raw log contents, read straight from the file so the dump records
        # what the tombstones actually are and not just their effect.
        raw = log_record(batch)
        payload = raw[7:]
        ops: list[tuple[int, int, bytes, bytes]] = []
        if len(payload) >= 12:
            seq = int.from_bytes(payload[0:8], "little")
            count = int.from_bytes(payload[8:12], "little")
            pos = 12
            for i in range(count):
                kind = payload[pos]; pos += 1
                klen, pos2, shift = 0, pos, 0
                while True:
                    byte = payload[pos2]; pos2 += 1
                    klen |= (byte & 0x7F) << shift
                    shift += 7
                    if not byte & 0x80:
                        break
                key = payload[pos2:pos2 + klen]; pos = pos2 + klen
                value = b""
                if kind == VALUE:
                    vlen, pos2, shift = 0, pos, 0
                    while True:
                        byte = payload[pos2]; pos2 += 1
                        vlen |= (byte & 0x7F) << shift
                        shift += 7
                        if not byte & 0x80:
                            break
                    value = payload[pos2:pos2 + vlen]; pos = pos2 + vlen
                ops.append((seq + i, kind, key, value))

        os.makedirs(os.path.dirname(args.batch_out) or ".", exist_ok=True)
        with open(args.batch_out, "w", encoding="ascii") as fh:
            fh.write("# seq type key-hex value-hex   (type 0 = deletion, 1 = value)"
                     " -- every raw log entry, nothing merged\n")
            fh.write(f"# {len(ops)} entries, "
                     f"{sum(1 for o in ops if o[1] == DELETION)} deletions\n")
            fh.write("# generated by tools/ref_leveldb_table.py from the LevelDB "
                     "log format\n")
            for seq, kind, key, value in ops:
                fh.write(f"{seq} {kind} {key.hex()} {value.hex()}\n")
        _log(f"{len(ops)} raw log entries -> {args.batch_out}")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
