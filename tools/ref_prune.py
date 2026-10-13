#!/usr/bin/env python3
"""
Cross-check: `mcme chunk prune` — whole chunks deleted by a box of
columns, in both worlds (decisions.md#D13).

The C selftest pins the library against the fixtures. This tool
proves the CLI against references: Bedrock keys read back through
amulet's own LevelDB binding, Java slots read back by walking the
region file's sector table with no mcme code, and byte-for-byte
comparison for everything that must not move.

What is compared:

  BEDROCK — the fixture's eight keys, prune by prune: a box over
            chunk (0,0) with its corners reversed tombstones that
            chunk's six subchunk keys and leaves the neighbour at
            (1,-2) and the nether key standing; the same box again
            is a no-op with nothing printed; the column at (1,-2)
            goes next; the nether key only with --dimension. The
            counts mcme itself reports agree before and after
            (`mcme chunks`), and every pruned key raises KeyError
            in amulet's reader while the survivors return bytes.

  JAVA    — r.0.0.mca's sector table: a box at y 300 (Y is parsed
            and ignored) zeroes chunk (0,0)'s slot and leaves
            (1,0), (0,1) and (31,31); r.-1.-1.mca stays
            byte-identical; `mcme region compact` then reclaims
            the punched payload — the file shrinks, the survivors'
            frames stay byte-identical, and the hole stays a hole;
            a column with no chunk is a no-op; `mcme chunk get`
            answers 2 for the pruned position.

  REPORT  — the JSON counts, exact: {"chunks":…,"keys":…,
            "region_files":…}, and human success prints nothing.

  EXITS   — a junk coordinate 1, a path that is not a world 2, a
            directory that is not a world 2, a dimension number
            Java has no directory for 1, a dimension the world has
            not made 2, a region file that will not read 3 with
            its bytes unchanged, and the LOCK the game holds 4.

The fixtures are COPIED before they are opened — a LevelDB open
compacts, and the goldens are never written.

Usage:
    venv/bin/python tools/ref_prune.py --mcme ./mcme
"""

from __future__ import annotations

import argparse
import fcntl
import json
import os
import shutil
import struct
import subprocess
import sys
import tempfile

from ref_java_fill import walk_chunk
from ref_bedrock_column import chunk_key

FIXTURE_BEDROCK = "tests/golden/worlds/bedrock-chunks"
FIXTURE_JAVA = "tests/golden/worlds/java"

# The fixture's eight keys: six subchunks of the chunk at (0,0), one
# at (1,-2), one nether key at (0,0) — the store's whole inventory.
CHUNK00_CYS = (-4, -1, 0, 1, 2, 4)


def _log(msg: str) -> None:
    print(f"ref_prune: {msg}", file=sys.stderr)


def run(*argv: str):
    p = subprocess.run(argv, capture_output=True, text=True)
    return p.returncode, p.stdout, p.stderr


def bytes_of(path: str) -> bytes:
    with open(path, "rb") as fh:
        return fh.read()


def frame_bytes(path: str, cx: int, cz: int):
    """The slot's raw frame — length, compression byte, data — or None
    when the slot is empty. What `region compact` promises to copy
    byte-identical."""
    data = bytes_of(path)
    slot = (cx & 31) + (cz & 31) * 32
    sector = int.from_bytes(data[slot * 4:slot * 4 + 3], "big")
    if sector == 0:
        return None
    at = sector * 4096
    (length,) = struct.unpack(">I", data[at:at + 4])
    return data[at:at + 4 + length]


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--mcme", required=True)
    args = ap.parse_args()
    mcme = args.mcme

    checks = 0
    tmp = tempfile.mkdtemp(prefix="ref_prune_")
    try:
        # ---- BEDROCK: eight keys, three chunks ---------------------
        bw = os.path.join(tmp, "bw")
        shutil.copytree(FIXTURE_BEDROCK, bw)
        db_path = os.path.join(bw, "db")

        def prune(*argv, want_json=None, want=0, silent=True):
            """Run `chunk prune` with --json, checking the exit code
            and the exact report; returns the parsed counts."""
            rc, out, err = run(mcme, "--json", "chunk", "prune", bw, *argv)
            if rc != want:
                raise AssertionError(
                    f"prune {argv}: exit {rc}: {err.strip()}")
            if want_json is None:
                return None
            got = json.loads(out)
            if got != want_json or out != json.dumps(
                    want_json, separators=(",", ":")) + "\n":
                raise AssertionError(
                    f"prune {argv}: report {out!r}, want {want_json}")
            if silent and err != "":
                raise AssertionError(
                    f"prune {argv}: stderr {err!r} on success")
            return got

        # inventory before: 8 keys, three (dimension, x, z) columns
        rc, out, _ = run(mcme, "--json", "chunks", bw)
        if rc != 0:
            raise AssertionError(f"chunks: exit {rc}")
        inv = json.loads(out)
        if inv["keys"] != 8 or inv["chunks"] != 3:
            raise AssertionError(f"fixture inventory {inv}")
        checks += 1
        _log("ok — the fixture starts with 8 keys in 3 chunks")

        # corners reversed, y far above the ground: one chunk, six
        # keys, no other world unit invented
        prune("15", "7", "15", "0", "0", "0",
              want_json={"chunks": 1, "keys": 6, "region_files": 0})

        # amulet's own reader: the six keys raise KeyError, the
        # neighbour and the other dimension return bytes
        from amulet.libs.leveldb import LevelDB
        db = LevelDB(db_path)
        try:
            for cy in CHUNK00_CYS:
                try:
                    db.get(chunk_key(0, 0, cy))
                except KeyError:
                    pass
                else:
                    raise AssertionError(
                        f"chunk (0,0) cy {cy} still readable")
            if not db.get(chunk_key(1, -2, 0)):
                raise AssertionError("neighbour key lost")
            if not db.get(chunk_key(0, 0, 0, 1)):
                raise AssertionError("nether key lost")
        finally:
            db.close()
        checks += 1
        _log("ok — the pruned chunk's six keys are gone; the "
             "neighbour and the nether key stand (amulet LevelDB)")

        # the same box again: human silence, then the exact no-op
        rc, out, err = run(mcme, "chunk", "prune", bw,
                           "0", "0", "0", "15", "0", "15")
        if rc != 0 or out != "" or err != "":
            raise AssertionError(
                f"repeat prune: exit {rc} out {out!r} err {err!r}")
        prune("0", "0", "0", "15", "0", "15",
              want_json={"chunks": 0, "keys": 0, "region_files": 0})
        checks += 1
        _log("ok — the same box twice: silent success, 0 chunks")

        # the column at (1,-2): blocks 0..-32 floor to it
        prune("0", "0", "0", "31", "0", "-32",
              want_json={"chunks": 1, "keys": 1, "region_files": 0})

        # the nether key only with its own flag
        prune("--dimension", "nether", "0", "0", "0", "15", "0", "15",
              want_json={"chunks": 1, "keys": 1, "region_files": 0})

        rc, out, _ = run(mcme, "--json", "chunks", bw)
        inv = json.loads(out)
        if rc != 0 or inv["keys"] != 0 or inv["chunks"] != 0:
            raise AssertionError(f"after prune: {inv}")
        checks += 1
        _log("ok — every prune's report matches the store's own "
             "inventory, which ends at 0 keys")

        # the LOCK the game holds: refused before anything is written
        lock_fh = open(os.path.join(db_path, "LOCK"), "w")
        try:
            fcntl.flock(lock_fh, fcntl.LOCK_EX)
            rc, out, err = run(mcme, "--json", "chunk", "prune", bw,
                               "0", "0", "0", "15", "0", "15")
        finally:
            lock_fh.close()
        if rc != 4 or out != "":
            raise AssertionError(
                f"held LOCK: exit {rc} out {out!r}, want 4 and no "
                f"report ({err.strip()})")
        checks += 1
        _log("ok — the LOCK the game holds refuses the prune, exit 4")

        # ---- exits ------------------------------------------------
        rc, out, _ = run(mcme, "chunk", "prune", bw,
                         "0", "junk", "0", "15", "0", "15")
        if rc != 1 or out != "":
            raise AssertionError(f"junk coordinate: exit {rc}")
        rc, out, _ = run(mcme, "chunk", "prune",
                         os.path.join(tmp, "nowhere"),
                         "0", "0", "0", "15", "0", "15")
        if rc != 2 or out != "":
            raise AssertionError(f"missing world: exit {rc}")
        not_a_world = os.path.join(tmp, "plaindir")
        os.mkdir(not_a_world)
        rc, out, _ = run(mcme, "chunk", "prune", not_a_world,
                         "0", "0", "0", "15", "0", "15")
        if rc != 2 or out != "":
            raise AssertionError(f"not a world: exit {rc}")
        checks += 1
        _log("ok — the refusals answer as documented: junk coordinate "
             "1, missing path 2, plain directory 2")

        # ---- JAVA: the slot is zeroed ------------------------------
        jw = os.path.join(tmp, "jw")
        shutil.copytree(FIXTURE_JAVA, jw)
        region = os.path.join(jw, "region", "r.0.0.mca")
        nether = os.path.join(jw, "region", "r.-1.-1.mca")
        region_before = bytes_of(region)
        nether_before = bytes_of(nether)
        survivors = [(1, 0), (0, 1), (31, 31)]

        for cx, cz in [(0, 0)] + survivors:
            if walk_chunk(region, cx, cz) is None:
                raise AssertionError(
                    f"fixture: chunk ({cx}, {cz}) not readable")
        checks += 1
        _log("ok — the fixture's r.0.0 holds chunks (0,0), (1,0), "
             "(0,1), (31,31)")

        # y 300: Y is parsed and ignored
        rc, out, err = run(mcme, "--json", "chunk", "prune", jw,
                           "0", "300", "0", "15", "301", "15")
        want = {"chunks": 1, "keys": 0, "region_files": 1}
        if rc != 0 or json.loads(out) != want or err != "":
            raise AssertionError(
                f"java prune: exit {rc} out {out!r} err {err!r}")
        if walk_chunk(region, 0, 0) is not None:
            raise AssertionError("chunk (0,0) still readable")
        for cx, cz in survivors:
            if walk_chunk(region, cx, cz) is None:
                raise AssertionError(f"chunk ({cx}, {cz}) was punched")
        if bytes_of(nether) != nether_before:
            raise AssertionError("r.-1.-1.mca was written")
        checks += 1
        _log("ok — a box at y 300 pruned chunk (0,0) alone: the slot "
             "reads empty, the survivors and the other file untouched")

        # a column no chunk stands in: nothing at all
        rc, out, err = run(mcme, "--json", "chunk", "prune", jw,
                           "32", "0", "32", "47", "0", "47")
        want = {"chunks": 0, "keys": 0, "region_files": 0}
        if rc != 0 or json.loads(out) != want or err != "":
            raise AssertionError(
                f"empty column: exit {rc} out {out!r} err {err!r}")
        checks += 1
        _log("ok — a column with no chunk: 0 chunks, 0 files, "
             "nothing printed")

        # region compact reclaims the punched bytes: the survivors'
        # frames copy byte-identical, the hole stays a hole
        before_frames = {c: frame_bytes(region, *c) for c in survivors}
        size_before = os.path.getsize(region)
        rc, out, err = run(mcme, "region", "compact", jw, "0", "0")
        if rc != 0 or out != "" or err != "":
            raise AssertionError(
                f"compact: exit {rc} out {out!r} err {err!r}")
        if os.path.getsize(region) >= size_before:
            raise AssertionError("compact did not shrink the file")
        if walk_chunk(region, 0, 0) is not None:
            raise AssertionError("compact resurrected the hole")
        for c in survivors:
            if frame_bytes(region, *c) != before_frames[c]:
                raise AssertionError(f"compact rewrote chunk {c}")
        checks += 1
        _log("ok — region compact reclaims the bytes; the survivors' "
             "frames ride through byte-identical, the hole stays")

        # the pruned position answers like any absent chunk
        rc, out, _ = run(mcme, "chunk", "get", jw, "0", "0", "0")
        if rc != 2 or out != "":
            raise AssertionError(f"get on pruned: exit {rc}")
        checks += 1
        _log("ok — mcme chunk get answers 2 for the pruned position")

        # java exits: a number with no directory, a dimension the
        # world has not made, a junk coordinate
        rc, out, _ = run(mcme, "chunk", "prune", jw,
                         "--dimension", "7", "0", "0", "0", "15", "0",
                         "15")
        if rc != 1 or out != "":
            raise AssertionError(f"dimension 7: exit {rc}")
        rc, out, _ = run(mcme, "chunk", "prune", jw,
                         "--dimension", "nether", "0", "0", "0",
                         "15", "0", "15")
        if rc != 2 or out != "":
            raise AssertionError(f"missing DIM-1: exit {rc}")
        rc, out, _ = run(mcme, "chunk", "prune", jw,
                         "0", "nope", "0", "15", "0", "15")
        if rc != 1 or out != "":
            raise AssertionError(f"junk coordinate: exit {rc}")
        checks += 1
        _log("ok — java refusals: dimension number 1, dimension the "
             "world has not made 2, junk coordinate 1")

        # a region file that will not read refuses the whole prune
        # with nothing punched — both files' bytes exactly as they
        # were
        jw2 = os.path.join(tmp, "jw2")
        shutil.copytree(FIXTURE_JAVA, jw2)
        region2 = os.path.join(jw2, "region", "r.0.0.mca")
        nether2 = os.path.join(jw2, "region", "r.-1.-1.mca")
        with open(region2, "wb") as fh:
            fh.write(b"GARBAGE!")     # 8 bytes: shorter than the
                                      # 8 KiB header
        corrupted = bytes_of(region2)
        nether2_bytes = bytes_of(nether2)
        rc, out, err = run(mcme, "chunk", "prune", jw2,
                           "0", "0", "0", "15", "0", "15")
        if rc != 3 or out != "":
            raise AssertionError(f"corrupt region: exit {rc}")
        if bytes_of(region2) != corrupted:
            raise AssertionError("a file that will not read was written")
        if bytes_of(nether2) != nether2_bytes:
            raise AssertionError("the other file was written")
        checks += 1
        _log("ok — a region file that will not read refuses the whole "
             "prune, exit 3, both files' bytes untouched")

        _log(f"ok — {checks} checks")
        return 0
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())
