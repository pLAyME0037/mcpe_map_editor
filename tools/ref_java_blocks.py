#!/usr/bin/env python3
"""
Cross-check: `mcme chunk get` / `chunk set` answer for a Java world
through the chunk's section palettes, and what they print is what the
1.20 layout holds.

The C selftest pins the palette layer against hand-built trees. This
tool proves the CLI against the REFERENCE: it reads the region file
itself — its own sector table walk, its own palette unpacking, no mcme
code — and compares block for block with what the verbs say.

  AIR       — the golden world's sections are stored without
              block_states, which the format says is air: every
              position reads `minecraft:air`, including one no
              section byte could name. JSON echoes the coordinates.

  ABSENT    — a chunk the region file does not have is "not stored",
              exit 2, for both verbs; a dimension without a region
              directory the same; junk coordinates and block specs
              exit 1; a missing world exit 2.

  SET       — a scratch copy: `chunk set` writes blocks the reference
              walk finds at the right positions with the right palette
              — a first edit into an untouched section makes a palette
              of air plus the block, states carried as Properties —
              and `chunk get` prints the same names the walk reads,
              neighbours still air.

  REUSE     — a block the section's palette already holds reuses its
              entry: the palette does not grow, and the position
              points at the entry it names.

  NO-OP     — the block already stored is exit 0 and no byte of the
              region file changes; air on a position no section
              covers is the same no-op, and no section is invented
              to hold it.

  SECTIONS  — a section the chunk does not list is created and kept
              in Y order: one appended above the stored ones, one
              spliced in before them, and the reference walk sees
              both in order with the block inside.

  ROOM      — set prints nothing on any path; `--dimension` names a
              directory the game has not made yet as "not stored";
              a Bedrock world still answers through its own layer,
              unchanged.

Usage:
    venv/bin/python tools/ref_java_blocks.py --mcme ./mcme
"""

from __future__ import annotations

import argparse
import gzip
import json
import os
import shutil
import struct
import subprocess
import sys
import tempfile

import amulet_nbt
import zlib

FIXTURE_WORLD = "tests/golden/worlds/java"
FIXTURE_BEDROCK = "tests/golden/worlds/bedrock"
AIR = "minecraft:air"


def _log(msg: str) -> None:
    print(f"ref_java_blocks: {msg}", file=sys.stderr)


def run(*argv: str):
    p = subprocess.run(argv, capture_output=True, text=True)
    return p.returncode, p.stdout, p.stderr


def walk_chunk(region_path: str, cx: int, cz: int):
    """The chunk's tag, read from the region file with no library: the
    sector table walked by hand, the payload decompressed here. None
    when the slot is empty."""
    with open(region_path, "rb") as fh:
        data = fh.read()
    slot = (cx & 31) + (cz & 31) * 32
    sector_offset = int.from_bytes(data[slot * 4:slot * 4 + 3], "big")
    if sector_offset == 0:
        return None
    at = sector_offset * 4096
    (length,) = struct.unpack(">I", data[at:at + 4])
    comp = data[at + 4]
    blob = data[at + 5:at + 5 + length - 1]
    if comp == 2:
        raw = zlib.decompress(blob)
    elif comp == 1:
        raw = gzip.decompress(blob)
    elif comp == 3:
        raw = blob
    else:
        raise AssertionError(f"{region_path}: compression {comp}")
    return amulet_nbt.load(raw).tag


def section_for(tag, y: int):
    """The section stored for a section Y, or None."""
    secs = tag.get("sections")
    if secs is None:
        return None
    want = y >> 4
    for s in secs:
        if s["Y"].py_data == want:
            return s
    return None


def block_at(tag, x: int, y: int, z: int) -> str:
    """The block at a position, unpacked the way the format packs —
    the reference's own bits, none of mcme's."""
    sec = section_for(tag, y)
    if sec is None:
        return AIR
    bs = sec.get("block_states")
    if bs is None:
        return AIR
    pal = bs["palette"]
    if len(pal) == 1:
        idx = 0
    else:
        bits = max(4, (len(pal) - 1).bit_length())
        per_long = 64 // bits
        pos = (x & 15) + (z & 15) * 16 + (y & 15) * 256
        word, bit = divmod(pos, per_long)
        longs = bs["data"].py_data
        idx = (longs[word] >> (bit * bits)) & ((1 << bits) - 1)
    entry = pal[idx]
    name = entry["Name"].py_data
    props = entry.get("Properties")
    if props is not None and len(props) > 0:
        items = ", ".join(
            f'{k}: "{v.py_data}"'
            for k, v in sorted(props.items(), key=lambda kv: kv[0].lower()))
        return f"{name}{{{items}}}"
    return name


def palette_len(tag, y: int) -> int:
    sec = section_for(tag, y)
    bs = None if sec is None else sec.get("block_states")
    if bs is None:
        return 0
    return len(bs["palette"])


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--mcme", required=True)
    args = ap.parse_args()
    mcme = args.mcme

    checks = 0
    tmp = tempfile.mkdtemp(prefix="ref_java_blocks_")
    try:
        world = os.path.join(tmp, "java")
        shutil.copytree(FIXTURE_WORLD, world)
        region = os.path.join(world, "region", "r.0.0.mca")

        # ---- AIR: the golden sections say air everywhere ----------
        for x, y, z in ((0, 0, 0), (15, 255, 15), (7, -100, 9)):
            rc, out, err = run(mcme, "chunk", "get", world,
                               str(x), str(y), str(z))
            if rc != 0 or out.strip() != AIR:
                raise AssertionError(
                    f"get ({x}, {y}, {z}): exit {rc} {out!r}, "
                    f"want 0 and {AIR!r} ({err.strip()})")
        checks += 1
        _log("ok — the golden sections read as the format's air")

        rc, out, err = run(mcme, "--json", "chunk", "get", world,
                           "5", "5", "5")
        if rc != 0 or json.loads(out) != {"block": AIR, "x": 5, "y": 5,
                                          "z": 5}:
            raise AssertionError(f"json get: exit {rc} {out!r}")
        checks += 1
        _log("ok — --json echoes the block and the coordinates")

        # ---- ABSENT: a chunk the region file does not have --------
        for argv, want in (
            (("chunk", "get", world, "40", "5", "5"), 2),
            (("chunk", "set", world, "40", "5", "5", "minecraft:stone"), 2),
            (("chunk", "get", world, "5", "5", "5",
              "--dimension", "nether"), 2),
            (("chunk", "set", world, "5", "5", "5", "minecraft:stone",
              "--dimension", "nether"), 2),
            (("chunk", "get", os.path.join(tmp, "nope"), "0", "0", "0"), 2),
            (("chunk", "get", world, "x", "0", "0"), 1),
            (("chunk", "set", world, "0", "0", "0", "minecraft:stone{x"), 1),
            (("chunk", "set", world, "0", "0", "0", "minecraft:stone",
              "--dimension", "7"), 1),
            # a y no section byte could name: the edit cannot be
            # held — reading it is air, checked below
            (("chunk", "set", world, "0", "32768", "0",
              "minecraft:stone"), 1),
        ):
            rc, out, err = run(mcme, *argv)
            if rc != want or out != "":
                raise AssertionError(
                    f"{' '.join(argv)}: exit {rc} stdout {out!r}, "
                    f"want {want} and nothing printed")
        rc, out, err = run(mcme, "chunk", "get", world, "0", "32768", "0")
        if out.strip() != AIR:
            raise AssertionError(f"get past the byte: {out!r}")
        checks += 1
        _log("ok — absent chunks, missing dimensions, junk: documented")

        # ---- SET: the walk reads what the verbs wrote -------------
        def set_block(*args_):
            rc, out, err = run(mcme, "chunk", "set", world, *args_)
            if rc != 0 or out != "":
                raise AssertionError(
                    f"set {args_}: exit {rc} {out!r} ({err.strip()})")

        def get_block(x: int, y: int, z: int, want: str):
            rc, out, err = run(mcme, "chunk", "get", world,
                               str(x), str(y), str(z))
            if rc != 0 or out.strip() != want:
                raise AssertionError(
                    f"get ({x}, {y}, {z}): exit {rc} {out.strip()!r}, "
                    f"want {want!r} ({err.strip()})")

        set_block("0", "0", "0", "minecraft:stone")
        set_block("5", "5", "5", 'minecraft:oak_log{pillar_axis:"x"}')
        get_block(0, 0, 0, "minecraft:stone")
        get_block(5, 5, 5, 'minecraft:oak_log{pillar_axis: "x"}')
        get_block(1, 0, 0, AIR)
        get_block(5, 6, 5, AIR)

        tag = walk_chunk(region, 0, 0)
        if tag is None:
            raise AssertionError("the walk finds no chunk (0, 0)")
        if block_at(tag, 0, 0, 0) != "minecraft:stone":
            raise AssertionError("walk: (0, 0, 0) is not stone")
        if block_at(tag, 5, 5, 5) != 'minecraft:oak_log{pillar_axis: "x"}':
            raise AssertionError("walk: (5, 5, 5) lost its states")
        if block_at(tag, 1, 0, 0) != AIR or block_at(tag, 5, 6, 5) != AIR:
            raise AssertionError("walk: a neighbour is not air")
        # the first edit into an untouched section: air plus the
        # block, states carried as Properties the reference reads
        sec = section_for(tag, 0)
        pal = sec["block_states"]["palette"]
        names = [e["Name"].py_data for e in pal]
        if names != [AIR, "minecraft:stone", "minecraft:oak_log"]:
            raise AssertionError(f"palette is {names}")
        entry = pal[2]
        if entry["Properties"]["pillar_axis"].py_data != "x":
            raise AssertionError("Properties did not survive the write")
        checks += 1
        _log("ok — set writes what the reference's own walk reads")

        # ---- REUSE: a block the palette holds reuses its entry ---
        set_block("1", "0", "0", "minecraft:stone")
        get_block(1, 0, 0, "minecraft:stone")
        tag = walk_chunk(region, 0, 0)
        if palette_len(tag, 0) != 3:
            raise AssertionError("the palette grew for a held block")
        if block_at(tag, 1, 0, 0) != "minecraft:stone":
            raise AssertionError("walk: (1, 0, 0) is not stone")
        checks += 1
        _log("ok — a block the palette holds reuses its entry")

        # ---- NO-OP: the same block changes no byte ---------------
        before = open(region, "rb").read()
        set_block("0", "0", "0", "minecraft:stone")
        if open(region, "rb").read() != before:
            raise AssertionError("a no-op set changed the region file")
        # air on a position no section covers: no section invented
        set_block("0", "256", "0", AIR)
        if open(region, "rb").read() != before:
            raise AssertionError("air on air changed the region file")
        get_block(0, 256, 0, AIR)
        checks += 1
        _log("ok — a no-op is exit 0 and no byte moves")

        # ---- SECTIONS: created, and kept in Y order --------------
        set_block("0", "256", "0", "minecraft:stone")   # Y 16: appended
        get_block(0, 256, 0, "minecraft:stone")
        get_block(0, 255, 0, AIR)
        set_block("0", "-16", "0", "minecraft:dirt")    # Y -1: spliced in
        get_block(0, -16, 0, "minecraft:dirt")
        get_block(0, 0, 0, "minecraft:stone")
        tag = walk_chunk(region, 0, 0)
        order = [s["Y"].py_data for s in tag["sections"]]
        if order != sorted(order) or order[0] != -1 or order[-1] != 16:
            raise AssertionError(f"sections are {order}")
        if len(order) != 18:
            raise AssertionError(f"{len(order)} sections, want 18")
        if block_at(tag, 0, 256, 0) != "minecraft:stone":
            raise AssertionError("walk: the new top section lost its block")
        if block_at(tag, 0, -16, 0) != "minecraft:dirt":
            raise AssertionError("walk: the spliced section lost its block")
        checks += 1
        _log("ok — new sections are created, and kept in Y order")

        # ---- ROOM: silent writes, and Bedrock unchanged ----------
        rc, out, err = run(mcme, "chunk", "set", world, "9", "9", "9",
                           "minecraft:bedrock")
        if rc != 0 or out != "":
            raise AssertionError(f"set prints on success: {rc} {out!r}")

        bedrock = os.path.join(tmp, "bedrock")
        shutil.copytree(FIXTURE_BEDROCK, bedrock)
        rc, out, err = run(mcme, "chunk", "set", bedrock,
                           "12", "64", "-30", "minecraft:stone")
        if rc != 0 or out != "":
            raise AssertionError(
                f"bedrock set: exit {rc} {out!r} ({err.strip()})")
        rc, out, err = run(mcme, "chunk", "get", bedrock,
                           "12", "64", "-30")
        if rc != 0 or out.strip() != "minecraft:stone":
            raise AssertionError(
                f"bedrock get back: exit {rc} {out.strip()!r}")
        checks += 1
        _log("ok — writes are silent; Bedrock still answers its own way")

        _log(f"ok — {checks} checks")
        return 0
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())
