#!/usr/bin/env python3
"""
Cross-check: mcme writes a block, amulet reads the world back.

The C selftest proves the write against the original fixture
payloads. This tool proves it against the REFERENCE: it copies the
golden store, runs `mcme chunk set` on the copy, then decodes the
edited subchunk with amulet-core 1.9.50's own loader — the code
path that reads worlds Minecraft loads — and asserts:

  * the edited position holds the new block
  * every other position in the subchunk is unchanged
  * the palette gained exactly the entry amulet itself would
    write for that block, byte for byte: name, states, version in
    that order, version 17694720 ("1, 14, 0, 0")
  * a spec whose states differ from an existing entry (pillar_axis
    "y" where the palette holds "x") appends rather than reuses
  * the version-9 wrapper keeps its Y index
  * the neighbouring chunk's payload is untouched
  * a missing subchunk refuses with exit 2 rather than creating
  * `chunk get` reads both edits back as the spec `chunk set`
    accepts — name, states in braces — and the text it prints
    can be set again, reusing the palette entry instead of
    appending a second one; under `--json` the same text comes
    back escaped, with the coordinates echoed

Direction is deliberate: mcme writes, the reference reads — the
store layer's tests already run it the other way round.

Usage:
    venv/bin/python tools/ref_bedrock_write.py --mcme ./mcme
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
from amulet_nbt import CompoundTag, IntTag, StringTag

FIXTURE_WORLD = "tests/golden/worlds/bedrock-chunks"
BLOCK_VERSION = 17694720  # amulet's default: "1, 14, 0, 0"


def chunk_key(x: int, z: int, cy: int, dimension: int = 0) -> bytes:
    k = struct.pack("<ii", x, z)
    if dimension != 0:
        k += struct.pack("<i", dimension)
    return k + bytes([0x2F, cy & 0xFF])


def decode_payload(iface, payload: bytes):
    """Wrapper bytes stripped, then every layer through amulet's
    loader. Returns (version, layers) as [(blocks, palette)]."""
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


def block_at(blocks, lx: int, ly: int, lz: int) -> int:
    """amulet's in-memory array is [x][y][z]; the fixtures are
    designed that way and ravelled for disk."""
    return int(numpy.asarray(blocks).reshape(16, 16, 16)[lx][ly][lz])


def pal_bytes(tag) -> bytes:
    return tag.save_to(compressed=False, little_endian=True)


def expected_entry(name: str, states: CompoundTag) -> bytes:
    return CompoundTag(
        {
            "name": StringTag(name),
            "states": states,
            "version": IntTag(BLOCK_VERSION),
        }
    ).save_to(compressed=False, little_endian=True)


def main() -> int:
    ap = argparse.ArgumentParser(
        description="mcme writes, amulet reads back"
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

    tmp = tempfile.mkdtemp(prefix="mcme_write_ref_")
    world = os.path.join(tmp, "world")
    shutil.copytree(args.world, world)
    db_path = os.path.join(world, "db")

    def read(cx: int, cz: int, cy: int) -> bytes:
        db = LevelDB(db_path)
        try:
            try:
                v = db.get(chunk_key(cx, cz, cy))
            except KeyError:
                v = None
        finally:
            db.close()
        if v is None:
            raise AssertionError(f"key ({cx}, {cz}, {cy}) not in store")
        return v

    neighbour_before = read(1, -2, 0)

    def run_set(x: int, y: int, z: int, block: str):
        r = subprocess.run(
            [args.mcme, "chunk", "set", world, str(x), str(y), str(z), block],
            capture_output=True,
            text=True,
        )
        return r.returncode, r.stderr.strip()

    def run_get(x: int, y: int, z: int, json_mode: bool = False):
        cmd = [args.mcme]
        if json_mode:
            cmd.append("--json")
        cmd += ["chunk", "get", world, str(x), str(y), str(z)]
        r = subprocess.run(cmd, capture_output=True, text=True)
        return r.returncode, r.stdout.strip(), r.stderr.strip()

    def edit_case(
        name: str,
        x: int,
        y: int,
        z: int,
        block_name: str,
        states: CompoundTag,
        spec: str,
    ) -> None:
        cx, cz = x // 16, z // 16
        cy = y // 16
        lx, ly, lz = x & 15, y & 15, z & 15

        before_payload = read(cx, cz, cy)
        before = decode_payload(iface, before_payload)
        rc, err = run_set(x, y, z, spec)
        if rc != 0:
            raise AssertionError(f"{name}: mcme exit {rc}: {err}")
        after_payload = read(cx, cz, cy)
        version, layers = decode_payload(iface, after_payload)

        if version != before[0]:
            raise AssertionError(f"{name}: wrapper {version} != {before[0]}")
        if before[0] == 9 and after_payload[:3] != before_payload[:3]:
            raise AssertionError(
                f"{name}: v9 header {after_payload[:3]!r} != "
                f"{before_payload[:3]!r}"
            )
        if len(layers) != len(before[1]):
            raise AssertionError(f"{name}: layer count changed")

        for i, ((b0, p0), (b1, p1)) in enumerate(zip(before[1], layers)):
            a0 = numpy.asarray(b0).reshape(16, 16, 16)
            a1 = numpy.asarray(b1).reshape(16, 16, 16)
            if i == 0:
                want = len(p0) + 1
                if len(p1) != want:
                    raise AssertionError(
                        f"{name}: palette {len(p1)}, want {want}"
                    )
                if block_at(a1, lx, ly, lz) != len(p0):
                    raise AssertionError(
                        f"{name}: position holds index "
                        f"{block_at(a1, lx, ly, lz)}, want {len(p0)}"
                    )
                a1[lx][ly][lz] = a0[lx][ly][lz]  # restore, then all equal
                if not numpy.array_equal(a0, a1):
                    raise AssertionError(f"{name}: other positions changed")
                for j in range(len(p0)):
                    if pal_bytes(p1[j]) != pal_bytes(p0[j]):
                        raise AssertionError(
                            f"{name}: palette entry {j} changed"
                        )
                got = pal_bytes(p1[len(p0)])
                want_bytes = expected_entry(block_name, states)
                if got != want_bytes:
                    raise AssertionError(
                        f"{name}: new palette entry differs from amulet's\n"
                        f"  got  {got!r}\n  want {want_bytes!r}"
                    )
            else:
                if not numpy.array_equal(a0, a1):
                    raise AssertionError(f"{name}: layer {i} changed")
                if [pal_bytes(t) for t in p0] != [
                    pal_bytes(t) for t in p1
                ]:
                    raise AssertionError(f"{name}: layer {i} palette changed")
        print(f"  ok {name}")

    # --- A block with no states joins the 16-entry palette. ---
    edit_case(
        "tnt at (1, 0, 0)",
        1, 0, 0,
        "minecraft:tnt",
        CompoundTag(),
        "minecraft:tnt",
    )

    # --- States that differ from an existing entry must append,
    #     not reuse: the palette holds pillar_axis "x". ---
    edit_case(
        "oak_log{pillar_axis:y} at (0, 68, 0)",
        0, 68, 0,
        "minecraft:oak_log",
        CompoundTag({"pillar_axis": StringTag("y")}),
        'minecraft:oak_log{pillar_axis:"y"}',
    )

    # --- get reads back what set wrote, in the spec set accepts. ---
    rc, out, err = run_get(1, 0, 0)
    if rc != 0 or out != "minecraft:tnt":
        raise AssertionError(f"get (1, 0, 0): exit {rc}, {out!r}: {err}")
    print("  ok get reads tnt back")

    rc, out, err = run_get(0, 68, 0)
    want_spec = 'minecraft:oak_log{pillar_axis: "y"}'
    if rc != 0 or out != want_spec:
        raise AssertionError(f"get (0, 68, 0): exit {rc}, {out!r}: {err}")
    print("  ok get reads states back")

    # --- the text get prints can be set again, and reuses the
    #     palette entry instead of appending a second one. ---
    version, layers = decode_payload(iface, read(0, 0, 4))
    palette_len = len(layers[0][1])
    rc, err = run_set(1, 68, 0, out)
    if rc != 0:
        raise AssertionError(f"round-trip set: exit {rc}: {err}")
    rc, out2, err = run_get(1, 68, 0)
    if rc != 0 or out2 != out:
        raise AssertionError(f"round-trip get: {out2!r} != {out!r}: {err}")
    version, layers = decode_payload(iface, read(0, 0, 4))
    if len(layers[0][1]) != palette_len:
        raise AssertionError("round-trip set appended a palette entry")
    blocks = numpy.asarray(layers[0][0]).reshape(16, 16, 16)
    if block_at(blocks, 1, 4, 0) != block_at(blocks, 0, 4, 0):
        raise AssertionError("round-trip set did not reuse the entry")
    print("  ok get's text sets again, entry reused")

    # --- under --json the same text comes back escaped, with the
    #     coordinates echoed. ---
    rc, out, err = run_get(0, 68, 0, json_mode=True)
    doc = json.loads(out) if rc == 0 else None
    if doc != {"block": want_spec, "x": 0, "y": 68, "z": 0}:
        raise AssertionError(f"get --json: exit {rc}, {doc!r}: {err}")
    print("  ok get --json")

    # --- The neighbour chunk is untouched, byte for byte. ---
    if read(1, -2, 0) != neighbour_before:
        raise AssertionError("neighbouring chunk changed")
    print("  ok neighbour untouched")

    # --- A missing subchunk refuses with exit 2. ---
    rc, err = run_set(0, 48, 0, "minecraft:tnt")
    if rc != 2:
        raise AssertionError(f"missing subchunk: exit {rc}, want 2: {err}")
    db = LevelDB(db_path)
    try:
        try:
            db.get(chunk_key(0, 0, 3))
            still_absent = False
        except KeyError:
            still_absent = True
    finally:
        db.close()
    if not still_absent:
        raise AssertionError("refused edit created the subchunk anyway")
    print("  ok missing subchunk exits 2, nothing created")

    shutil.rmtree(tmp, ignore_errors=True)
    print("PASS: mcme writes, amulet reads")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except AssertionError as e:
        print(f"FAIL: {e}", file=sys.stderr)
        sys.exit(1)
