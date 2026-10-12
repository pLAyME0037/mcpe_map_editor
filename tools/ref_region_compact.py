#!/usr/bin/env python3
"""
Cross-check: `mcme region compact` packs a region file the reference
reads back identically.

The C selftest pins compaction against its own snapshots. This tool
proves the packed file against the REFERENCE: it walks the sector
table itself, keeps every payload's bytes, and hands them to
amulet_nbt.

  PACK       — a scratch copy of the golden world with slack made the
               honest way, two writes to one chunk (the first write's
               payload is dead the moment the second lands). After
               compaction the walk finds the same chunks with the same
               bytes, compression and timestamps; the counts are each
               payload's own sector count; the offsets run
               contiguously from sector 2 in slot order; the file is
               exactly the header plus those sectors; the dead payload
               is not in the file anymore; and amulet_nbt loads every
               payload the new table points at.

  INVENTORY  — `mcme region --json` reports the compacted file's own
               size, chunk count, payload sizes and timestamps: the
               read verbs see what the rewrite wrote.

  IDEMPOTENT — compacting the compacted file changes no byte.

  ROOM       — a region file that does not exist is exit 2, a corrupt
               one exit 3, a chunk whose frame will not read exit 3 (a
               compactor that skipped what it could not parse would
               drop it), a chunk too big for a slot's count byte exit 3,
               Bedrock and bad coordinates take their documented codes —
               and the verb prints nothing on any path, since what it
               did is the inventory's next listing.

Usage:
    venv/bin/python tools/ref_region_compact.py --mcme ./mcme
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
FIXTURE_BEDROCK = "tests/golden/worlds/bedrock"


def _log(msg: str) -> None:
    print(f"ref_region_compact: {msg}", file=sys.stderr)


def plain(value):
    """Anything amulet's own decode gives, as plain Python."""
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


def walk_region(path: str):
    """Every slot of a region file, read from the sector table with no
    library involved: {slot: (offset, count, comp, blob, raw, stamp)}
    for the present ones, plus the file's size. Every payload is
    decompressed here, so the walk itself proves each one reads."""
    with open(path, "rb") as fh:
        data = fh.read()
    if len(data) < 8192:
        raise AssertionError(f"{path}: {len(data)} bytes, no 8 KiB header")
    slots = {}
    for slot in range(1024):
        sector_offset = int.from_bytes(data[slot * 4:slot * 4 + 3], "big")
        if sector_offset == 0:
            continue
        count = data[slot * 4 + 3]
        at = sector_offset * 4096
        (length,) = struct.unpack(">I", data[at:at + 4])
        comp = data[at + 4]
        blob = data[at + 5:at + 5 + length - 1]
        if comp == 1:
            raw = gzip.decompress(blob)
        elif comp == 2:
            raw = zlib.decompress(blob)
        elif comp == 3:
            raw = blob
        else:
            raise AssertionError(
                f"{path}: slot {slot} compression byte {comp}")
        (stamp,) = struct.unpack(">I", data[4096 + slot * 4:4100 + slot * 4])
        slots[slot] = (sector_offset, count, comp, blob, raw, stamp)
    return slots, len(data)


def main() -> int:
    ap = argparse.ArgumentParser(
        description="mcme region compact and the reference's own walk "
                    "agree on the packed file"
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

    tmp = tempfile.mkdtemp(prefix="mcme_region_compact_ref_")
    try:
        world = os.path.join(tmp, "world")
        shutil.copytree(args.world, world)
        region_path = os.path.join(world, "region", "r.0.0.mca")

        # PACK — slack made the honest way: two writes to one chunk.
        rc, snbt, err = run("region", "chunk", world, "0", "0")
        if rc != 0:
            raise AssertionError(f"read (0, 0): exit {rc} ({err})")
        doc1 = amulet_nbt.from_snbt(snbt)
        doc1["ref_pass1"] = amulet_nbt.StringTag("first")
        rc, out, err = run("region", "chunk", "set", world, "0", "0",
                           doc1.to_snbt(indent=None))
        if rc != 0 or out != "":
            raise AssertionError(f"set #1 (0, 0): exit {rc} stdout {out!r}")
        dead_blob = walk_region(region_path)[0][0][3]

        doc2 = amulet_nbt.from_snbt(doc1.to_snbt(indent=None))
        doc2["ref_pass1"] = amulet_nbt.StringTag("second")
        doc2["ref_pass2"] = amulet_nbt.LongTag(7)
        before = time.time()
        rc, out, err = run("region", "chunk", "set", world, "0", "0",
                           doc2.to_snbt(indent=None))
        if rc != 0 or out != "":
            raise AssertionError(f"set #2 (0, 0): exit {rc} stdout {out!r}")

        pre, pre_size = walk_region(region_path)
        if 0 not in pre:
            raise AssertionError("the two writes did not leave a chunk")

        rc, out, err = run("region", "compact", world, "0", "0")
        if rc != 0:
            raise AssertionError(f"compact: exit {rc} ({err})")
        if out != "":
            raise AssertionError(f"compact printed {out!r}, want nothing")
        if os.path.exists(region_path + ".tmp"):
            raise AssertionError("the .tmp file the compact writes "
                                 "through is still there")
        checks += 1
        _log("compact ran silent and left no .tmp file behind")

        post, post_size = walk_region(region_path)
        if set(post) != set(pre):
            raise AssertionError(
                f"slots {sorted(post)}, want the pre-compact {sorted(pre)}")
        for slot in pre:
            off_p, cnt_p, comp_p, blob_p, raw_p, stamp_p = pre[slot]
            off_q, cnt_q, comp_q, blob_q, raw_q, stamp_q = post[slot]
            if (comp_q, blob_q, stamp_q) != (comp_p, blob_p, stamp_p):
                raise AssertionError(
                    f"slot {slot}: frame or stamp changed under compaction")
        if not before - 60 <= post[0][5] <= time.time() + 60:
            raise AssertionError(
                f"slot 0's stamp {post[0][5]} is not the write this "
                f"session made ({int(before)})")
        checks += 1
        _log("the same chunks, byte-identical frames and stamps")

        # The counts are the payloads' own, the offsets contiguous in
        # slot order from sector 2, the size exactly what those
        # sectors add up to.
        at = 2
        for slot in sorted(post):
            off, count, comp, blob, raw, stamp = post[slot]
            want_count = (5 + len(blob) + 4095) // 4096
            if count != want_count:
                raise AssertionError(
                    f"slot {slot}: claims {count} sectors, its frame "
                    f"needs {want_count}")
            if off != at:
                raise AssertionError(
                    f"slot {slot}: at sector {off}, packing puts it "
                    f"at {at}")
            at += count
        if post_size != at * 4096:
            raise AssertionError(
                f"file is {post_size} bytes, header plus packed "
                f"sectors is {at * 4096}")
        checks += 1
        _log("counts are the payloads' own, offsets contiguous from "
             "sector 2, size exact")

        # The dead payload is gone and the file is smaller.
        with open(region_path, "rb") as fh:
            packed = fh.read()
        if dead_blob in packed:
            raise AssertionError(
                "the superseded payload's bytes are still in the file")
        if not post_size < pre_size:
            raise AssertionError(
                f"file is {post_size} bytes, no smaller than the "
                f"{pre_size} it was")
        checks += 1
        _log("the superseded payload is gone and the file shrank")

        # amulet_nbt loads every payload the new table points at, and
        # reads what the old file's bytes said.
        for slot in sorted(post):
            got = plain(amulet_nbt.load(post[slot][4]).tag)
            want = plain(amulet_nbt.load(pre[slot][4]).tag)
            if got != want:
                raise AssertionError(f"slot {slot}: amulet_nbt reads "
                                     "different documents")
        checks += 1
        _log("amulet_nbt loads every compacted payload")

        # INVENTORY — the read verbs see the rewritten file.
        rc, inv_json, err = run("--json", "region", world, "0", "0")
        if rc != 0:
            raise AssertionError(f"inventory: exit {rc} ({err})")
        inv = json.loads(inv_json)
        if inv["chunks"] != len(post) or inv["bytes"] != post_size:
            raise AssertionError(
                f"inventory says {inv['chunks']} chunks, "
                f"{inv['bytes']} bytes; the file is {len(post)} "
                f"chunks, {post_size} bytes")
        for row in inv["entries"]:
            slot = (row["x"] & 31) + (row["z"] & 31) * 32
            off, count, comp, blob, raw, stamp = post[slot]
            if row["bytes"] != len(blob) or row["timestamp"] != stamp:
                raise AssertionError(
                    f"inventory row for chunk ({row['x']}, {row['z']}), "
                    "file disagrees")
        checks += 1
        _log("the inventory reports the compacted file's own numbers")

        # IDEMPOTENT — compacting the compacted file changes no byte.
        rc, out, err = run("region", "compact", world, "0", "0")
        if rc != 0 or out != "":
            raise AssertionError(f"compact again: exit {rc} stdout {out!r}")
        with open(region_path, "rb") as fh:
            twice = fh.read()
        if twice != packed:
            raise AssertionError(
                f"the second compaction changed {len(twice)} bytes")
        checks += 1
        _log("a second compaction changes no byte")

        # ROOM — nothing there is not something broken.
        rc, out, err = run("region", "compact", world, "9", "9")
        if rc != 2 or out != "":
            raise AssertionError(
                f"missing region file: exit {rc} stdout {out!r}, want 2")
        checks += 1
        _log("a region file that does not exist is exit 2, silent")

        junk_path = os.path.join(world, "region", "r.7.7.mca")
        with open(junk_path, "wb") as fh:
            fh.write(b"junkjunk")
        rc, out, err = run("region", "compact", world, "7", "7")
        if rc != 3 or out != "":
            raise AssertionError(
                f"corrupt region file: exit {rc} stdout {out!r}, want 3")
        os.remove(junk_path)
        checks += 1
        _log("a corrupt region file is exit 3, silent")

        # A slot pointing at a payload header that will not read —
        # its length runs past the file — refuses the whole file.
        bad_path = os.path.join(world, "region", "r.6.6.mca")
        with open(bad_path, "wb") as fh:
            fh.write(bytes(8192 + 4096))
        with open(bad_path, "r+b") as fh:
            fh.seek(0)
            fh.write(bytes([0, 0, 2, 1]))       # slot 0: sector 2, count 1
            fh.seek(8192)
            fh.write(struct.pack(">I", 0x00100000))  # 1 MiB, past EOF
            fh.write(bytes([2]))
        rc, out, err = run("region", "compact", world, "6", "6")
        if rc != 3 or out != "":
            raise AssertionError(
                f"unreadable frame: exit {rc} stdout {out!r}, want 3")
        os.remove(bad_path)
        checks += 1
        _log("a slot whose frame will not read refuses the whole file")

        # A payload bigger than the 255 sectors a slot's count byte
        # can claim: mcme reads it, but a rewrite could not record it
        # honestly, so the file is refused.
        big_path = os.path.join(world, "region", "r.5.5.mca")
        length = 255 * 4096
        with open(big_path, "wb") as fh:
            fh.write(bytes(8192))
            fh.seek(0)
            fh.write(bytes([0, 0, 2, 255]))     # slot 0: sector 2, count 255
            fh.seek(8192)
            fh.write(struct.pack(">I", length))
            fh.write(bytes([2]))
            fh.write(bytes(length - 1))
        rc, out, err = run("region", "compact", world, "5", "5")
        if rc != 3 or out != "" or "a slot can claim 255" not in err:
            raise AssertionError(
                f"oversized chunk: exit {rc} stdout {out!r} "
                f"stderr {err!r}, want 3 and the 255-sector message")
        os.remove(big_path)
        checks += 1
        _log("a chunk too big for a slot to claim is exit 3 with "
             "the reason")

        # Usage, dimensions and Bedrock take their documented codes,
        # on text and --json alike, with nothing printed.
        for argv, want_rc in (
            (("region", "compact", world, "x", "0"), 1),
            (("region", "compact", world, "0"), 1),
            (("--json", "region", "compact", world, "0", "z"), 1),
            (("region", "compact", world, "0", "0", "--dimension", "7"), 1),
            (("region", "compact", world, "0", "0", "--dimension",
              "nether"), 2),
            (("region", "compact", FIXTURE_BEDROCK, "0", "0"), 2),
            (("region", "compact", os.path.join(tmp, "nope"), "0", "0"), 2),
        ):
            rc, out, err = run(*argv)
            if rc != want_rc or out != "":
                raise AssertionError(
                    f"{' '.join(argv)}: exit {rc} stdout {out!r}, "
                    f"want {want_rc} and nothing printed")
        checks += 1
        _log("usage, dimensions and Bedrock take their documented codes")

        _log(f"ok — {checks} checks")
        return 0
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())
