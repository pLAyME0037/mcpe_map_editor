#!/usr/bin/env python3
"""
Writes the Bedrock chunk-store fixture: real subchunk
payloads under real keys, in a real LevelDB store.

The subchunk codec is proven against payload files; the
chunk *write* path needs a store that already holds
subchunks, the way a world does. This tool seeds one:
each golden payload (tools/ref_bedrock_subchunk.py's
output, produced by amulet's own encoder) is put under
the key its chunk and subchunk coordinates dictate,
through amulet-leveldb — Minecraft's own LevelDB fork —
so the store on disk is one the game itself could have
written.

Values go in verbatim. Store values are uncompressed
(see wiki/world.md): the fork's compression lives inside
the database files, not the values, so what the game
reads back is exactly what was put.

The layout is chosen so the selftest's edits cover the
cases that matter: a wide palette (bits16), a
waterlogged subchunk (two layers), the compact bits==0
form, both version-9 index wrappers, a legacy version-1
wrapper (present but not edited — a new-style entry
joining a legacy palette is a format-mixing edit the
write path does not need to bless), a second chunk (an
edit must not leak into a neighbour), and a nether key
(the dimension field's written form).

The store is recreated from scratch on every run:
amulet-leveldb's LevelDB() constructor compacts on open,
so an existing fixture would be rewritten rather than
rebuilt, and the fixture would stop matching its
generator.

Usage:
    venv/bin/python tools/ref_bedrock_store.py \
        --subchunks tests/golden/subchunks \
        --out tests/golden/worlds/bedrock-chunks/db
"""

from __future__ import annotations

import argparse
import os
import shutil
import struct
import sys

# (case, chunk x, chunk z, dimension, subchunk Y). The
# coordinates are the store's addressing, deliberately
# spread: negative Y, both version-9 wrappers, a second
# chunk, and a non-zero dimension.
CASES = [
    ("v8-bits4", 0, 0, 0, 0),
    ("v8-waterlogged", 0, 0, 0, 1),
    ("v8-bpb0", 0, 0, 0, -1),
    ("v9-cy4", 0, 0, 0, 4),
    ("v9-cyneg4", 0, 0, 0, -4),
    ("v8-bits16", 1, -2, 0, 0),
    ("v8-bits1", 0, 0, 1, 0),
    ("v1-legacy", 0, 0, 0, 2),
]


def _key(x: int, z: int, dimension: int, cy: int) -> bytes:
    """A subchunk key, in the layout the game writes:
    x, z, [dimension — omitted when zero], 0x2f, Y."""
    k = struct.pack("<ii", x, z)
    if dimension != 0:
        k += struct.pack("<i", dimension)
    k += bytes([0x2F, cy & 0xFF])
    return k


def main() -> int:
    ap = argparse.ArgumentParser(
        description="Seed a Bedrock chunk-store fixture"
    )
    ap.add_argument(
        "--subchunks",
        required=True,
        help="dir of golden subchunk payloads (.bin)",
    )
    ap.add_argument("--out", required=True, help="db/ dir to write")
    args = ap.parse_args()

    if os.path.isdir(args.out):
        shutil.rmtree(args.out)
    os.makedirs(args.out)

    from amulet.libs.leveldb import LevelDB

    db = LevelDB(args.out)
    for case, x, z, dimension, cy in CASES:
        with open(os.path.join(args.subchunks, case + ".bin"), "rb") as f:
            payload = f.read()
        key = _key(x, z, dimension, cy)
        db.put(key, payload)
        got = db.get(key)
        if got != payload:
            print(
                f"FAIL {case}: the store did not round-trip "
                "the payload",
                file=sys.stderr,
            )
            return 1
    db.close()

    print(f"wrote {len(CASES)} subchunk records to {args.out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
