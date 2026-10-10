#!/usr/bin/env python3
"""
Dump a LevelDB store's live key/value set as golden data.

`mcme` implements the LevelDB on-disk format itself (decisions.md#D9), so it
needs an independent authority for "what is actually in this store". The
reference is Python's `leveldb` binding — the same library Minecraft's world
format is built on — driving a copy of the fixture.

The dump is the *merged, deduplicated* view: for any key written more than once,
only the newest survives, and a key that was deleted does not appear at all.
That is the contract mcme_leveldb_iterate() promises, so it is what gets pinned.

Values are written as hex rather than raw bytes because Bedrock chunk payloads
contain NULs and non-UTF-8 bytes, and a text fixture that cannot be inspected
with `head` is a fixture nobody will debug with.

`--batch-out` additionally dumps the *raw* log contents: every WriteBatch entry
in every .log file, deletions included and with nothing merged away. The live
dump above proves deletions are applied (their keys are simply absent); this
one proves which keys are the tombstones, so a test can assert that a specific
key reports "deleted" rather than "absent" — the distinction a naive
implementation gets wrong by returning an older copy of the key instead.

Usage:
    venv/bin/python tools/ref_leveldb.py --db tests/golden/worlds/bedrock/db \
        --out tests/golden/leveldb/bedrock.txt \
        --batch-out tests/golden/leveldb/bedrock-batch.txt
"""

from __future__ import annotations

import argparse
import os
import shutil
import struct
import sys
import tempfile

# LevelDB log block types. A logical record spans blocks as FIRST/MIDDLE/LAST.
FULL, FIRST, MIDDLE, LAST = 1, 2, 3, 4

# WriteBatch entry types.
DELETION, VALUE = 0, 1


def _log(msg: str) -> None:
    print(f"ref_leveldb: {msg}", file=sys.stderr)


def _reassemble_log(data: bytes) -> list[bytes]:
    """Splits a LevelDB log file into logical record payloads.

    32 KiB blocks of `crc32c(4) | len(2) | type(1) | payload`. A record
    starting in one block continues in the next as FIRST/MIDDLE/LAST fragments.
    A short final block is a normal end of file, not an error — LevelDB does
    not pad the tail.
    """
    records: list[bytes] = []
    offset = 0
    pending = b""

    while offset + 7 <= len(data):
        _crc, length, rtype = struct.unpack_from("<IHb", data, offset)
        offset += 7
        if length == 0 or offset + length > len(data):
            break  # torn tail from an interrupted write; nothing more to read
        payload = data[offset : offset + length]
        offset += length

        if rtype == FULL:
            if pending:
                _log("FULL record while a fragmented one was open")
            records.append(payload)
            pending = b""
        elif rtype == FIRST:
            if pending:
                _log("FIRST record while a fragmented one was open")
            pending = payload
        elif rtype == MIDDLE:
            pending += payload
        elif rtype == LAST:
            records.append(pending + payload)
            pending = b""

    if pending:
        _log("trailing fragmented record with no LAST fragment")
    return records


def _batches(records: list[bytes]) -> list[tuple[int, int, bytes, bytes]]:
    """Expands WriteBatch records into (sequence, type, key, value) tuples."""
    out: list[tuple[int, int, bytes, bytes]] = []
    for rec in records:
        if len(rec) < 12:
            _log("record shorter than a WriteBatch header")
            continue
        seq = int.from_bytes(rec[0:8], "little")
        count = int.from_bytes(rec[8:12], "little")
        pos = 12
        for i in range(count):
            if pos >= len(rec):
                _log("batch count overruns its record")
                break
            etype = rec[pos]
            pos += 1
            klen, used = 0, 0
            while True:
                if pos + used >= len(rec):
                    _log("unterminated key length")
                    return out
                b = rec[pos + used]
                klen |= (b & 0x7F) << (7 * used)
                used += 1
                if not b & 0x80:
                    break
            pos += used
            key = rec[pos : pos + klen]
            pos += klen
            value = b""
            if etype == VALUE:
                vlen, used = 0, 0
                while True:
                    if pos + used >= len(rec):
                        _log("unterminated value length")
                        return out
                    b = rec[pos + used]
                    vlen |= (b & 0x7F) << (7 * used)
                    used += 1
                    if not b & 0x80:
                        break
                pos += used
                value = rec[pos : pos + vlen]
                pos += vlen
            out.append((seq + i, etype, key, value))
    return out


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--db", default="tests/golden/worlds/bedrock/db")
    ap.add_argument("--out", default="tests/golden/leveldb/bedrock.txt")
    ap.add_argument("--batch-out", default="")
    args = ap.parse_args()

    if not os.path.isdir(args.db):
        _log(f"no such directory: {args.db}")
        return 1

    # Opening a LevelDB for reading may still touch the directory (it takes the
    # LOCK file), so work on a copy and leave the fixture byte-identical. A
    # fixture that mutates when read is a fixture that stops matching its
    # committed checksums.
    with tempfile.TemporaryDirectory(prefix="ref_leveldb_") as tmp:
        copy = os.path.join(tmp, "db")
        shutil.copytree(args.db, copy)

        from amulet.libs.leveldb import LevelDB

        db = LevelDB(copy)
        try:
            items = sorted(db.items())
        finally:
            db.close()

    os.makedirs(os.path.dirname(args.out) or ".", exist_ok=True)

    lines = []
    for key, value in items:
        lines.append(f"{key.hex()} {value.hex()}")

    header = (
        "# key-hex value-hex, one entry per line, sorted by key\n"
        f"# {len(items)} live entries\n"
        "# generated by tools/ref_leveldb.py from the Python leveldb binding\n"
    )
    with open(args.out, "w", encoding="ascii") as fh:
        fh.write(header)
        fh.write("\n".join(lines))
        if lines:
            fh.write("\n")

    total = sum(len(v) for _, v in items)
    _log(f"{len(items)} keys, {total} value bytes -> {args.out}")

    if args.batch_out:
        # Read the logs directly. Unlike opening the store, reading a file
        # takes no LOCK and cannot disturb the fixture.
        entries: list[tuple[int, int, bytes, bytes]] = []
        logs = sorted(
            n for n in os.listdir(args.db) if n.endswith(".log")
        )
        for name in logs:
            with open(os.path.join(args.db, name), "rb") as fh:
                entries.extend(_batches(_reassemble_log(fh.read())))
        entries.sort(key=lambda e: (e[2], -e[0]))

        lines = [
            "# seq type key-hex value-hex   (type 0 = deletion, 1 = value)"
            " -- every raw log entry, nothing merged",
            f"# {len(entries)} entries, "
            f"{sum(1 for e in entries if e[1] == DELETION)} deletions",
            "# generated by tools/ref_leveldb.py from the LevelDB log format",
        ]
        for seq, etype, key, value in entries:
            lines.append(f"{seq} {etype} {key.hex()} {value.hex()}")

        os.makedirs(os.path.dirname(args.batch_out) or ".", exist_ok=True)
        with open(args.batch_out, "w", encoding="ascii") as fh:
            fh.write("\n".join(lines) + "\n")
        _log(f"{len(entries)} raw entries -> {args.batch_out}")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
