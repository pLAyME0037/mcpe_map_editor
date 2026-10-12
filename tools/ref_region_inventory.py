#!/usr/bin/env python3
"""
Cross-check: mcme's region inventory against the region format read
straight from its own bytes.

The C selftest pins `mcme_region_chunk_payload` against the fixture
this tree carries. This tool proves the whole `mcme region` verb
against a reader written from the container's own description —
location table, timestamps, payload headers — with no help from
mcme and without decompressing anything:

  JSON  — for every region in the golden Java world, the object the
          verb prints must be, entry for entry and in order, what
          the sector tables say: which slots are filled, each
          payload's length and compression byte, each timestamp,
          and the world chunk coordinate each slot addresses.

  TEXT  — the human form must say the same thing as the JSON: the
          header line's counts and file size, one row per chunk in
          the same order, coordinates, sizes and compression
          matching, the timestamp printed as the UTC instant it
          names.

  ROOM  — an empty region lists on exit 0; a world that is not
          Java, a region file that is not there, a dimension Java
          has no directory for, a coordinate that is not a number,
          and a file whose payload header will not read each take
          their own exit code. A compression byte the format does
          not name is reported as the number it is — the inventory
          answers what the file holds, not what mcme can open.

Usage:
    venv/bin/python tools/ref_region_inventory.py --mcme ./mcme
"""

from __future__ import annotations

import argparse
import datetime
import json
import os
import re
import shutil
import struct
import subprocess
import sys
import tempfile

FIXTURE_WORLD = "tests/golden/worlds/java"

COMPRESSION = {1: "gzip", 2: "zlib", 3: "stored", 127: "external"}

ROW = re.compile(
    r"^  chunk \((-?\d+), (-?\d+)\)\s+(\S+)\s+(\d+) bytes  (\S+)$"
)


def _log(msg: str) -> None:
    print(f"ref_region_inventory: {msg}", file=sys.stderr)


def inventory(path: str) -> dict:
    """What one region file holds, from its own bytes: file size plus
    the entries in the file's slot order, with world chunk
    coordinates, exactly the fields the verb reports."""
    name = os.path.basename(path)[: -len(".mca")]
    _, rx_s, rz_s = name.split(".")
    rx, rz = int(rx_s), int(rz_s)

    with open(path, "rb") as fh:
        data = fh.read()

    entries = []
    for slot in range(1024):
        at = slot * 4
        sector_offset = int.from_bytes(data[at:at + 3], "big")
        if sector_offset == 0:
            continue
        timestamp = struct.unpack(">I", data[4096 + at:4096 + at + 4])[0]
        payload = sector_offset * 4096
        length = struct.unpack(">I", data[payload:payload + 4])[0]
        code = data[payload + 4]
        entries.append({
            "x": rx * 32 + slot % 32,
            "z": rz * 32 + slot // 32,
            "compression": COMPRESSION.get(code, str(code)),
            "bytes": length - 1,
            "timestamp": timestamp,
        })

    return {
        "path": path,
        "rx": rx,
        "rz": rz,
        "chunks": len(entries),
        "bytes": len(data),
        "entries": entries,
    }


def main() -> int:
    ap = argparse.ArgumentParser(
        description="mcme region and the format's own bytes agree"
    )
    ap.add_argument("--mcme", required=True, help="path to mcme")
    ap.add_argument(
        "--world",
        default=FIXTURE_WORLD,
        help="golden Java world whose region files are the starting point",
    )
    args = ap.parse_args()

    checks = 0

    def run(*argv: str):
        r = subprocess.run([args.mcme, *argv], capture_output=True, text=True)
        return r.returncode, r.stdout.strip(), r.stderr.strip()

    # The fixture's regions, each read both ways and compared to the
    # file's own bytes in full — order included, because slot order
    # is the file's, not the reporter's.
    region_dir = os.path.join(args.world, "region")
    names = sorted(n for n in os.listdir(region_dir) if n.endswith(".mca"))
    if not names:
        _log(f"FAIL: no region files in {region_dir}")
        return 1

    for name in names:
        _, rx_s, rz_s = name[: -len(".mca")].split(".")
        rx, rz = int(rx_s), int(rz_s)
        path = os.path.join(region_dir, name)
        want = inventory(path)

        rc, out, err = run("--json", "region", args.world, str(rx), str(rz))
        if rc != 0:
            raise AssertionError(
                f"{name}: --json exit {rc}, want 0 ({err})")
        got = json.loads(out)
        if got != want:
            raise AssertionError(
                f"{name}: JSON disagrees with the file\n"
                f"  got:  {got}\n  want: {want}")
        checks += 1
        _log(f"{name}: {got['chunks']} chunks, JSON = the sector tables")

        rc, out, err = run("region", args.world, str(rx), str(rz))
        if rc != 0:
            raise AssertionError(f"{name}: text exit {rc}, want 0 ({err})")
        lines = out.splitlines()
        head = f"{path}: {want['chunks']} of 1024 chunks, {want['bytes']} bytes"
        if lines[0] != head:
            raise AssertionError(
                f"{name}: header {lines[0]!r}, want {head!r}")
        rows = [ROW.match(line) for line in lines[1:]]
        if any(m is None for m in rows):
            raise AssertionError(f"{name}: unparseable row in {lines[1:]}")
        said = [
            {
                "x": int(m.group(1)),
                "z": int(m.group(2)),
                "compression": m.group(3),
                "bytes": int(m.group(4)),
                "timestamp": m.group(5),
            }
            for m in rows
        ]
        want_rows = [
            {
                **entry,
                "timestamp": datetime.datetime.fromtimestamp(
                    entry["timestamp"], datetime.timezone.utc
                ).strftime("%Y-%m-%dT%H:%M:%SZ"),
            }
            for entry in want["entries"]
        ]
        if said != want_rows:
            raise AssertionError(
                f"{name}: text rows disagree\n"
                f"  got:  {said}\n  want: {want_rows}")
        checks += 1
        _log(f"{name}: text = the JSON, rows in slot order")

    # A world copy, so the room checks can add files the fixture does
    # not carry without ever writing into the golden world itself.
    tmp = tempfile.mkdtemp(prefix="mcme_region_ref_")
    try:
        world = os.path.join(tmp, "world")
        shutil.copytree(args.world, world)
        copy_regions = os.path.join(world, "region")

        # An empty region: an all-zero header advertises no chunks,
        # and that is an answer, not a fault.
        empty = os.path.join(copy_regions, "r.3.3.mca")
        with open(empty, "wb") as fh:
            fh.write(bytes(8192))
        rc, out, err = run("--json", "region", world, "3", "3")
        got = json.loads(out) if rc == 0 else None
        if rc != 0 or got["chunks"] != 0 or got["entries"] != [] \
                or got["bytes"] != 8192:
            raise AssertionError(
                f"empty region: exit {rc}, got {out!r} ({err})")
        rc, out, err = run("region", world, "3", "3")
        if rc != 0 or out.splitlines() != [
            f"{empty}: 0 of 1024 chunks, 8192 bytes"
        ]:
            raise AssertionError(f"empty region text: exit {rc} {out!r}")
        checks += 1
        _log("empty region lists on exit 0, both forms")

        # A slot whose payload header declares length 0: the file is
        # there and is not one mcme reads — exit 3, message naming
        # the fault.
        junk = os.path.join(copy_regions, "r.4.4.mca")
        header = bytearray(bytes(8192))
        header[0:3] = (2).to_bytes(3, "big")  # slot (0, 0) at sector 2
        header[3] = 1                          # one sector claimed
        with open(junk, "wb") as fh:
            fh.write(bytes(header) + bytes(8192))
        rc, out, err = run("region", world, "4", "4")
        if rc != 3 or "length 0" not in err:
            raise AssertionError(
                f"zero-length region: exit {rc}, want 3 ({err})")
        rc, out, err = run("--json", "region", world, "4", "4")
        if rc != 3 or out != "":
            raise AssertionError(
                f"zero-length region: --json exit {rc} stdout {out!r}")
        checks += 1
        _log("a payload header that will not read is exit 3, silent in JSON")

        # Compression 7 — a byte the format does not name: the
        # inventory reports it as it stands and lists the chunk.
        unknown = os.path.join(copy_regions, "r.5.5.mca")
        header = bytearray(bytes(8192))
        header[0:3] = (2).to_bytes(3, "big")
        header[3] = 1
        body = bytearray(bytes(8192))
        body[0:4] = (10).to_bytes(4, "big")  # ten bytes incl. the comp byte
        body[4] = 7
        with open(unknown, "wb") as fh:
            fh.write(bytes(header) + bytes(body))
        rc, out, err = run("--json", "region", world, "5", "5")
        got = json.loads(out) if rc == 0 else None
        entry = {
            "x": 160, "z": 160, "compression": "7", "bytes": 9,
            "timestamp": 0,
        }
        if rc != 0 or got["entries"] != [entry]:
            raise AssertionError(
                f"unknown compression: exit {rc}, got {out!r} ({err})")
        checks += 1
        _log("an unnamed compression byte is reported, not refused")

        # The refusals, each on its own condition.
        rc, out, err = run("region", os.path.join(tmp, "nope"), "0", "0")
        if rc != 2:
            raise AssertionError(f"missing world: exit {rc}, want 2 ({err})")
        checks += 1
        _log("a world that is not there is exit 2")

        rc, out, err = run("region", args.world, "9", "9")
        if rc != 2:
            raise AssertionError(
                f"missing region: exit {rc}, want 2 ({err})")
        rc, out, err = run("--json", "region", args.world, "9", "9")
        if rc != 2 or out != "":
            raise AssertionError(
                f"missing region: --json exit {rc} stdout {out!r}")
        checks += 1
        _log("a region file that is not there is exit 2, silent in JSON")

        bedrock = os.path.join(args.world, "..", "bedrock")
        if os.path.isdir(bedrock):
            rc, out, err = run("region", bedrock, "0", "0")
            if rc != 2 or "not a Java world" not in err:
                raise AssertionError(
                    f"bedrock world: exit {rc} ({err})")
            checks += 1
            _log("a Bedrock world is exit 2")

        for dimension, directory in (("nether", "DIM-1"), ("end", "DIM1")):
            rc, out, err = run("region", args.world, "0", "0",
                               "--dimension", dimension)
            if rc != 2 or directory not in err:
                raise AssertionError(
                    f"--dimension {dimension}: exit {rc} ({err})")
            checks += 1
            _log(f"--dimension {dimension} looks in {directory}/")

        rc, out, err = run("region", args.world, "0", "0",
                           "--dimension", "7")
        if rc != 1 or "no directory for dimension 7" not in err:
            raise AssertionError(f"--dimension 7: exit {rc} ({err})")
        checks += 1
        _log("a dimension Java has no directory for is usage")

        for bad in (("x", "0"), ("0", "1.5")):
            rc, out, err = run("region", args.world, bad[0], bad[1])
            if rc != 1:
                raise AssertionError(
                    f"coordinate {bad!r}: exit {rc}, want 1 ({err})")
        rc, out, err = run("region", args.world, "0")
        if rc != 1:
            raise AssertionError(f"missing argument: exit {rc}, want 1")
        checks += 1
        _log("bad coordinates and missing arguments are usage")

    finally:
        shutil.rmtree(tmp, ignore_errors=True)

    _log(f"ok — {checks} checks")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except AssertionError as exc:
        _log(f"FAIL: {exc}")
        sys.exit(1)
