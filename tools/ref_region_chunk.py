#!/usr/bin/env python3
"""
Cross-check: `mcme region chunk` and amulet_nbt read the same chunk
out of the same region file.

The C selftest pins the region reader against the .nbt goldens this
tree carries. This tool proves the verb against the REFERENCE: it
walks the sector table itself, hands the inflated payload to
amulet_nbt's own loader, and requires the two readers to agree on the
WHOLE tree.

  READ   — for every chunk in the golden world's manifest,
           `mcme --json region chunk` against `amulet_nbt.load` of
           the payload the sector table points at: deep-equal plain
           Python on both sides, each through its own decoder.

  PRINT  — the document printed two ways from two paths must be the
           same text: `mcme region chunk`, SNBT and --json, against
           `mcme nbt dump` on the golden .nbt ref_region.py wrote
           from that payload — and mcme's SNBT must parse back
           through amulet's own `from_snbt` into the same tree, so
           the printed line is not just stable but readable.

  ROOM   — no chunk at those coordinates is exit 2 and says so, a
           region file that is not there is exit 2, a world that is
           not Java is exit 2, the DIM-1/DIM1 paths are looked in, a
           dimension Java has no directory for and a coordinate that
           is not a number are usage, and a chunk whose NBT will not
           decode is exit 3 — with nothing on stdout under --json.

Usage:
    venv/bin/python tools/ref_region_chunk.py --mcme ./mcme
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
import zlib

FIXTURE_WORLD = "tests/golden/worlds/java"
FIXTURE_GOLDS = "tests/golden/world"


def _log(msg: str) -> None:
    print(f"ref_region_chunk: {msg}", file=sys.stderr)


def plain(value):
    """Anything amulet's own decode gives, as plain Python: NBT tags
    unwrap through py_data, dicts and lists recurse, byte arrays
    become lists of numbers the way JSON has to hold them."""
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


def first_diff(a, b, path="$"):
    """Where two trees part company, or None: the report an agent can
    act on is the path, not the two trees."""
    if isinstance(a, dict) and isinstance(b, dict):
        if set(a) != set(b):
            return f"{path}: keys only here {sorted(set(a) - set(b))}, " \
                   f"only there {sorted(set(b) - set(a))}"
        for k in a:
            d = first_diff(a[k], b[k], f"{path}.{k}")
            if d:
                return d
        return None
    if isinstance(a, list) and isinstance(b, list):
        if len(a) != len(b):
            return f"{path}: {len(a)} items, the other {len(b)}"
        for i, (x, y) in enumerate(zip(a, b)):
            d = first_diff(x, y, f"{path}[{i}]")
            if d:
                return d
        return None
    if a != b:
        return f"{path}: {a!r} != {b!r}"
    return None


def payload_of(path: str, cx: int, cz: int) -> bytes:
    """One chunk's inflated NBT, straight from the sector table —
    the container read written from the format's own description,
    with no library on either side of the comparison."""
    with open(path, "rb") as fh:
        data = fh.read()
    slot = (cx & 31) + (cz & 31) * 32
    sector_offset = int.from_bytes(data[slot * 4:slot * 4 + 3], "big")
    if sector_offset == 0:
        raise AssertionError(f"{path}: no chunk at slot {slot}")
    at = sector_offset * 4096
    (length,) = struct.unpack(">I", data[at:at + 4])
    comp = data[at + 4]
    blob = data[at + 5:at + 5 + length - 1]
    if comp == 1:
        return gzip.decompress(blob)
    if comp == 2:
        return zlib.decompress(blob)
    if comp == 3:
        return blob
    raise AssertionError(f"{path}: compression byte {comp} is not handled")


def main() -> int:
    ap = argparse.ArgumentParser(
        description="mcme region chunk and amulet_nbt agree on the chunk"
    )
    ap.add_argument("--mcme", required=True, help="path to mcme")
    ap.add_argument(
        "--world",
        default=FIXTURE_WORLD,
        help="golden Java world whose chunks are the starting point",
    )
    ap.add_argument(
        "--golds",
        default=FIXTURE_GOLDS,
        help="directory of ref_region.py's .nbt exports",
    )
    args = ap.parse_args()

    import amulet_nbt

    checks = 0

    def run(*argv: str):
        r = subprocess.run([args.mcme, *argv], capture_output=True, text=True)
        return r.returncode, r.stdout.strip(), r.stderr.strip()

    manifest = os.path.join(args.golds, "MANIFEST.txt")
    entries = []
    with open(manifest) as fh:
        for line in fh:
            if line.startswith("#") or not line.strip():
                continue
            region, cx, cz, codec, _raw = line.split()
            entries.append((region, int(cx), int(cz)))
    if not entries:
        _log(f"FAIL: no chunks listed in {manifest}")
        return 1

    for region, cx, cz in entries:
        region_path = os.path.join(args.world, "region", f"{region}.mca")
        raw = payload_of(region_path, cx, cz)
        want = plain(amulet_nbt.load(raw).tag)

        rc, out, err = run("--json", "region", "chunk",
                           args.world, str(cx), str(cz))
        if rc != 0:
            raise AssertionError(f"({cx}, {cz}): --json exit {rc} ({err})")
        got = json.loads(out)
        d = first_diff(got, want)
        if d is not None:
            raise AssertionError(
                f"({cx}, {cz}): mcme and amulet_nbt disagree at {d}")
        checks += 1
        _log(f"({cx}, {cz}): mcme JSON = amulet_nbt on the raw payload")

        gold = os.path.join(args.golds, f"{region}.{cx}.{cz}.nbt")
        rc_snbt, snbt, err = run("region", "chunk", args.world, str(cx), str(cz))
        rc_dump, dumped, err = run("nbt", "dump", gold)
        if rc_snbt != 0 or rc_dump != 0:
            raise AssertionError(
                f"({cx}, {cz}): text exit {rc_snbt}, dump exit {rc_dump} ({err})")
        if snbt != dumped:
            raise AssertionError(
                f"({cx}, {cz}): SNBT differs from nbt dump on the golden")
        reparsed = plain(amulet_nbt.from_snbt(snbt))
        d = first_diff(reparsed, want)
        if d is not None:
            raise AssertionError(
                f"({cx}, {cz}): amulet cannot read mcme's SNBT at {d}")

        rc_json_a, json_a, err = run("--json", "region", "chunk",
                                     args.world, str(cx), str(cz))
        rc_json_b, json_b, err = run("--json", "nbt", "dump", gold)
        if json_a != json_b:
            raise AssertionError(
                f"({cx}, {cz}): JSON differs from nbt dump on the golden")
        checks += 1
        _log(f"({cx}, {cz}): printed text = nbt dump, and parses back")

    # A world copy, so the room checks can add files the fixture does
    # not carry without ever writing into the golden world itself.
    tmp = tempfile.mkdtemp(prefix="mcme_chunk_ref_")
    try:
        world = os.path.join(tmp, "world")
        shutil.copytree(args.world, world)

        rc, out, err = run("region", "chunk", world, "2", "2")
        if rc != 2 or "no chunk is stored at (2, 2)" not in err:
            raise AssertionError(f"absent chunk: exit {rc} ({err})")
        rc, out, err = run("--json", "region", "chunk", world, "2", "2")
        if rc != 2 or out != "":
            raise AssertionError(
                f"absent chunk: --json exit {rc} stdout {out!r}")
        checks += 1
        _log("a chunk the world does not store is exit 2, silent in JSON")

        rc, out, err = run("region", "chunk", world, "288", "0")
        if rc != 2 or "r.9.0.mca" not in err:
            raise AssertionError(f"missing region: exit {rc} ({err})")
        checks += 1
        _log("a region file that is not there is exit 2")

        bedrock = os.path.join(args.world, "..", "bedrock")
        if os.path.isdir(bedrock):
            rc, out, err = run("region", "chunk", bedrock, "0", "0")
            if rc != 2 or "not a Java world" not in err:
                raise AssertionError(f"bedrock world: exit {rc} ({err})")
            checks += 1
            _log("a Bedrock world is exit 2")

        for dimension, directory in (("nether", "DIM-1"), ("end", "DIM1")):
            rc, out, err = run("region", "chunk", world, "0", "0",
                               "--dimension", dimension)
            if rc != 2 or directory not in err:
                raise AssertionError(
                    f"--dimension {dimension}: exit {rc} ({err})")
            checks += 1
            _log(f"--dimension {dimension} looks in {directory}/")

        rc, out, err = run("region", "chunk", world, "0", "0",
                           "--dimension", "7")
        if rc != 1 or "no directory for dimension 7" not in err:
            raise AssertionError(f"--dimension 7: exit {rc} ({err})")
        for bad in (("x", "0"), ("0", "1.5")):
            rc, out, err = run("region", "chunk", world, bad[0], bad[1])
            if rc != 1:
                raise AssertionError(
                    f"coordinate {bad!r}: exit {rc}, want 1 ({err})")
        rc, out, err = run("region", "chunk", world)
        if rc != 1:
            raise AssertionError(f"missing argument: exit {rc}, want 1")
        checks += 1
        _log("bad dimensions and bad coordinates are usage")

        # A region whose chunk inflates to something that is not NBT:
        # the container is fine, the document is not, and the chunk —
        # the unit — refuses whole. Chunk (224, 224) is r.7.7, which
        # the fixture does not carry, so this file is its own.
        header = bytearray(bytes(8192))
        header[0:3] = (2).to_bytes(3, "big")  # slot (0, 0) at sector 2
        header[3] = 1
        body = bytearray(bytes(8192))
        blob = zlib.compress(b"not nbt at all")
        body[0:4] = (len(blob) + 1).to_bytes(4, "big")
        body[4] = 2
        body[5:5 + len(blob)] = blob
        with open(os.path.join(world, "region", "r.7.7.mca"), "wb") as fh:
            fh.write(bytes(header) + bytes(body))
        rc, out, err = run("region", "chunk", world, "224", "224")
        if rc != 3 or "will not decode" not in err:
            raise AssertionError(f"junk NBT: exit {rc}, want 3 ({err})")
        rc, out, err = run("--json", "region", "chunk", world, "224", "224")
        if rc != 3 or out != "":
            raise AssertionError(
                f"junk NBT: --json exit {rc} stdout {out!r}")
        checks += 1
        _log("a chunk that will not decode is exit 3, silent in JSON")

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
