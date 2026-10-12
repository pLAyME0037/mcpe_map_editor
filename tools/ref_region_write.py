#!/usr/bin/env python3
"""
Cross-check: `mcme region chunk set` writes a Java chunk the reference
reads back.

The C selftest pins the write against a byte-for-byte diff of the
golden file. This tool proves the bytes against the REFERENCE: it
walks the sector table itself and hands the new payload to amulet_nbt.

  WRITE    — a scratch copy of the golden world. A chunk's SNBT comes
             from `mcme region chunk`, is edited through amulet's own
             tags (one field changed, one added), and goes back
             through `mcme region chunk set`. amulet_nbt.load of the
             payload the sector table now points at must equal the
             edited document, deep-equal, and the slot's timestamp
             must be minutes old, not the fixture's.

  FIDELITY — every byte of the region file outside that slot's
             location entry, its timestamp, and the bytes appended
             past the file's original end must match the original
             region file exactly: the write touches what it says it
             touches and nothing else.

  ROOM     — an absent chunk gets created (the inventory counts it
             after), a region file that did not exist gets created
             with its 8 KiB header (and the inventory finds its one
             chunk at the world coordinates it was given), and every
             refusal takes its documented code: a world that is not
             Java, no region directory for that dimension, a corrupt
             region file, a coordinate that is not a number, an
             unparseable or non-compound document, a bad dimension,
             a missing argument — with nothing on stdout at any
             point, since a write verb prints nothing.

Usage:
    venv/bin/python tools/ref_region_write.py --mcme ./mcme
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
import time
import zlib

FIXTURE_WORLD = "tests/golden/worlds/java"


def _log(msg: str) -> None:
    print(f"ref_region_write: {msg}", file=sys.stderr)


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


def read_slot(path: str, cx: int, cz: int):
    """(compression_byte, compressed_blob, inflated_nbt, timestamp) for one
    slot, read from the sector table with no library involved."""
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
    stamp_at = 4096 + slot * 4
    (timestamp,) = struct.unpack(">I", data[stamp_at:stamp_at + 4])
    if comp == 1:
        raw = gzip.decompress(blob)
    elif comp == 2:
        raw = zlib.decompress(blob)
    elif comp == 3:
        raw = blob
    else:
        raise AssertionError(f"{path}: compression byte {comp} is not handled")
    return comp, blob, raw, timestamp


def main() -> int:
    ap = argparse.ArgumentParser(
        description="mcme region chunk set and amulet_nbt agree on what was written"
    )
    ap.add_argument("--mcme", required=True, help="path to mcme")
    ap.add_argument(
        "--world",
        default=FIXTURE_WORLD,
        help="golden Java world; only copies of it are ever written",
    )
    args = ap.parse_args()

    import amulet_nbt

    checks = 0

    def run(*argv: str):
        r = subprocess.run([args.mcme, *argv], capture_output=True, text=True)
        return r.returncode, r.stdout.strip(), r.stderr.strip()

    tmp = tempfile.mkdtemp(prefix="mcme_region_write_ref_")
    try:
        world = os.path.join(tmp, "world")
        shutil.copytree(args.world, world)
        region_path = os.path.join(world, "region", "r.0.0.mca")
        original_path = os.path.join(args.world, "region", "r.0.0.mca")

        # WRITE — edit a real chunk through amulet's tags and send it back.
        rc, snbt0, err = run("region", "chunk", world, "0", "0")
        if rc != 0:
            raise AssertionError(f"read (0, 0): exit {rc} ({err})")
        doc = amulet_nbt.from_snbt(snbt0)
        doc["InhabitedTime"] = amulet_nbt.LongTag(12345)
        doc["ref_written"] = amulet_nbt.StringTag("written")
        edited_snbt = doc.to_snbt(indent=None)
        want = plain(doc)

        before = time.time()
        rc, out, err = run("region", "chunk", "set", world, "0", "0",
                           edited_snbt)
        if rc != 0:
            raise AssertionError(f"set (0, 0): exit {rc} ({err})")
        if out != "":
            raise AssertionError(f"set (0, 0) printed {out!r}, want nothing")

        comp, blob, raw, timestamp = read_slot(region_path, 0, 0)
        if comp != 2:
            raise AssertionError(f"written payload compression byte {comp}")
        got = plain(amulet_nbt.load(raw).tag)
        d = first_diff(got, want)
        if d is not None:
            raise AssertionError(f"amulet_nbt reads back {d}")
        if not (before - 60 <= timestamp <= time.time() + 60):
            raise AssertionError(
                f"timestamp {timestamp} is not this write's ({int(before)})")
        checks += 1
        _log("(0, 0): edited document round-trips through the region file")

        # FIDELITY — outside the slot's entry, its timestamp, and the
        # appended tail, the file is the original, bit for bit.
        with open(original_path, "rb") as fh:
            orig = fh.read()
        with open(region_path, "rb") as fh:
            new = fh.read()
        if len(new) <= len(orig):
            raise AssertionError(
                f"file is {len(new)} bytes, no longer than the original")
        if new[4:4096] != orig[4:4096]:
            raise AssertionError("location table outside slot 0 changed")
        if new[4100:len(orig)] != orig[4100:]:
            raise AssertionError(
                "bytes past the timestamp entry changed: the write "
                "rewrote more than its slot")
        checks += 1
        _log("every byte outside the slot's entry, timestamp and tail "
             "is the original file")

        # The inventory reports the payload this tool decompressed,
        # and the old chunks' entries are still the old entries.
        rc, inv_json, err = run("--json", "region", world, "0", "0")
        if rc != 0:
            raise AssertionError(f"inventory: exit {rc} ({err})")
        inv = json.loads(inv_json)
        if inv["chunks"] != 4:
            raise AssertionError(f"inventory counts {inv['chunks']}, want 4")
        row = next(e for e in inv["entries"] if e["x"] == 0 and e["z"] == 0)
        if row["bytes"] != len(blob) or row["compression"] != "zlib":
            raise AssertionError(
                f"inventory row {row}, want zlib {len(blob)} bytes")
        if row["timestamp"] != timestamp:
            raise AssertionError("inventory timestamp differs from the file")
        checks += 1
        _log("inventory reports exactly the payload the file holds")

        # ROOM — an absent chunk gets created.
        rc, out, err = run("region", "chunk", "set", world, "2", "2",
                           "{ref_created: 1L}")
        if rc != 0 or out != "":
            raise AssertionError(f"create (2, 2): exit {rc} stdout {out!r}")
        rc, back, err = run("region", "chunk", world, "2", "2")
        if rc != 0 or plain(amulet_nbt.from_snbt(back)) != \
                plain(amulet_nbt.from_snbt("{ref_created: 1L}")):
            raise AssertionError(
                f"created chunk reads back {back!r} exit {rc} ({err})")
        rc, inv_json, err = run("--json", "region", world, "0", "0")
        if rc != 0 or json.loads(inv_json)["chunks"] != 5:
            raise AssertionError(f"inventory after create: exit {rc}")
        checks += 1
        _log("an absent chunk is created, and the inventory counts it")

        # ROOM — a region file that did not exist gets created with its
        # header: chunk (900, 0) lives in r.28.0, sector 2 of a fresh
        # file — so the header is zeros except that one slot's entry,
        # which points at sector 2 of one sector.
        before_fresh = time.time()
        rc, out, err = run("region", "chunk", "set", world, "900", "0",
                           "{ref_fresh: 2L}")
        if rc != 0 or out != "":
            raise AssertionError(f"create file: exit {rc} stdout {out!r}")
        fresh_path = os.path.join(world, "region", "r.28.0.mca")
        with open(fresh_path, "rb") as fh:
            head = fh.read(8192)
        want_header = bytearray(8192)
        want_header[16:20] = (2).to_bytes(3, "big") + bytes([1])
        stamp_at = 4096 + 16
        want_header[stamp_at:stamp_at + 4] = head[stamp_at:stamp_at + 4]
        if head != bytes(want_header):
            raise AssertionError("fresh region file header is not zeros plus "
                                 "slot 4's entry at sector 2")
        comp, blob, raw, timestamp = read_slot(fresh_path, 900, 0)
        if not (before_fresh - 60 <= timestamp <= time.time() + 60):
            raise AssertionError(
                f"fresh file timestamp {timestamp} is not this write's")
        if plain(amulet_nbt.load(raw).tag) != \
                plain(amulet_nbt.from_snbt("{ref_fresh: 2L}")):
            raise AssertionError("fresh file payload does not read back")
        rc, inv_json, err = run("--json", "region", world, "28", "0")
        if rc != 0:
            raise AssertionError(f"inventory of fresh file: exit {rc} ({err})")
        inv = json.loads(inv_json)
        if inv["chunks"] != 1 or inv["entries"][0]["x"] != 900:
            raise AssertionError(f"fresh inventory {inv}")
        checks += 1
        _log("a region file that did not exist is created with its header, "
             "chunk at the world coordinates given")

        # ROOM — a world with no region directory: the store is what must
        # exist, not just the level.
        bare = os.path.join(tmp, "bare")
        shutil.copytree(args.world, bare)
        shutil.rmtree(os.path.join(bare, "region"))
        rc, out, err = run("region", "chunk", "set", bare, "0", "0", "{a: 1}")
        if rc != 2 or "no region directory" not in err:
            raise AssertionError(f"no region dir: exit {rc} ({err})")
        checks += 1
        _log("a world with no region directory is exit 2")

        # ROOM — the wrong world, the wrong file: both exit 2, and the
        # corrupt file is exit 3 because it exists and will not read.
        bedrock = os.path.join(os.path.dirname(args.world.rstrip("/")),
                               "bedrock")
        if os.path.isdir(bedrock):
            rc, out, err = run("region", "chunk", "set", bedrock, "0", "0",
                               "{a: 1}")
            if rc != 2 or "not a Java world" not in err:
                raise AssertionError(f"bedrock world: exit {rc} ({err})")
        rc, out, err = run("region", "chunk", "set",
                           os.path.join(tmp, "nope"), "0", "0", "{a: 1}")
        if rc != 2:
            raise AssertionError(f"missing world: exit {rc}, want 2 ({err})")
        with open(os.path.join(world, "region", "r.9.9.mca"), "wb") as fh:
            fh.write(b"junk")
        rc, out, err = run("region", "chunk", "set", world, "288", "288",
                           "{a: 1}")
        if rc != 3 or "shorter than the 8192-byte header" not in err:
            raise AssertionError(f"corrupt region: exit {rc} ({err})")
        checks += 1
        _log("wrong world or missing world is exit 2, a corrupt region "
             "file is exit 3")

        # ROOM — dimensions: nether and end look in DIM-1 and DIM1 (the
        # fixture has neither, so no directory), anything else is usage.
        for dimension, directory in (("nether", "DIM-1"), ("end", "DIM1")):
            rc, out, err = run("region", "chunk", "set", world, "0", "0",
                               "{a: 1}", "--dimension", dimension)
            if rc != 2 or directory not in err:
                raise AssertionError(
                    f"--dimension {dimension}: exit {rc} ({err})")
        rc, out, err = run("region", "chunk", "set", world, "0", "0",
                           "{a: 1}", "--dimension", "7")
        if rc != 1 or "no directory for dimension 7" not in err:
            raise AssertionError(f"--dimension 7: exit {rc} ({err})")
        checks += 1
        _log("--dimension nether and end look in DIM-1/ and DIM1/, "
             "a dimension Java has no directory for is usage")

        # ROOM — usage: coordinates, documents, and the argument list.
        rc, out, err = run("region", "chunk", "set", world, "x", "0", "{a: 1}")
        if rc != 1:
            raise AssertionError(f"bad coordinate: exit {rc}, want 1")
        rc, out, err = run("region", "chunk", "set", world, "0", "0",
                           "{DataVersion: ")
        if rc != 1:
            raise AssertionError(f"unparseable document: exit {rc}, want 1")
        rc, out, err = run("region", "chunk", "set", world, "0", "0", "[1, 2]")
        if rc != 1 or "rooted in a compound" not in err:
            raise AssertionError(f"list root: exit {rc} ({err})")
        rc, out, err = run("region", "chunk", "set", world, "0", "0")
        if rc != 1:
            raise AssertionError(f"missing argument: exit {rc}, want 1")
        rc, out, err = run("--json", "region", "chunk", "set", world,
                           "4", "4", "{DataVersion: ")
        if rc != 1 or out != "":
            raise AssertionError(
                f"--json usage fault: exit {rc} stdout {out!r}")
        checks += 1
        _log("bad coordinates, bad documents and missing arguments are "
             "usage, silent under --json")

        _log(f"ok — {checks} checks")

    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except AssertionError as exc:
        _log(f"FAIL: {exc}")
        sys.exit(1)
