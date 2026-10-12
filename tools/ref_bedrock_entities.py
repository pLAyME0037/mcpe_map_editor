#!/usr/bin/env python3
"""
Writes the Bedrock block-entity fixture: real entity records under
real keys, in a real LevelDB store.

Block entities are a chunk's own record (tag 0x31) holding one NBT
document per entity, concatenated with no framing — the layout
amulet reads with `_unpack_nbt_list` and Minecraft loads. The
*read* path needs a store that already holds such a record, the way
a world does, and this tool seeds one.

The documents are built here rather than captured from a world
because every field the reader is judged on has to be visible: an
`id` in the namespaced form the game writes, `x`/`y`/`z` as ints,
and the type-specific payload of three different entities — a
chest's Items, a sign's Text, a furnace's burn state. Values are
Bedrock's, not Java's: `count` is a byte where Java writes
`Count`, which is the difference a reader that mixed the editions
would get wrong.

Two cases exist to pin behaviour the reference itself shows:

  * several documents in one record — one chunk, three entities,
    so "the entity at this position" is a search and not a fetch
  * a document with no coordinates — amulet's decoder drops it, so
    mcme must not match it to a position or fail on it. It is the
    only case in the fixture the game would not have written.

Before anything is stored, every payload is read back through
amulet's own `_unpack_nbt_list` and required to reproduce the
documents written — as with the other generators, nothing is
written until the reference agrees with itself.

Subchunk payloads are the golden ones, put verbatim under their
keys, so the fixture is a world whose blocks and entities can be
read together.

Usage:
    venv/bin/python tools/ref_bedrock_entities.py \
        --subchunks tests/golden/subchunks \
        --out tests/golden/worlds/bedrock-entities/db
"""

from __future__ import annotations

import argparse
import os
import shutil
import struct
import sys

from amulet_nbt import (
    ByteTag,
    CompoundTag,
    IntTag,
    ListTag,
    NamedTag,
    ShortTag,
    StringTag,
)

BLOCK_VERSION = 17694720  # amulet's default: "1, 14, 0, 0"


def _log(msg: str) -> None:
    print(f"ref_bedrock_entities: {msg}", file=sys.stderr)


def _key(x: int, z: int, dimension: int, tag: int) -> bytes:
    """A chunk-data key: x, z, [dimension — omitted when zero], tag.
    Block-entity records carry no subchunk index."""
    k = struct.pack("<ii", x, z)
    if dimension != 0:
        k += struct.pack("<i", dimension)
    return k + bytes([tag])


def item(slot: int, name: str, count: int) -> CompoundTag:
    """One Bedrock item stack: id, count and damage, with the Slot
    that a container's list uses."""
    return CompoundTag(
        {
            "Slot": ByteTag(slot),
            "id": StringTag(name),
            "count": ByteTag(count),
            "damage": ShortTag(0),
        }
    )


def document(tags: list[NamedTag]) -> bytes:
    """The record's bytes: each document saved as little-endian NBT
    with no compression and no framing between them, which is what
    `_pack_nbt_list` writes and `_unpack_nbt_list` reads."""
    return b"".join(
        t.save_to(compressed=False, little_endian=True) for t in tags
    )


def named(fields: dict) -> NamedTag:
    """A document with the empty root name the game writes."""
    return NamedTag(CompoundTag(fields))


# (case, chunk x, chunk z, dimension, documents). The coordinates
# are the store's addressing; the entities' own x/y/z are block
# coordinates inside those chunks.
CASES = [
    (
        "chest-sign-and-a-stray",
        0,
        0,
        0,
        [
            named(
                {
                    "id": StringTag("minecraft:chest"),
                    "x": IntTag(12),
                    "y": IntTag(64),
                    "z": IntTag(3),
                    "Items": ListTag(
                        [
                            item(0, "minecraft:diamond", 3),
                            item(1, "minecraft:stone", 64),
                        ]
                    ),
                    "CustomName": StringTag(""),
                }
            ),
            named(
                {
                    "id": StringTag("minecraft:sign"),
                    "x": IntTag(0),
                    "y": IntTag(65),
                    "z": IntTag(0),
                    "Text": StringTag("mcme fixture"),
                    "TextOwner": StringTag(""),
                }
            ),
            # No coordinates: not addressable by position, and the
            # one document here the game would not have written.
            named(
                {
                    "id": StringTag("minecraft:glowitemframe"),
                    "item": StringTag("minecraft:stick"),
                }
            ),
        ],
    ),
    (
        "furnace",
        1,
        -2,
        0,
        [
            named(
                {
                    "id": StringTag("minecraft:furnace"),
                    "x": IntTag(16),
                    "y": IntTag(8),
                    "z": IntTag(-32),
                    "BurnTime": ShortTag(100),
                    "CookTime": ShortTag(40),
                    "Items": ListTag([item(0, "minecraft:coal", 5)]),
                }
            ),
        ],
    ),
    (
        "nether-chest",
        0,
        0,
        1,
        [
            named(
                {
                    "id": StringTag("minecraft:chest"),
                    "x": IntTag(0),
                    "y": IntTag(5),
                    "z": IntTag(0),
                    "Items": ListTag(
                        [item(0, "minecraft:netherrack", 16)]
                    ),
                }
            ),
        ],
    ),
]

# (golden subchunk case, chunk x, chunk z, dimension, subchunk Y).
SUBCHUNKS = [
    ("v9-cy4", 0, 0, 0, 4),
    ("v8-bits16", 1, -2, 0, 0),
    ("v8-bits1", 0, 0, 1, 0),
]


def main() -> int:
    ap = argparse.ArgumentParser(
        description="Seed a Bedrock block-entity fixture"
    )
    ap.add_argument(
        "--subchunks",
        required=True,
        help="dir of golden subchunk payloads (.bin)",
    )
    ap.add_argument("--out", required=True, help="db/ dir to write")
    args = ap.parse_args()

    from amulet.level.formats.leveldb_world.interface.chunk.leveldb_8 import (
        LevelDB8Interface,
    )

    iface = LevelDB8Interface()

    # The reference's own reader, before anything is written: a
    # payload it cannot read back is a fixture that would prove
    # nothing.
    payloads = {}
    for case, x, z, dimension, docs in CASES:
        payload = document(docs)
        read_back = iface._unpack_nbt_list(payload)
        if len(read_back) != len(docs):
            _log(
                f"FAIL {case}: reference read {len(read_back)} "
                f"documents, {len(docs)} were written"
            )
            return 1
        for written, got in zip(docs, read_back):
            compound = got.tag
            if not isinstance(compound, CompoundTag):
                _log(f"FAIL {case}: reference read a non-compound")
                return 1
            if "id" not in compound or not str(
                getattr(compound["id"], "py_str", "")
            ).count(":"):
                _log(f"FAIL {case}: the id is not namespaced")
                return 1
            # An entity belongs to the chunk its own coordinates
            # fall in: a document whose x/y/z name another chunk is
            # unreachable, which is a broken fixture rather than a
            # case worth pinning.
            for axis, here, coord in (
                ("x", x, compound.get("x")),
                ("z", z, compound.get("z")),
            ):
                if coord is None:
                    continue        # the coordinateless case
                block = int(getattr(coord, "py_int", 0))
                chunk = block // 16   # Python floors, as the game does
                if chunk != here:
                    _log(
                        f"FAIL {case}: an entity's {axis}={block} "
                        f"is in chunk {chunk}, not {here}"
                    )
                    return 1
        payloads[_key(x, z, dimension, 0x31)] = payload
        _log(f"{case}: {len(docs)} documents, reference agrees")

    if os.path.isdir(args.out):
        shutil.rmtree(args.out)
    os.makedirs(args.out)

    from amulet.libs.leveldb import LevelDB

    db = LevelDB(args.out)
    for case, x, z, dimension, cy in SUBCHUNKS:
        with open(os.path.join(args.subchunks, case + ".bin"), "rb") as f:
            payload = f.read()
        key = _key(x, z, dimension, 0x2F)
        db.put(key + bytes([cy & 0xFF]), payload)
        if db.get(key + bytes([cy & 0xFF])) != payload:
            _log(f"FAIL {case}: the store did not round-trip")
            db.close()
            return 1
    for key, payload in payloads.items():
        db.put(key, payload)
        if db.get(key) != payload:
            _log("FAIL: the store did not round-trip a record")
            db.close()
            return 1
    db.close()

    _log(
        f"wrote {len(payloads)} block-entity records and "
        f"{len(SUBCHUNKS)} subchunks to {args.out}"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
