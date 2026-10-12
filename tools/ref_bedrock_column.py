#!/usr/bin/env python3
"""
Cross-check: mcme reports a column's height, amulet scans the
same column back.

The C selftest pins `mcme_bedrock_column_height` against the
fixture payloads it was designed from. This tool proves the
answers against the REFERENCE: it copies the golden store, asks
mcme for a column, then walks that column with amulet-core
1.9.50's own loader — the code path that reads worlds Minecraft
loads — and requires the two to agree on:

  * which subchunks the column holds (every key the store has
    for the chunk, in order)
  * the topmost and bottommost non-air block of layer zero, by
    Y and by block name, treating minecraft:air,
    minecraft:cave_air and minecraft:void_air as empty
  * a column the world never stored is exit 2
  * a subchunk the game deleted leaves the list, and the
    surface drops to the next one down
  * a payload neither reader can decode is exit 3, not air
  * a block `mcme chunk set` places into ground that was never
    saved becomes that column's surface, and amulet finds that
    block where mcme says it is

The states of a surface block are not compared: the claim under
test is how high the ground is and what block it is, and the
spec text mcme prints is the same one `chunk get` prints, which
ref_bedrock_write.py already cross-checks character for
character.

Direction is deliberate: mcme reads, the reference reads — the
write cross-check runs the other way round.

Usage:
    venv/bin/python tools/ref_bedrock_column.py --mcme ./mcme
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

import numpy

FIXTURE_WORLD = "tests/golden/worlds/bedrock-chunks"
AIR = {"minecraft:air", "minecraft:cave_air", "minecraft:void_air"}


def _log(msg: str) -> None:
    print(f"ref_bedrock_column: {msg}", file=sys.stderr)


def chunk_key(x: int, z: int, cy: int, dimension: int = 0) -> bytes:
    k = struct.pack("<ii", x, z)
    if dimension != 0:
        k += struct.pack("<i", dimension)
    return k + bytes([0x2F, cy & 0xFF])


def decode_payload(iface, payload: bytes):
    """Wrapper bytes stripped, then every layer through amulet's
    loader. Returns (version, layers) as [(blocks, palette)]; an
    empty layer list is the all-air marker."""
    v = payload[0]
    if v == 1:
        count, rest = 1, payload[1:]
    elif v == 8:
        count, rest = payload[1], payload[2:]
    elif v == 9:
        count, rest = payload[1], payload[3:]
    else:
        raise AssertionError(f"wrapper version {v}")
    layers = []
    for _ in range(count):
        blocks, palette, rest = iface._load_palette_blocks(rest)
        layers.append((blocks, palette))
    if rest != b"":
        raise AssertionError(f"{len(rest)} trailing bytes")
    return v, layers


def name_of(entry) -> str:
    """A palette entry's block name. Entries are amulet_nbt
    NamedTags wrapping the {name, states, version} compound; the
    tag is read off the modern attribute when it has one."""
    comp = entry.tag if hasattr(entry, "tag") else entry.value
    name = comp["name"]
    return str(getattr(name, "py_data", name))


def column(db, iface, x: int, z: int, dimension: int = 0):
    """The column through block (x, z), as the reference reads it:
    every subchunk the store holds for the chunk, and the lowest
    and highest non-air block of layer zero in that column."""
    cx, cz = x // 16, z // 16
    lx, lz = x & 15, z & 15
    subchunks, floor, surface = [], None, None
    for cy in range(-128, 128):
        try:
            payload = db.get(chunk_key(cx, cz, cy, dimension))
        except KeyError:
            continue
        subchunks.append(cy)
        _version, layers = decode_payload(iface, payload)
        if not layers:
            continue        # the all-air marker: stored, empty
        blocks, palette = layers[0]
        arr = numpy.asarray(blocks).reshape(16, 16, 16)
        for ly in range(16):
            name = name_of(palette[int(arr[lx][ly][lz])])
            if name in AIR:
                continue
            if floor is None:
                floor = (cy * 16 + ly, name)
            surface = (cy * 16 + ly, name)
    return subchunks, floor, surface


def main() -> int:
    ap = argparse.ArgumentParser(
        description="mcme reports a column, amulet reads it back"
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

    tmp = tempfile.mkdtemp(prefix="mcme_column_ref_")
    world = os.path.join(tmp, "world")
    shutil.copytree(args.world, world)
    db_path = os.path.join(world, "db")

    def run_height(x: int, z: int, dimension: int = 0):
        cmd = [args.mcme, "--json", "chunk", "height", world,
               str(x), str(z)]
        if dimension != 0:
            cmd += ["--dimension", str(dimension)]
        r = subprocess.run(cmd, capture_output=True, text=True)
        return r.returncode, r.stdout.strip(), r.stderr.strip()

    def expect_column(name: str, x: int, z: int, dimension: int = 0):
        nonlocal checks
        db = LevelDB(db_path)
        try:
            want_subs, want_floor, want_surface = column(
                db, iface, x, z, dimension)
        finally:
            db.close()

        rc, out, err = run_height(x, z, dimension)
        if rc != 0:
            raise AssertionError(f"{name}: mcme exit {rc}: {err}")
        got = json.loads(out)
        checks += 1

        if got["subchunks"] != want_subs:
            raise AssertionError(
                f"{name}: subchunks {got['subchunks']} != "
                f"{want_subs}")
        checks += 1

        for key, want in (("floor", want_floor),
                          ("surface", want_surface)):
            if want is None:
                if got[key] is not None:
                    raise AssertionError(
                        f"{name}: {key} {got[key]}, want null")
            else:
                y, block = want
                if got[key] is None:
                    raise AssertionError(
                        f"{name}: {key} is null, want y {y} {block}")
                if got[key]["y"] != y:
                    raise AssertionError(
                        f"{name}: {key} y {got[key]['y']} != {y}")
                if not got[key]["block"].startswith(block):
                    raise AssertionError(
                        f"{name}: {key} block "
                        f"{got[key]['block']!r} does not start with "
                        f"{block!r}")
            checks += 1
        _log(f"{name}: {len(want_subs)} subchunks, "
             f"floor={want_floor}, surface={want_surface}")

    # --- The fixture's own column: six subchunks, negative Y, both
    #     wrapper versions and a version-1 payload among them.
    expect_column("overworld (0, 0)", 0, 0)
    # --- A second column of a second chunk, and one local column
    #     of a chunk whose neighbours are far away.
    expect_column("overworld (16, -32)", 16, -32)
    expect_column("overworld (1, 1)", 1, 1)
    # --- Another dimension: the same coordinates, its own keys.
    expect_column("nether (0, 0)", 0, 0, dimension=1)

    # --- Ground the world never stored: nothing to measure.
    rc, out, err = run_height(500, 500)
    if rc != 2:
        raise AssertionError(
            f"unstored column: exit {rc}, want 2 ({err})")
    if out:
        raise AssertionError(
            f"unstored column: stdout {out!r}, want nothing")
    checks += 1
    _log("unstored column: exit 2, silent")

    # --- A subchunk the game deleted is not part of the answer.
    db = LevelDB(db_path)
    try:
        db.delete(chunk_key(0, 0, 4))
    finally:
        db.close()
    expect_column("overworld (0, 0) after a deletion", 0, 0)
    checks += 1

    # --- A payload neither reader can decode is exit 3, not air.
    db = LevelDB(db_path)
    try:
        db.put(chunk_key(0, 0, 8), b"\x08\xfa" + b"xxxxxx")
    finally:
        db.close()
    rc, out, err = run_height(0, 0)
    if rc != 3:
        raise AssertionError(
            f"junk payload: exit {rc}, want 3 ({err})")
    checks += 1
    _log("junk payload: exit 3")

    # --- A block mcme places into ground that was never saved is
    #     the next height read's surface, and amulet finds it
    #     there.
    r = subprocess.run(
        [args.mcme, "chunk", "set", world, "200", "40", "200",
         "mcme:placed"],
        capture_output=True, text=True,
    )
    if r.returncode != 0:
        raise AssertionError(
            f"set into unstored ground: exit {r.returncode}: "
            f"{r.stderr.strip()}")
    checks += 1
    expect_column("overworld (200, 200) after a set", 200, 200)

    shutil.rmtree(tmp, ignore_errors=True)
    _log(f"ok — {checks} checks")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except AssertionError as exc:
        _log(f"FAIL: {exc}")
        sys.exit(1)
