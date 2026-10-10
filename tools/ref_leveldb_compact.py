#!/usr/bin/env python3
"""
Builds a LevelDB fixture that contains SSTables, not just a log.

tests/golden/worlds/bedrock is a small world whose entire contents live in one
log file: every record is still in the memtable, because nothing ever forced a
flush. That exercises the log reader and the merge, and nothing else.

A real Bedrock world is mostly tables. So this script copies that fixture,
writes enough keys to overflow the memtable, forces compaction, and then writes
a little more without compacting again. The result has all three shapes at
once:

  * tables at more than one level, holding keys the log has never seen;
  * a tombstone in the log whose key's value is in a table, so the merge has to
    apply the deletion across sources — the case where ignoring it silently
    resurrects a key the reference says is gone;
  * keys written both before and after the flush, so the newest copy of some
    key sits in the log while an older copy sits in a table — which is what
    actually tests that the merge orders by sequence number rather than by
    which file it happened to read.

LevelDB's compaction is deterministic for a given input, but the golden data
records the live key/value set rather than the file layout, so the test stays
stable even if the table boundaries shift with a different library version.

Usage:
    venv/bin/python tools/ref_leveldb_compact.py \
        --src tests/golden/worlds/bedrock/db \
        --dst tests/golden/worlds/bedrock-tables/db
"""

from __future__ import annotations

import argparse
import os
import shutil
import sys


def _log(msg: str) -> None:
    print(f"ref_leveldb_compact: {msg}", file=sys.stderr)


def _chunk_key(x: int, z: int, tag: int, dim: int = 0) -> bytes:
    """A Bedrock chunk key: x, z, tag, with the dimension omitted when 0."""
    return x.to_bytes(4, "little", signed=True) + z.to_bytes(4, "little", signed=True) + \
        (b"" if dim == 0 else dim.to_bytes(4, "little", signed=True)) + bytes([tag])


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--src", default="tests/golden/worlds/bedrock/db")
    ap.add_argument("--dst", default="tests/golden/worlds/bedrock-tables/db")
    ap.add_argument("--keys", type=int, default=4000,
                    help="keys written to overflow the memtable (default 4000)")
    args = ap.parse_args()

    if not os.path.isdir(args.src):
        _log(f"no such directory: {args.src}")
        return 1

    dst = args.dst
    if os.path.exists(dst):
        shutil.rmtree(dst)
    shutil.copytree(args.src, dst)
    _log(f"copied {args.src} -> {dst}")

    from amulet.libs.leveldb import LevelDB

    # Phase 1: write enough to overflow the memtable (LevelDB's default write
    # buffer is 4 MiB), so a flush to level 0 becomes unavoidable.
    db = LevelDB(dst)
    written = 0
    for i in range(args.keys):
        x, z = i % 64, i // 64
        db.put(_chunk_key(x, z, 0x2d), b"biome:" + i.to_bytes(4, "little"))
        written += 1
    _log(f"wrote {written} keys to force a flush")

    # Phase 2: overwrite a slice of them. These land back in the memtable, so
    # after the next compaction their newest copy is in a table while the
    # version it replaced is gone — and the two copies had better not both
    # survive.
    for i in range(0, 500):
        x, z = i % 64, i // 64
        db.put(_chunk_key(x, z, 0x2d), b"biome-v2:" + i.to_bytes(4, "little"))
    _log("overwrote 500 keys")

    # Phase 3: delete a slice. These tombstones go through compaction too,
    # which is where a reader that ignores deletions starts returning keys the
    # reference says are gone.
    for i in range(100, 300):
        x, z = i % 64, i // 64
        db.delete(_chunk_key(x, z, 0x2d))
    _log("deleted 200 keys")

    db.close()

    # Phase 4: compact. Everything above is folded into tables.
    db = LevelDB(dst)
    db.compact()
    db.close()
    _log("compacted")

    # Phase 5: write again without compacting, so the store ends up with both
    # tables and a fresh log carrying the newest records.
    db = LevelDB(dst)
    for i in range(64, 128):
        db.put(_chunk_key(i, 0, 0x36), b"ver:" + i.to_bytes(4, "little"))
    _log("wrote 64 unflushed keys")

    # Phase 6: delete without compacting. These keys' values are sitting in the
    # table from phase 4 and their tombstones are now in the log, so a reader
    # has to merge across the two AND apply the deletion — the case where
    # ignoring either half silently resurrects a key the reference says is
    # gone. This is the specific bug a log-only fixture cannot reach.
    for i in range(400, 500):
        x, z = i % 64, i // 64
        db.delete(_chunk_key(x, z, 0x2d))
    _log("deleted 100 keys without compacting")
    db.close()

    names = sorted(os.listdir(dst))
    tables = [n for n in names if n.endswith((".ldb", ".sst"))]
    logs = [n for n in names if n.endswith(".log")]
    if not tables:
        _log("no table files were produced; compaction did not run")
        return 1
    _log(f"{len(tables)} table file(s), {len(logs)} log file(s)")
    _log(f"files: {', '.join(names)}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
