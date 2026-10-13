#!/usr/bin/env python3
"""
Cross-check: `mcme convert` — the Bedrock fixture rewritten as
java/1.20, block for block against PyMCTranslate.

The C selftest pins the library against the fixture. This tool
proves the CLI against the REFERENCE: every block of every stored
subchunk answers twice — once from PyMCTranslate's own
bedrock(1, 20, 80) -> universal -> java(1, 20, 0), once from a
walk of the output region files that reads the sector table and
unpacks the palettes itself, no mcme code.

What is compared:

  BLOCKS   — every position of every subchunk the input store
             holds, under the two documented policies: mcme keeps
             only the map's properties and lets every other state
             take the Java default, which is the reference's own
             answer for the block's empty states; and a block
             neither edition knows keeps its name and loses its
             states, where PyMCTranslate passes the states
             through untouched. The v1 legacy subchunk writes no
             section and reads back air; the waterlogged layer is
             dropped and counted. Booleans compare true/1 and
             false/0 alike — the two editions spell them
             differently over the same NBT meaning.

  REPORT   — the JSON counts, exact: two chunks, seven subchunks,
             24576 blocks, and the three skips the fixture
             carries (the legacy subchunk, the nether key, the
             water layer).

  LEVEL    — level.dat: LevelName carried, DataVersion 3463, the
             seed under WorldGenSettings, the spawn under the
             Java field names (SpawnX/Y/Z, the names the probe
             reads).

  EXITS    — the documented refusals: --to banana is 1, an
             unimplemented pair is 5, an output that exists is 1
             with nothing written, a path that is not a world is
             2, a Java input is 5.

The fixture store is COPIED before it is opened — a LevelDB open
compacts, and the golden is never written.

Usage:
    venv/bin/python tools/ref_convert.py --mcme ./mcme
"""

from __future__ import annotations

import argparse
import gzip
import io
import json
import logging
import os
import shutil
import subprocess
import sys
import tempfile

import amulet_nbt
import numpy

from ref_java_fill import walk_chunk, section_for
from ref_bedrock_column import chunk_key, decode_payload

FIXTURE_CHUNKS = "tests/golden/worlds/bedrock-chunks"
FIXTURE_LEVEL = "tests/golden/worlds/bedrock/level.dat"
FIXTURE_JAVA = "tests/golden/worlds/java"
AIR = "minecraft:air"
CHUNKS = ((0, 0), (1, -2))

TRUEISH = {True, "true", 1, "1"}
FALSEISH = {False, "false", 0, "0"}

WANT_REPORT = {
    "from": "bedrock",
    "to": "java/1.20",
    "chunks": 2,
    "subchunks": 7,
    "blocks": 24576,
    "skipped": {
        "block_entities": 0,
        "chunk_data": 0,
        "entities": 0,
        "legacy_subchunks": 1,
        "other": 1,
        "water_layers": 1,
    },
}


def _log(msg: str) -> None:
    print(f"ref_convert: {msg}", file=sys.stderr)


def run(*argv: str):
    p = subprocess.run(argv, capture_output=True, text=True)
    return p.returncode, p.stdout, p.stderr


def py(v):
    return v.py_data if hasattr(v, "py_data") else v


class UnknownBlocks(logging.Handler):
    """PyMCTranslate warns when it has no translation for a block
    and passes it through. Those blocks are mcme's documented
    unknown-block case: the name survives, the states do not."""

    def __init__(self) -> None:
        super().__init__()
        self.hits = 0

    def emit(self, record: logging.LogRecord) -> None:
        if "Could not find translation information" in record.getMessage():
            self.hits += 1


class Reference:
    """The reference's own answers: bedrock to universal to java."""

    def __init__(self) -> None:
        import PyMCTranslate
        from amulet.api.block import Block

        self._Block = Block
        self._unknown = UnknownBlocks()
        logging.getLogger().addHandler(self._unknown)
        tm = PyMCTranslate.new_translation_manager()
        self._bed = tm.get_version("bedrock", (1, 20, 80))
        self._java = tm.get_version("java", (1, 20, 0))
        self._cache: dict = {}
        self._defaults: dict = {}

    def _translate(self, name: str, states: dict):
        ns, base = name.split(":", 1)
        block = self._Block(ns, base, dict(states))
        start = self._unknown.hits
        universal, _, _ = self._bed.block.to_universal(block)
        java, _, _ = self._java.block.from_universal(universal)
        unknown = self._unknown.hits > start
        if not isinstance(java, self._Block):
            raise AssertionError(
                f"{name}: the reference answered with an entity")
        out_name = f"{java.namespace}:{java.base_name}"
        props = ({str(k): py(v) for k, v in java.properties.items()}
                 if java.properties else {})
        return out_name, props, unknown

    def answer(self, name: str, states: dict | None):
        """(java name, properties, default properties) for a bedrock
        block, canonically spelled. An unknown block loses its
        states on mcme's side of the comparison, so the reference's
        pass-through states are set aside."""
        if states is None:      # legacy: no Java spelling at all
            return AIR, {}, {}
        key = (name, repr(sorted((k, py(v)) for k, v in states.items())))
        if key not in self._cache:
            out_name, props, unknown = self._translate(name, states)
            if unknown:
                self._cache[key] = (out_name, {}, {})
            else:
                dkey = (name, ())
                if dkey not in self._defaults:
                    _, dprops, _ = self._translate(name, {})
                    self._defaults[dkey] = canon(dprops)
                self._cache[key] = (out_name, canon(props),
                                    self._defaults[dkey])
        return self._cache[key]


def canon(props: dict) -> dict:
    """One spelling for values both editions write differently:
    true/1/true and 0/false/0 are the same NBT meaning."""
    out = {}
    for k, v in props.items():
        v = py(v)
        if v in TRUEISH:
            out[k] = "true"
        elif v in FALSEISH:
            out[k] = "false"
        else:
            out[k] = str(v)
    return out


def props_match(got: dict, want: dict, default: dict):
    """The two sides agree when every property agrees, a property
    only the reference has is the Java default (mcme left it where
    the format puts an absent property), and a property only mcme
    has is the default the same way."""
    for k in set(got) | set(want):
        g, w, d = got.get(k), want.get(k), default.get(k)
        if g == w:
            continue
        if g is None:
            if w == d or w == "false":
                continue
            return False, k
        if w is None:
            if g == d or g == "false":
                continue
            return False, k
        return False, k
    return True, None


def read_block(tag, x: int, y: int, z: int):
    """The block at a position of the output chunk: the walk's own
    palette unpacking, returned as name and raw properties."""
    sec = section_for(tag, y)
    if sec is None:
        return AIR, {}
    bs = sec.get("block_states")
    if bs is None:
        return AIR, {}
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
    if props is None:
        return name, {}
    return name, {str(k): py(v) for k, v in props.items()}


def entry_of(entry):
    """(name, states) of a Bedrock palette entry; states is None
    for a legacy entry, which has no version to carry."""
    comp = entry.tag if hasattr(entry, "tag") else entry.value
    name = str(py(comp["name"]))
    if "states" in comp:
        return name, dict(comp["states"])
    return name, None


def load_input(world: str):
    """The fixture's stored subchunks, read from the COPIED store:
    (cx, cz, cy) -> (kind, layer) where kind is 'ok' or 'legacy'
    and layer is (blocks, palette) for the first layer only — the
    waterlogged second layer is what the report counts and drops."""
    from amulet.libs.leveldb import LevelDB
    from amulet.level.formats.leveldb_world.interface.chunk.leveldb_8 import (
        LevelDB8Interface,
    )

    iface = LevelDB8Interface()
    db = LevelDB(os.path.join(world, "db"))
    subs = {}
    try:
        for cx, cz in CHUNKS:
            for cy in range(-5, 6):
                try:
                    payload = db.get(chunk_key(cx, cz, cy))
                except KeyError:
                    continue
                version, layers = decode_payload(iface, payload)
                if version == 1:
                    subs[(cx, cz, cy)] = ("legacy", None)
                else:
                    subs[(cx, cz, cy)] = ("ok", layers[0])
    finally:
        db.close()
    return subs


def load_level(path: str, little_endian: bool, skip_header: bool):
    raw = open(path, "rb").read()
    if skip_header:
        raw = raw[8:]
    if not little_endian:
        raw = gzip.decompress(raw)
    root = amulet_nbt.load(io.BytesIO(raw), little_endian=little_endian)
    data = root.get("Data")
    if data is None:
        data = root
    return data


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--mcme", required=True)
    args = ap.parse_args()
    mcme = args.mcme

    checks = 0
    tmp = tempfile.mkdtemp(prefix="ref_convert_")
    try:
        # The golden store is copied whole; the copy is what gets
        # opened, because a LevelDB open compacts what it touches.
        in_world = os.path.join(tmp, "in")
        shutil.copytree(FIXTURE_CHUNKS, in_world)
        shutil.copy(FIXTURE_LEVEL, os.path.join(in_world, "level.dat"))
        out = os.path.join(tmp, "out")

        # ---- the run, and the counts it prints --------------------
        rc, stdout, stderr = run(mcme, "--json", "convert", in_world,
                                 out, "--to", "java/1.20")
        if rc != 0:
            raise AssertionError(
                f"convert: exit {rc} ({stderr.strip()})")
        report = json.loads(stdout)
        if report != WANT_REPORT:
            raise AssertionError(
                f"report is {report}, want {WANT_REPORT}")
        checks += 1
        _log("ok — the conversion runs and the report counts every "
             "chunk, subchunk and skip exactly")

        # ---- blocks: the reference and the walk, position by
        # position ------------------------------------------------
        subs = load_input(in_world)
        ref = Reference()
        mismatches = []
        compared = 0
        for cx, cz in CHUNKS:
            region = os.path.join(
                out, "region", f"r.{cx >> 5}.{cz >> 5}.mca")
            tag = walk_chunk(region, cx, cz)
            if tag is None:
                raise AssertionError(f"no output chunk ({cx}, {cz})")
            ok_cys = sorted(cy for (a, b, cy), (kind, _) in subs.items()
                            if (a, b) == (cx, cz) and kind == "ok")
            legacy_cys = sorted(
                cy for (a, b, cy), (kind, _) in subs.items()
                if (a, b) == (cx, cz) and kind == "legacy")
            out_ys = sorted(s["Y"].py_data for s in tag["sections"])
            if out_ys != ok_cys:
                raise AssertionError(
                    f"chunk ({cx}, {cz}) stores sections {out_ys}, "
                    f"want the stored non-legacy subchunks {ok_cys}")

            for cy in ok_cys + legacy_cys:
                kind, layer = subs[(cx, cz, cy)]
                if kind == "legacy":
                    # A legacy subchunk writes no section — the
                    # sections list already proved it — and an
                    # absent section reads back air. The selftest
                    # pins mcme's own reader to the same answer.
                    continue
                blocks, palette = layer
                arr = numpy.asarray(blocks).reshape(16, 16, 16)
                for lx in range(16):
                    for ly in range(16):
                        for lz in range(16):
                            name, states = entry_of(
                                palette[int(arr[lx][ly][lz])])
                            want_name, want, default = ref.answer(
                                name, states)
                            x, y, z = (cx * 16 + lx, cy * 16 + ly,
                                       cz * 16 + lz)
                            got_name, got_props = read_block(tag, x, y, z)
                            got = canon(got_props)
                            compared += 1
                            if got_name != want_name:
                                mismatches.append(
                                    (x, y, z, f"name {got_name} != "
                                     f"{want_name}"))
                            else:
                                ok, key = props_match(got, want, default)
                                if not ok:
                                    mismatches.append(
                                        (x, y, z,
                                         f"prop {key}: {got} != {want} "
                                         f"(default {default})"))
        if mismatches:
            shown = "; ".join(f"({x}, {y}, {z}) {why}"
                              for x, y, z, why in mismatches[:8])
            raise AssertionError(
                f"{len(mismatches)} of {compared} positions disagree "
                f"with the reference: {shown}")
        if compared != WANT_REPORT["blocks"]:
            raise AssertionError(
                f"compared {compared} positions, the report counted "
                f"{WANT_REPORT['blocks']} blocks")
        checks += 1
        _log(f"ok — {compared} positions read back exactly as "
             f"PyMCTranslate answers them, policies included")

        # ---- level.dat: the fields that must travel ---------------
        data_out = load_level(os.path.join(out, "level.dat"), False,
                              False)
        data_in = load_level(os.path.join(in_world, "level.dat"), True,
                             True)
        if data_out.get("LevelName").py_data != data_in["LevelName"].py_data:
            raise AssertionError(
                f"LevelName {data_out.get('LevelName')} != "
                f"{data_in['LevelName']}")
        if data_out.get("DataVersion").py_int != 3463:
            raise AssertionError(
                f"DataVersion {data_out.get('DataVersion')}, want 3463")
        seed_in = data_in.get("RandomSeed")
        seed_in = seed_in.py_int if seed_in is not None else 0
        seed_out = data_out["WorldGenSettings"]["seed"].py_int
        if seed_out != seed_in:
            raise AssertionError(
                f"seed {seed_out} != the input's {seed_in}")
        for field, default in (("SpawnX", 0), ("SpawnY", 64),
                               ("SpawnZ", 0)):
            want = data_in.get(field)
            want = want.py_int if want is not None else default
            got = data_out.get(field)
            if got is None or got.py_int != want:
                raise AssertionError(
                    f"{field}: {got}, want {want} — under the names "
                    f"the probe reads")
        checks += 1
        _log("ok — level.dat carries the name, the seed under "
             "WorldGenSettings, the spawn under SpawnX/Y/Z, and "
             "DataVersion 3463")

        # ---- the documented refusals -------------------------------
        # a --to that names no edition: usage
        rc, stdout, stderr = run(mcme, "convert", in_world,
                                 os.path.join(tmp, "e1"), "--to",
                                 "banana")
        if rc != 1 or stdout != "":
            raise AssertionError(
                f"--to banana: exit {rc} stdout {stdout!r}, want 1 "
                f"and nothing printed")
        # --to java/1.21 parses and then answers not-implemented
        rc, stdout, stderr = run(mcme, "convert", in_world,
                                 os.path.join(tmp, "e2"), "--to",
                                 "java/1.21")
        if rc != 5 or stdout != "":
            raise AssertionError(
                f"--to java/1.21: exit {rc} stdout {stdout!r}, want 5")
        # an output that exists: refused whole, nothing written
        exists = os.path.join(tmp, "exists")
        os.makedirs(exists)
        rc, stdout, stderr = run(mcme, "convert", in_world, exists,
                                 "--to", "java/1.20")
        if rc != 1 or stdout != "" or os.listdir(exists):
            raise AssertionError(
                f"existing output: exit {rc} stdout {stdout!r} "
                f"dir {os.listdir(exists)}, want 1 and nothing written")
        # a path that is not a world
        nowhere = os.path.join(tmp, "nowhere")
        target = os.path.join(tmp, "e4")
        rc, stdout, stderr = run(mcme, "convert", nowhere, target,
                                 "--to", "java/1.20")
        if rc != 2 or stdout != "" or os.path.exists(target):
            raise AssertionError(
                f"missing input: exit {rc} stdout {stdout!r}, want 2 "
                f"and no output")
        # a Java input: the pair is not implemented
        java_world = os.path.join(tmp, "java")
        shutil.copytree(FIXTURE_JAVA, java_world)
        target = os.path.join(tmp, "e5")
        rc, stdout, stderr = run(mcme, "convert", java_world, target,
                                 "--to", "java/1.20")
        if rc != 5 or stdout != "" or os.path.exists(target):
            raise AssertionError(
                f"java input: exit {rc} stdout {stdout!r}, want 5 and "
                f"no output")
        checks += 1
        _log("ok — the refusals answer as documented: bad --to 1, "
             "unimplemented pair 5, existing output 1, missing world "
             "2, Java input 5")

        _log(f"ok — {checks} checks")
        return 0
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())
