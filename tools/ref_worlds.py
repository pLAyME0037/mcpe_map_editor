#!/usr/bin/env python3
"""
Generate reference worlds for the golden world-format tests.

Phase 3 reads world containers (Java Anvil `.mca` regions, Bedrock LevelDB).
Like tools/ref_dump.py, this drives the *installed Amulet* as an oracle rather
than reimplementing anything: we ask amulet-core to write worlds, then check the
resulting files into tests/golden/worlds/ as byte-for-byte fixtures.

Why fixtures rather than "generate at test time":
  - The generator needs the Amulet venv, which a plain `./rebuild.sh test` must
    not require.
  - A committed fixture pins the exact bytes we claim to read correctly. If
    amulet-core changes its writer, our tests must not silently follow.

Usage:
    venv/bin/python tools/ref_worlds.py --out tests/golden/worlds

A bare run (no --out) generates into a temporary directory: the goldens are
never an implicit target, so a battery that sweeps every tools/ref_*.py
cannot replant them by accident (G22 caught exactly that: the generator
ran bare, Amulet re-saved both worlds in place, and the region timestamps
the core selftest pins changed with them).

Output layout:
    <out>/java/     a small Java world (level.dat + region/*.mca + entities/*.mca)
    <out>/bedrock/  a small Bedrock world (level.dat + db/ LevelDB store)

The chunk coordinates deliberately span more than one region file in each axis
(chunks at (0,0), (1,0), (0,1), (31,31) and (-1,-1)), so the region sharding
rule — chunk (cx, cz) belongs to r.<floor(cx/32)>.<floor(cz/32)>.mca — is
exercised rather than trivially satisfied by a single chunk at the origin.
"""

from __future__ import annotations

import argparse
import os
import shutil
import sys
import tempfile

import amulet
from amulet.api.chunk import Chunk

OVERWORLD = "minecraft:overworld"

# Chunks to place. See the module docstring for why these specifically.
CHUNK_COORDS = [(0, 0), (1, 0), (0, 1), (31, 31), (-1, -1)]


def _log(msg: str) -> None:
    print(f"ref_worlds: {msg}", file=sys.stderr)


def _build(fmt_cls, path: str, platform, version, label: str) -> None:
    """Create an empty world at `path`, then reopen it and drop chunks in.

    The create/open and the chunk writing are two separate amulet calls because
    `create_and_open` returns the format wrapper, not a usable level: chunks are
    written through `amulet.load_level`, which wraps the format in a World.
    """
    if os.path.exists(path):
        shutil.rmtree(path)
    os.makedirs(path)

    fmt = fmt_cls(path)
    fmt.create_and_open(platform, version, overwrite=True)
    fmt.close()

    # Bedrock worlds carry the display name in a plain-text levelname.txt, and
    # amulet's own format detection requires it to identify a directory as a
    # world. create_and_open does not write one, so a world it just made cannot
    # be re-opened by amulet.load_level without this. Real Bedrock worlds have
    # the file, so writing one keeps the fixture honest rather than convenient.
    with open(os.path.join(path, "levelname.txt"), "w", encoding="utf-8") as fh:
        fh.write("mcme ref world")

    level = amulet.load_level(path)
    for cx, cz in CHUNK_COORDS:
        level.put_chunk(Chunk(cx, cz), OVERWORLD)
    level.save()
    level.close()

    files = []
    for root, _dirs, names in os.walk(path):
        for name in sorted(names):
            files.append(os.path.relpath(os.path.join(root, name), path))
    _log(f"{label} -> {path} ({len(files)} files)")
    for name in sorted(files):
        _log(f"    {name}")


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--out", default=None,
                    help="directory to plant the worlds in (default: a "
                         "fresh temporary directory — pass "
                         "tests/golden/worlds to replant the goldens)")
    args = ap.parse_args()
    if args.out is None:
        args.out = tempfile.mkdtemp(prefix="ref_worlds_")

    from amulet.level.formats.anvil_world import AnvilFormat
    from amulet.level.formats.leveldb_world import LevelDBFormat

    os.makedirs(args.out, exist_ok=True)
    _build(AnvilFormat, os.path.join(args.out, "java"),
           "java", (1, 20, 0), "java")
    _build(LevelDBFormat, os.path.join(args.out, "bedrock"),
           "bedrock", 100, "bedrock")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
