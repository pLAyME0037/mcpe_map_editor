#!/usr/bin/env python3
"""
Cross-check: mcme reads a block entity, amulet reads the same
record back.

The C selftest pins `mcme_bedrock_block_entity` against the
fixture this tree generates. This tool proves it against the
REFERENCE: it copies the golden entity store, asks mcme for the
entity at a block, then reads the same chunk record with
amulet-core 1.9.50's own loader — `_unpack_nbt_list`, the code
path that reads worlds Minecraft loads — and requires the two to
agree on the WHOLE document, field for field:

  * a chest with two item stacks, a sign's text, a furnace's
    burn state, and a chest in another dimension
  * several documents in one record: the read is a search by the
    coordinates each document carries, and the answer is the one
    that matches
  * a document with no coordinates is not matched to a position
  * the record belongs to the chunk the coordinates fall in: the
    same local block one chunk over is not that entity
  * no record at that block is exit 2, a record the game deleted
    is exit 2, and a record that will not decode is exit 3

The comparison is the whole tree rather than a few named fields:
each side is reduced to plain Python (dicts, lists, strings,
numbers) and compared for equality, so a reader that renames a
field, drops one, or reads a byte as something else fails here.

Direction is deliberate: mcme reads, the reference reads — the
write cross-check runs the other way round.

Usage:
    venv/bin/python tools/ref_bedrock_entity.py --mcme ./mcme
"""

from __future__ import annotations

import argparse
import json
import os
import shutil
import struct
import subprocess
import sys
import tempfile

FIXTURE_WORLD = "tests/golden/worlds/bedrock-entities"


def _log(msg: str) -> None:
    print(f"ref_bedrock_entity: {msg}", file=sys.stderr)


def record_key(x: int, z: int, dimension: int = 0) -> bytes:
    k = struct.pack("<ii", x, z)
    if dimension != 0:
        k += struct.pack("<i", dimension)
    return k + bytes([0x31])


def plain(value):
    """Anything amulet's own decode gives, as plain Python: NBT
    tags unwrap through py_data, dicts and lists recurse, byte
    arrays become lists of numbers the way JSON has to hold
    them."""
    py_data = getattr(value, "py_data", None)
    if py_data is not None and not isinstance(value, (dict, list)):
        return plain(py_data)
    if isinstance(value, dict):
        return {k: plain(v) for k, v in value.items()}
    if isinstance(value, (list, tuple)):
        return [plain(v) for v in value]
    if isinstance(value, (bytes, bytearray)):
        return list(value)
    return value


def field(comp, key):
    """A compound's entry, or None: amulet_nbt compounds answer
    `in` and `[]` like a dict."""
    return comp[key] if key in comp else None


def documents(iface, payload: bytes):
    """The record's documents, decoded by the reference. A
    document with no coordinates gets the sentinel far-off value,
    which matches no block and so matches no query."""
    far = -1 << 30
    out = []
    for doc in iface._unpack_nbt_list(payload):
        comp = doc.tag
        out.append(
            {
                "id": str(getattr(field(comp, "id"), "py_str", "")),
                "x": int(getattr(field(comp, "x"), "py_int", far)),
                "y": int(getattr(field(comp, "y"), "py_int", far)),
                "z": int(getattr(field(comp, "z"), "py_int", far)),
                "doc": plain(comp.py_data),
            }
        )
    return out


def main() -> int:
    ap = argparse.ArgumentParser(
        description="mcme reads a block entity, amulet reads it back"
    )
    ap.add_argument("--mcme", required=True, help="path to mcme")
    ap.add_argument(
        "--world",
        default=FIXTURE_WORLD,
        help="golden world whose store is the starting point",
    )
    args = ap.parse_args()

    from amulet.libs.leveldb import LevelDB
    from amulet.level.formats.leveldb_world.interface.chunk.leveldb_8 import (
        LevelDB8Interface,
    )

    iface = LevelDB8Interface()
    checks = 0

    tmp = tempfile.mkdtemp(prefix="mcme_entity_ref_")
    world = os.path.join(tmp, "world")
    shutil.copytree(args.world, world)
    db_path = os.path.join(world, "db")

    def run_entity(x: int, y: int, z: int, dimension: int = 0):
        cmd = [args.mcme, "--json", "chunk", "entity", world,
               str(x), str(y), str(z)]
        if dimension != 0:
            cmd += ["--dimension", str(dimension)]
        r = subprocess.run(cmd, capture_output=True, text=True)
        return r.returncode, r.stdout.strip(), r.stderr.strip()

    def expect_entity(name: str, x: int, y: int, z: int,
                      dimension: int = 0):
        nonlocal checks
        cx, cz = x // 16, z // 16
        db = LevelDB(db_path)
        try:
            try:
                payload = db.get(record_key(cx, cz, dimension))
            except KeyError:
                payload = None
        finally:
            db.close()

        here = []
        if payload is not None:
            here = [
                d
                for d in documents(iface, payload)
                if (d["x"], d["y"], d["z"]) == (x, y, z)
            ]

        rc, out, err = run_entity(x, y, z, dimension)
        if not here:
            # No record for the chunk, or none of its documents at
            # that block: either way there is no entity there.
            if rc != 2:
                raise AssertionError(
                    f"{name}: no document at that block, so exit "
                    f"{rc} not 2 ({err})")
            checks += 1
            _log(f"{name}: nothing at that block, exit 2")
            return
        if rc != 0:
            raise AssertionError(f"{name}: mcme exit {rc}: {err}")
        if len(here) != 1:
            raise AssertionError(
                f"{name}: the reference found {len(here)} "
                "documents at that block")
        if json.loads(out) != here[0]["doc"]:
            raise AssertionError(
                f"{name}: mcme read {out!r}, reference read "
                f"{here[0]['doc']!r}")
        checks += 1
        _log(f"{name}: whole document agrees "
             f"(id {here[0]['id']!r})")

    # --- The chest: the first of three documents in one record.
    expect_entity("chest", 12, 64, 3)
    # --- The sign: the second document of the same record.
    expect_entity("sign", 0, 65, 0)
    # --- The coordinateless document is matched to no position.
    expect_entity("no document at that block", 12, 64, 4)
    # --- A furnace in another chunk, at a negative chunk
    #     coordinate.
    expect_entity("furnace", 16, 8, -32)
    # --- Same local block, one chunk over: not that entity.
    expect_entity("same local block, other chunk", 12, 64, 19)
    # --- Another dimension's own record.
    expect_entity("nether chest", 0, 5, 0, dimension=1)

    # --- A record the game deleted is an absence, not a fault.
    db = LevelDB(db_path)
    try:
        db.delete(record_key(0, 0))
    finally:
        db.close()
    expect_entity("after a deletion", 12, 64, 3)

    # --- A record that will not decode is exit 3.
    db = LevelDB(db_path)
    try:
        db.put(record_key(0, 0), b"\x0a\x00\x03id\x08\x00\x04xx")
    finally:
        db.close()
    rc, out, err = run_entity(12, 64, 3)
    if rc != 3:
        raise AssertionError(
            f"junk record: exit {rc}, want 3 ({err})")
    checks += 1
    _log("junk record: exit 3")

    shutil.rmtree(tmp, ignore_errors=True)
    _log(f"ok — {checks} checks")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except AssertionError as exc:
        _log(f"FAIL: {exc}")
        sys.exit(1)
