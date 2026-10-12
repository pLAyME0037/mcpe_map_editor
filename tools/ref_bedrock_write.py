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
  * a subchunk the world has not stored is created by
    `chunk set` and by `chunk fill`, in the version its
    neighbours use, while `chunk get` on ground still
    unstored refuses with exit 2 — reads report the store,
    writes extend it
  * a payload mcme cannot decode is a format problem (exit 3)
    for `chunk set` and `chunk fill` alike, and neither
    writes anything
  * `chunk get` reads both edits back as the spec `chunk set`
    accepts — name, states in braces — and the text it prints
    can be set again, reusing the palette entry instead of
    appending a second one; under `--json` the same text comes
    back escaped, with the coordinates echoed
  * `--dimension 1` edits the dimension's own key — same
    coordinates, different store entry — the overworld
    subchunk stays byte-identical, and an unknown dimension
    name is a usage error
  * `chunk fill` sets every position of a box (corners in any
    order) to the block, changes nothing outside it, reuses
    the palette entry instead of appending, leaves the
    subchunk the box does not reach byte-identical, and
    creates the subchunk when the box reaches into ground
    never stored
  * `chunk stats` counts a box: the filled box is exactly its
    eight tnt, an unstored subchunk counts as air with exit 0
    and a stderr note, the tally agrees with one `chunk get`
    per position, `--json` carries `missing_subchunks` and the
    sorted `blocks` array, and a Y past the key's byte is a
    coordinate error

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

    def read(cx: int, cz: int, cy: int, dimension: int = 0) -> bytes:
        db = LevelDB(db_path)
        try:
            try:
                v = db.get(chunk_key(cx, cz, cy, dimension))
            except KeyError:
                v = None
        finally:
            db.close()
        if v is None:
            raise AssertionError(
                f"key ({cx}, {cz}, {cy}, dim {dimension}) not in store"
            )
        return v

    neighbour_before = read(1, -2, 0)

    def run_set(x: int, y: int, z: int, block: str, dimension: int = 0):
        cmd = [args.mcme, "chunk", "set", world, str(x), str(y), str(z), block]
        if dimension != 0:
            cmd += ["--dimension", str(dimension)]
        r = subprocess.run(cmd, capture_output=True, text=True)
        return r.returncode, r.stderr.strip()

    def run_get(x: int, y: int, z: int, json_mode: bool = False,
                dimension: int = 0):
        cmd = [args.mcme]
        if json_mode:
            cmd.append("--json")
        cmd += ["chunk", "get", world, str(x), str(y), str(z)]
        if dimension != 0:
            cmd += ["--dimension", str(dimension)]
        r = subprocess.run(cmd, capture_output=True, text=True)
        return r.returncode, r.stdout.strip(), r.stderr.strip()

    def run_cli(*args_):
        r = subprocess.run(
            [args.mcme] + [str(a) for a in args_],
            capture_output=True, text=True,
        )
        return r.returncode, r.stdout.strip(), r.stderr.strip()

    def edit_case(
        name: str,
        x: int,
        y: int,
        z: int,
        block_name: str,
        states: CompoundTag,
        spec: str,
        dimension: int = 0,
    ) -> None:
        cx, cz = x // 16, z // 16
        cy = y // 16
        lx, ly, lz = x & 15, y & 15, z & 15

        before_payload = read(cx, cz, cy, dimension)
        before = decode_payload(iface, before_payload)
        rc, err = run_set(x, y, z, spec, dimension=dimension)
        if rc != 0:
            raise AssertionError(f"{name}: mcme exit {rc}: {err}")
        after_payload = read(cx, cz, cy, dimension)
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

    # --- --dimension 1 edits the dimension's own key: same
    #     coordinates, different store entry, and the overworld
    #     subchunk is byte-identical after. ---
    overworld_before = read(0, 0, 0)
    edit_case(
        "tnt in dimension 1 at (0, 0, 0)",
        0, 0, 0,
        "minecraft:tnt",
        CompoundTag(),
        "minecraft:tnt",
        dimension=1,
    )
    if read(0, 0, 0) != overworld_before:
        raise AssertionError("the dimension-1 edit touched the overworld")
    rc, out, err = run_get(0, 0, 0, dimension=1)
    if rc != 0 or out != "minecraft:tnt":
        raise AssertionError(f"get --dimension 1: exit {rc}, {out!r}: {err}")
    print("  ok dimension 1 reads back, overworld untouched")

    # --- an unknown dimension name is a usage error. ---
    r = subprocess.run(
        [args.mcme, "chunk", "set", world, "0", "0", "0",
         "minecraft:tnt", "--dimension", "mars"],
        capture_output=True, text=True,
    )
    if r.returncode != 1:
        raise AssertionError(f"bad dimension name: exit {r.returncode}")
    print("  ok bad dimension name exits 1")

    # --- fill sets every position of a box (corners given in
    #     any order), changes nothing outside it, and reuses
    #     the palette entry the set cases already added. ---
    cy2_before = read(0, 0, 2)
    _, layers0 = decode_payload(iface, read(0, 0, 0))
    before_blocks = numpy.asarray(layers0[0][0]).reshape(16, 16, 16)
    palette_len = len(layers0[0][1])
    tnt_bytes = expected_entry("minecraft:tnt", CompoundTag())
    tnt_index = None
    for j, t in enumerate(layers0[0][1]):
        if pal_bytes(t) == tnt_bytes:
            tnt_index = j
    if tnt_index is None:
        raise AssertionError("tnt missing from the palette")
    rc, _, err = run_cli("chunk", "fill", world,
                      1, 1, 1, 0, 0, 0, "minecraft:tnt")
    if rc != 0:
        raise AssertionError(f"fill: exit {rc}: {err}")
    version, layers = decode_payload(iface, read(0, 0, 0))
    if len(layers[0][1]) != palette_len:
        raise AssertionError("fill appended a palette entry")
    blocks = numpy.asarray(layers[0][0]).reshape(16, 16, 16)
    want = before_blocks.copy()
    want[0:2, 0:2, 0:2] = tnt_index
    if not numpy.array_equal(blocks, want):
        raise AssertionError("fill: grid differs outside the box")
    if read(0, 0, 2) != cy2_before:
        raise AssertionError("fill touched a subchunk it does not "
                             "reach")
    rc, out, err = run_get(1, 1, 1)
    if rc != 0 or out != "minecraft:tnt":
        raise AssertionError(f"get after fill: exit {rc}, {out!r}: {err}")
    print("  ok fill sets the box and nothing else")

    # --- a fill whose box reaches into ground the world never
    #     stored creates it, and amulet reads that subchunk
    #     back: all tnt, version 8, air under the block. ---
    rc, _, err = run_cli("chunk", "fill", world,
                         32, 0, 0, 47, 15, 15, "minecraft:tnt")
    if rc != 0:
        raise AssertionError(f"fill into unstored ground: exit {rc}: {err}")
    version, layers = decode_payload(iface, read(2, 0, 0))
    if version != 8:
        raise AssertionError(f"created subchunk version {version}, want 8")
    if len(layers[0][1]) != 2:
        raise AssertionError(
            f"created subchunk palette: {len(layers[0][1])} entries"
        )
    if pal_bytes(layers[0][1][0]) != expected_entry(
        "minecraft:air", CompoundTag()
    ):
        raise AssertionError("created subchunk's first entry is not air")
    blocks = numpy.asarray(layers[0][0]).reshape(16, 16, 16)
    if not numpy.array_equal(blocks, numpy.full((16, 16, 16), 1)):
        raise AssertionError("fill into unstored ground: not all tnt")
    rc, out, err = run_get(32, 8, 0)
    if rc != 0 or out != "minecraft:tnt":
        raise AssertionError(f"get on created ground: exit {rc}, {out!r}: {err}")
    print("  ok fill creates the subchunk it reaches into")

    # --- stats counts what a box is made of, in the spec text
    #     the other verbs speak. The filled box is one block. ---
    rc, out, err = run_cli("chunk", "stats", world, 0, 0, 0, 1, 1, 1)
    if rc != 0:
        raise AssertionError(f"stats: exit {rc}: {err}")
    if out.splitlines() != ["8 minecraft:tnt"]:   # run_cli strips
        raise AssertionError(f"stats of the filled box: {out!r}")
    if err:
        raise AssertionError(f"stats of a stored box noted: {err!r}")
    print("  ok stats counts the filled box exactly")

    # --- a box with a missing subchunk counts it as air, exits
    #     0 (reading a hole is answerable), and says so. ---
    rc, out, err = run_cli("chunk", "stats", world, 0, 0, 0, 0, 48, 0)
    if rc != 0:
        raise AssertionError(f"stats hole: exit {rc}: {err}")
    rows = []
    for line in out.splitlines():
        count_s, spec = line.split(None, 1)
        rows.append((int(count_s), spec))
    if sum(c for c, _ in rows) != 49:
        raise AssertionError(f"stats hole total: {rows!r}")
    if [c for c, s in rows if s == "minecraft:air"] != [1]:
        raise AssertionError(f"stats hole air: {rows!r}")
    counts = [c for c, _ in rows]
    if counts != sorted(counts, reverse=True):
        raise AssertionError(f"stats not sorted: {rows!r}")
    if "1 subchunk not stored, counted as air" not in err:
        raise AssertionError(f"stats hole stderr: {err!r}")
    print("  ok stats counts an unstored subchunk as air")

    # --- the same verdict by hand: one chunk get per position
    #     of the small box, tallied. ---
    per_position = {}
    for x in range(0, 2):
        for y in range(0, 2):
            for z in range(0, 2):
                rc, spec, err = run_get(x, y, z)
                if rc != 0:
                    raise AssertionError(
                        f"get for stats: exit {rc}: {err}")
                per_position[spec] = per_position.get(spec, 0) + 1
    rc, out, err = run_cli("chunk", "stats", world, 0, 0, 0, 1, 1, 1)
    if rc != 0:
        raise AssertionError(f"stats re-run: exit {rc}: {err}")
    got = {}
    for line in out.splitlines():
        count_s, spec = line.split(None, 1)
        got[spec] = int(count_s)
    if got != per_position:
        raise AssertionError(f"stats vs get: {got} != {per_position}")
    print("  ok stats agrees with chunk get, position by position")

    # --- --json: missing_subchunks and the sorted blocks
    #     array, exactly as wiki/cli.md documents. ---
    rc, out, err = run_cli("--json", "chunk", "stats", world,
                           0, 0, 0, 0, 48, 0)
    if rc != 0:
        raise AssertionError(f"stats --json: exit {rc}: {err}")
    doc = json.loads(out)
    if doc["missing_subchunks"] != 1:
        raise AssertionError(f"stats --json missing: {doc!r}")
    jcounts = [row["count"] for row in doc["blocks"]]
    if jcounts != sorted(jcounts, reverse=True):
        raise AssertionError(f"stats --json unsorted: {doc!r}")
    if any(not isinstance(row["block"], str) for row in doc["blocks"]):
        raise AssertionError(f"stats --json block field: {doc!r}")
    print("  ok stats --json shape")

    # --- a Y the key cannot name is a coordinate problem: the
    #     box is refused before the store opens (exit 1). ---
    rc, _, err = run_cli("chunk", "stats", world, 0, 0, 0, 0, 3000, 0)
    if rc != 1:
        raise AssertionError(f"stats deep Y: exit {rc}: {err}")
    print("  ok stats refuses a Y beyond the key's byte")

    # --- The neighbour chunk is untouched, byte for byte. ---
    if read(1, -2, 0) != neighbour_before:
        raise AssertionError("neighbouring chunk changed")
    print("  ok neighbour untouched")

    # --- `chunk set` into ground never stored creates it: the
    #     key appears, amulet decodes the payload, and the
    #     block sits in a subchunk that was absent a moment
    #     ago — version 9, because neighbour cy 4 carries the
    #     Y index. `chunk get` on ground still unstored keeps
    #     refusing: reads report the store, writes extend it. ---
    db = LevelDB(db_path)
    try:
        try:
            db.get(chunk_key(0, 0, 3))
            raise AssertionError("cy3 already stored, nothing to create")
        except KeyError:
            pass
    finally:
        db.close()
    rc, err = run_set(0, 48, 0, "minecraft:tnt")
    if rc != 0:
        raise AssertionError(f"set into unstored ground: exit {rc}: {err}")
    version, layers = decode_payload(iface, read(0, 0, 3))
    if version != 9:
        raise AssertionError(f"created subchunk version {version}, want 9")
    if len(layers[0][1]) != 2:
        raise AssertionError(
            f"created subchunk palette: {len(layers[0][1])} entries"
        )
    blocks = numpy.asarray(layers[0][0]).reshape(16, 16, 16)
    if block_at(blocks, 0, 0, 0) != 1:
        raise AssertionError("created subchunk does not hold the block")
    for lx, ly, lz in ((15, 15, 15), (1, 0, 0), (0, 1, 0)):
        if block_at(blocks, lx, ly, lz) != 0:
            raise AssertionError("created subchunk is not air elsewhere")
    rc, out, err = run_get(0, 48, 0)
    if rc != 0 or out != "minecraft:tnt":
        raise AssertionError(f"get on created ground: {out!r}: {err}")
    rc, out, err = run_get(0, 96, 0)
    if rc != 2:
        raise AssertionError(f"get on unstored ground: exit {rc}: {err}")
    print("  ok set creates the subchunk, get on a hole still exits 2")

    # --- A payload mcme cannot decode is a format problem for
    #     both write verbs (exit 3), and neither writes
    #     anything: the junk stays junk. ---
    junk = bytes([8, 250]) + b"xxxxxx"
    db = LevelDB(db_path)
    try:
        db.put(chunk_key(0, 0, 5), junk)
    finally:
        db.close()
    rc, _, err = run_cli("chunk", "set", world, "0", "80", "0",
                         "minecraft:tnt")
    if rc != 3:
        raise AssertionError(f"set into unreadable payload: exit {rc}: {err}")
    rc, _, err = run_cli("chunk", "fill", world,
                         0, 80, 0, 15, 95, 15, "minecraft:tnt")
    if rc != 3:
        raise AssertionError(f"fill over unreadable payload: exit {rc}: {err}")
    db = LevelDB(db_path)
    try:
        still_junk = db.get(chunk_key(0, 0, 5)) == junk
    finally:
        db.close()
    if not still_junk:
        raise AssertionError("a refused write changed the payload")
    print("  ok an unreadable payload exits 3, writes nothing")

    shutil.rmtree(tmp, ignore_errors=True)
    print("PASS: mcme writes, amulet reads")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except AssertionError as e:
        print(f"FAIL: {e}", file=sys.stderr)
        sys.exit(1)
