#!/usr/bin/env python3
"""
Cross-check: `mcme chunk fill` on a Java world — the box through the
section palettes, one repack per section, and the whole-fill
refusals.

The C selftest pins the library against hand-built trees. This tool
proves the CLI against the REFERENCE: it reads the region files
itself — its own sector table walk, its own palette unpacking, no
mcme code — and compares block for block with what the box wrote.

  FILL     — a box covering a whole section lands the format's own
             shorthand: one palette entry and no data array, and the
             walk reads the block at every position of the box.

  NO-OP    — a fill that changes nothing rewrites nothing: the
             region file keeps every byte, both for a box already
             standing in the block and for air on a section that is
             all air.

  PARTIAL  — a box reaching into a section keeps the rest: the
             positions outside it hold what they held, the palette
             grows by the one entry, states carried as Properties
             the walk reads back, and `chunk get` prints what the
             walk finds.

  CROSS    — a box spanning chunks fills each chunk it touches and
             leaves the neighbours alone — chunks past the box,
             sections past the box's Y, positions past its edges —
             and a box spanning region files fills the chunks in
             each file it can reach.

  ABSENT   — a box touching a region file the world does not have
             refuses the whole fill: exit 2, no byte of any region
             file changed. A chunk the file does not have the same.
             The refusal is checked BEFORE anything is written, so
             the untouched files are compared byte for byte.

  ROOM     — fill prints nothing on success; junk coordinates and
             junk specs exit 1; a missing world and a dimension the
             game has not made exit 2; Bedrock fill still answers
             through its own layer, unchanged.

Usage:
    venv/bin/python tools/ref_java_fill.py --mcme ./mcme
"""

from __future__ import annotations

import argparse
import gzip
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
    print(f"ref_java_fill: {msg}", file=sys.stderr)


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


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--mcme", required=True)
    args = ap.parse_args()
    mcme = args.mcme

    checks = 0
    tmp = tempfile.mkdtemp(prefix="ref_java_fill_")
    try:
        world = os.path.join(tmp, "java")
        shutil.copytree(FIXTURE_WORLD, world)
        region = os.path.join(world, "region", "r.0.0.mca")
        nether_file = os.path.join(world, "region", "r.-1.-1.mca")

        def fill(*argv, want=0):
            rc, out, err = run(mcme, "chunk", "fill", world, *argv)
            if rc != want or out != "":
                raise AssertionError(
                    f"fill {argv}: exit {rc} stdout {out!r}, "
                    f"want {want} and nothing printed ({err.strip()})")
            return err

        def get(x: int, y: int, z: int, want: str):
            rc, out, err = run(mcme, "chunk", "get", world,
                               str(x), str(y), str(z))
            if rc != 0 or out.strip() != want:
                raise AssertionError(
                    f"get ({x}, {y}, {z}): exit {rc} {out.strip()!r}, "
                    f"want {want!r} ({err.strip()})")

        def bytes_of(path: str) -> bytes:
            with open(path, "rb") as fh:
                return fh.read()

        # ---- ABSENT: the whole fill refuses, nothing written ------
        # The box spans four region files: r.-1.-1 exists, r.-1.0
        # and r.0.-1 the world does not have at all. The check runs
        # over every file before the first write, so a missing one
        # leaves every file holding the bytes it held.
        before = {p: bytes_of(p)
                  for p in (region, nether_file)}
        err = fill("-16", "-16", "-16", "15", "15", "15",
                   "minecraft:dirt", want=2)
        if "not stored" not in err:
            raise AssertionError(f"absent file: stderr {err!r}")
        for p, data in before.items():
            if bytes_of(p) != data:
                raise AssertionError(f"absent file: {p} was written")
        checks += 1
        _log("ok — a box reaching a region file the world does not "
             "have refuses the whole fill, nothing written")

        # In the file the world HAS: a box reaching chunk (1, 1),
        # which the slot table does not fill. Same refusal, same
        # byte-for-byte silence.
        err = fill("0", "0", "0", "31", "15", "31",
                   "minecraft:dirt", want=2)
        if "not stored" not in err:
            raise AssertionError(f"absent chunk: stderr {err!r}")
        if bytes_of(region) != before[region]:
            raise AssertionError("absent chunk: r.0.0.mca was written")
        checks += 1
        _log("ok — a chunk the file does not have refuses the fill "
             "the same way")

        # ---- FILL: a whole section, the format's shorthand --------
        fill("0", "0", "0", "15", "15", "15", "minecraft:stone")
        tag = walk_chunk(region, 0, 0)
        if tag is None:
            raise AssertionError("the walk finds no chunk (0, 0)")
        sec = section_for(tag, 0)
        if sec is None:
            raise AssertionError("no section 0 after the fill")
        bs = sec["block_states"]
        pal = bs["palette"]
        names = [e["Name"].py_data for e in pal]
        if names != ["minecraft:stone"]:
            raise AssertionError(f"palette is {names}, want one entry")
        if "data" in bs:
            raise AssertionError(
                "a fully covered section kept a data array: the "
                "format's own shorthand is one entry and none")
        for x, y, z in ((0, 0, 0), (8, 8, 8), (15, 15, 15), (3, 14, 7)):
            if block_at(tag, x, y, z) != "minecraft:stone":
                raise AssertionError(f"walk: ({x}, {y}, {z}) not stone")
        if block_at(tag, 0, 16, 0) != AIR:
            raise AssertionError("walk: section 1 was touched")
        get(3, 7, 9, "minecraft:stone")
        get(0, 16, 0, AIR)
        checks += 1
        _log("ok — a fully covered section is one entry, no data, "
             "and the walk reads the block everywhere in the box")

        # ---- NO-OP: a fill that changes nothing writes nothing ----
        once = bytes_of(region)
        fill("0", "0", "0", "15", "15", "15", "minecraft:stone")
        if bytes_of(region) != once:
            raise AssertionError(
                "the same fill again changed the region file")
        # air on a section the golden world stores as all air
        fill("0", "192", "0", "15", "207", "15", "minecraft:air")
        if bytes_of(region) != once:
            raise AssertionError(
                "air on an all-air section changed the region file")
        checks += 1
        _log("ok — a fill that changes nothing rewrites nothing")

        # ---- PARTIAL: the box's edges, the neighbours kept --------
        fill("0", "20", "0", "7", "30", "7",
             'minecraft:oak_log{pillar_axis:"x"}')
        tag = walk_chunk(region, 0, 0)
        sec = section_for(tag, 16)
        if sec is None:
            raise AssertionError("no section 1 after the partial fill")
        bs = sec["block_states"]
        names = [e["Name"].py_data for e in bs["palette"]]
        if names != [AIR, "minecraft:oak_log"]:
            raise AssertionError(
                f"partial palette is {names}, want air plus the block")
        if "data" not in bs:
            raise AssertionError("partial section has no data array")
        if bs["palette"][1]["Properties"]["pillar_axis"].py_data != "x":
            raise AssertionError("Properties did not survive the fill")
        want = {
            (0, 20, 0): 'minecraft:oak_log{pillar_axis: "x"}',
            (7, 30, 7): 'minecraft:oak_log{pillar_axis: "x"}',
            (7, 20, 3): 'minecraft:oak_log{pillar_axis: "x"}',
            (8, 20, 0): AIR,        # past the box in x
            (0, 19, 0): AIR,        # past the box in y
            (0, 31, 0): AIR,        # the section's own edge
            (8, 31, 7): AIR,
        }
        for (x, y, z), block in want.items():
            got = block_at(tag, x, y, z)
            if got != block:
                raise AssertionError(
                    f"walk: ({x}, {y}, {z}) is {got!r}, want {block!r}")
        get(3, 22, 3, 'minecraft:oak_log{pillar_axis: "x"}')
        get(8, 22, 3, AIR)
        checks += 1
        _log("ok — a partial box keeps everything outside it, "
             "states included, and get agrees with the walk")

        # ---- CROSS: chunks, sections, and neighbours --------------
        # The box spans chunks (0, 0) and (1, 0) — both stored —
        # through a height of a section neither had touched. A box
        # reaching (2, 0) would refuse instead: the golden file
        # does not store that chunk, the way ABSENT showed.
        fill("0", "40", "0", "17", "45", "15", "minecraft:stone")
        chunks = {}
        for cx, cz in ((0, 0), (1, 0), (0, 1)):
            chunks[(cx, cz)] = walk_chunk(region, cx, cz)
            if chunks[(cx, cz)] is None:
                raise AssertionError(f"chunk ({cx}, {cz}) vanished")
        want = {
            (0, 0): ((3, 44, 3, "minecraft:stone"),
                     (3, 32, 3, AIR),     # below the box
                     (3, 46, 3, AIR),     # above the box
                     (3, 5, 5, "minecraft:stone")),  # first fill
            (1, 0): ((16, 44, 15, "minecraft:stone"),
                     (17, 45, 0, "minecraft:stone"),
                     (18, 44, 3, AIR),    # past the box in x
                     (16, 32, 3, AIR),
                     (16, 46, 3, AIR)),
            (0, 1): ((3, 44, 19, AIR),   # chunk past the box
                     (3, 5, 19, AIR)),
        }
        for (cx, cz), points in want.items():
            tag = chunks[(cx, cz)]
            for x, y, z, block in points:
                got = block_at(tag, x, y, z)
                if got != block:
                    raise AssertionError(
                        f"walk: chunk ({cx}, {cz}) ({x}, {y}, {z}) "
                        f"is {got!r}, want {block!r}")
        get(16, 44, 15, "minecraft:stone")
        get(3, 44, 3, "minecraft:stone")
        get(18, 44, 3, AIR)
        checks += 1
        _log("ok — the box fills every chunk it touches and every "
             "neighbour keeps its block")

        # ---- CROSS-FILE: the same box in another region file ------
        fill("-16", "-16", "-16", "-1", "-1", "-1", "minecraft:dirt")
        tag = walk_chunk(nether_file, -1, -1)
        if tag is None:
            raise AssertionError("the walk finds no chunk (-1, -1)")
        sec = section_for(tag, -16)
        if sec is None:
            raise AssertionError("no section -1 after the cross fill")
        bs = sec["block_states"]
        names = [e["Name"].py_data for e in bs["palette"]]
        if names != ["minecraft:dirt"] or "data" in bs:
            raise AssertionError(
                f"cross-file section is {names} data={'data' in bs}")
        if block_at(tag, -1, -1, -1) != "minecraft:dirt":
            raise AssertionError("cross-file walk lost the block")
        if block_at(tag, -1, 0, -1) != AIR:
            raise AssertionError("cross-file fill touched section 0")
        checks += 1
        _log("ok — a box spans region files: each one the box can "
             "reach is filled, in its own file")

        # ---- ROOM: silence, usage, and Bedrock unchanged ----------
        for argv, want_rc in (
            (("chunk", "fill", world, "x", "0", "0", "1", "1", "1",
              "minecraft:stone"), 1),
            (("chunk", "fill", world, "0", "0", "0", "1", "1", "1",
              "minecraft:stone{x"), 1),
            (("chunk", "fill", world, "0", "0", "0", "1", "1", "1",
              "minecraft:stone", "--dimension", "7"), 1),
            # the box's Y past the byte that keeps a section's Y
            (("chunk", "fill", world, "0", "32768", "0", "1", "32767",
              "1", "minecraft:stone"), 1),
            (("chunk", "fill", os.path.join(tmp, "nope"), "0", "0", "0",
              "1", "1", "1", "minecraft:stone"), 2),
            # no DIM-1 directory: the chunks of that dimension are
            # not stored
            (("chunk", "fill", world, "0", "0", "0", "1", "1", "1",
              "minecraft:stone", "--dimension", "nether"), 2),
        ):
            rc, out, err = run(mcme, *argv)
            if rc != want_rc or out != "":
                raise AssertionError(
                    f"{' '.join(argv)}: exit {rc} stdout {out!r}, "
                    f"want {want_rc} and nothing printed")

        bedrock = os.path.join(tmp, "bedrock")
        shutil.copytree(FIXTURE_BEDROCK, bedrock)
        rc, out, err = run(mcme, "chunk", "fill", bedrock,
                           "12", "64", "-30", "30", "70", "-12",
                           "minecraft:stone")
        if rc != 0 or out != "":
            raise AssertionError(
                f"bedrock fill: exit {rc} {out!r} ({err.strip()})")
        rc, out, err = run(mcme, "chunk", "get", bedrock,
                           "13", "65", "-29")
        if rc != 0 or out.strip() != "minecraft:stone":
            raise AssertionError(
                f"bedrock get back: exit {rc} {out.strip()!r}")
        checks += 1
        _log("ok — usage and absences are documented; Bedrock fill "
             "still answers its own way")

        _log(f"ok — {checks} checks")
        return 0
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())
