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
             section and reads back air; the second layer carries
             as the Java `waterlogged` property only where the
             reference gives the block that property
             (decisions.md#D15) — the fixture's water stands over
             grass, dirt and stone, none of them waterloggable, so
             every water position drops and is counted. Booleans
             compare true/1 and
             false/0 alike — the two editions spell them
             differently over the same NBT meaning.

  REPORT   — the JSON counts, exact: two chunks, seven subchunks,
             24576 blocks, no documents carried, and the skips
             the fixture carries (the legacy subchunk, the nether
             key, the 3280 water positions) — plus the four tick
             records this tool fabricates on the copy: three
             entries carried, four dropped (decisions.md#D18).

  TICKS    — the fabricated records against the output's
             fluid_ticks and block_ticks lists: every carried
             entry named by the same pinned reference, t spelled
             time − currentTick, p 0, water and lava under
             fluid_ticks, and both keys standing in every chunk
             even where they are empty. amulet reads and writes
             no tick records, so this mirror IS the spec mcme
             carries.

  ENTITIES — a second world, the block-entity fixture: the
             report's document counts, and the output's
             block_entities list against the fixture's own
             records read back from the COPIED store — id and
             coordinates, the stacks under Slot/id/Count, the
             smelter timers as ints — with the sign, the
             coordinate-less item frame and the nether's chest
             among the documents that do not arrive.

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
import re
import shutil
import subprocess
import sys
import tempfile

import amulet_nbt
import numpy

from ref_java_fill import walk_chunk, section_for
from ref_bedrock_column import chunk_key, decode_payload

FIXTURE_CHUNKS = "tests/golden/worlds/bedrock-chunks"
FIXTURE_ENTITIES = "tests/golden/worlds/bedrock-entities"
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
    "block_entities_carried": 0,
    "ticks_carried": 3,
    "biomes_carried": 255,
    "skipped": {
        "biomes": 258,
        "block_entities": 0,
        "chunk_data": 0,
        "entities": 0,
        "items": 0,
        "legacy_subchunks": 1,
        "other": 1,
        "ticks": 4,
        "water_positions": 3280,
    },
}

# The entities fixture's report, as far as this check names it: the
# whole-document counts D15 added, and the ones that must stay zero
# (decisions.md#D15).
WANT_ENTITIES_REPORT = {
    "chunks": 2,
    "subchunks": 2,
    "blocks": 8192,
    "block_entities_carried": 2,
    "ticks_carried": 0,
    "biomes_carried": 0,
    "skipped": {
        "biomes": 0,
        "block_entities": 2,
        "chunk_data": 0,
        "entities": 0,
        "items": 0,
        "legacy_subchunks": 0,
        "other": 2,
        "ticks": 0,
        "water_positions": 0,
    },
}

# The ids the carry maps — under the game's own savegame spelling
# ("Chest", "BlastFurnace"; every document in a real world uses it)
# and the namespaced spelling the frozen fixture holds
# ("minecraft:chest"). Java's answer is the namespaced id beside
# each; every other document drops and counts (decisions.md#D15).
CARRY_IDS = {
    "chest": "minecraft:chest",
    "trappedchest": "minecraft:trapped_chest",
    "enderchest": "minecraft:ender_chest",
    "barrel": "minecraft:barrel",
    "hopper": "minecraft:hopper",
    "furnace": "minecraft:furnace",
    "blastfurnace": "minecraft:blast_furnace",
    "smoker": "minecraft:smoker",
    "dispenser": "minecraft:dispenser",
    "dropper": "minecraft:dropper",
}


def java_id(doc_id):
    """Java's id for this document's, or None. The namespaced
    spelling is the short one with the prefix stripped, compared
    without case: "minecraft:chest" and "Chest" are the same
    container in two writers' hands."""
    if not isinstance(doc_id, str):
        return None
    short = doc_id
    if short.startswith("minecraft:"):
        short = short[len("minecraft:"):]
    return CARRY_IDS.get(short.lower())


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
        self._waterlog: dict = {}

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
        key = (name,
               repr(sorted((k, type(v).__name__, py(v))
                           for k, v in states.items())))
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

    def waterloggable(self, name: str) -> bool:
        """Whether the Java block carries the `waterlogged` property:
        the reference's own set over java/1.20 (decisions.md#D15)."""
        if name not in self._waterlog:
            self._waterlog[name] = bool(
                self._java.is_waterloggable(name))
        return self._waterlog[name]

    def biome(self, bedrock_id: int) -> str:
        """The Java biome name for a Bedrock biome id, through the
        reference's own two legs: unpack to the Bedrock name — an id
        its tables do not name answers plains, with a warning
        mcme's count plays the part of (decisions.md#D19) — to
        universal, to Java."""
        name = self._bed.biome.unpack(int(bedrock_id))
        universal = self._bed.biome.to_universal(name)
        return str(self._java.biome.from_universal(universal))

    def biome_int_ids(self) -> list:
        """The ids the reference's Bedrock tables name at all:
        exactly the set convert's biome table holds — any other id
        falls back to plains on both sides (decisions.md#D19)."""
        return sorted(int(i) for i in
                      self._bed.biome._biome_int_to_str)


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
    (cx, cz, cy) -> (kind, layers) where kind is 'ok' or 'legacy'
    and layers is the subchunk's full list of (blocks, palette)
    pairs — the second layer is the waterlogging half D15 folds
    into the Java property."""
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
                    subs[(cx, cz, cy)] = ("ok", layers)
    finally:
        db.close()
    return subs


def put_tick_record(db, cx: int, cz: int, tag: int, snbt: str) -> None:
    """One fabricated tick record on the copy: SNBT in, little-endian
    bytes out, the chunk's own 9-byte key. The fixtures ship no tick
    records; the closed store the census read counted 2235."""
    key = (cx).to_bytes(4, "little", signed=True) \
        + (cz).to_bytes(4, "little", signed=True) + bytes([tag])
    t = amulet_nbt.from_snbt(snbt)
    db.put(key, t.to_nbt(compressed=False, little_endian=True))


def put_biome_record(db, cx: int, cz: int, payload: bytes) -> None:
    """One fabricated Data3D record on the copy: bytes as the game
    writes them — a heightmap, then the arrays, then padding — under
    the chunk's own 9-byte key. The fixtures ship none; the closed
    store the census read held 36,848."""
    key = (cx).to_bytes(4, "little", signed=True) \
        + (cz).to_bytes(4, "little", signed=True) + bytes([0x2B])
    db.put(key, payload)


def biome_record_chunk() -> bytes:
    """Chunk (0,0)'s record, byte for byte what the selftest writes:
    a zeroed heightmap, then six arrays from the world floor — the
    id-3 rename in the single form, two strata with no subchunk to
    stand in (their cells count, never carry), the uniform id 1, a
    mixed width-2 array whose third id the map does not know (the
    cell holding it becomes the reference's plains fallback and
    counts), and the uniform id 6 on top — then the game's 0xFF
    tail, which both decoders walk off a byte at a time."""
    rec = bytearray(512)                      # heightmap: zeros
    rec += bytes([0x00, 3, 0, 0, 0])          # cy -4: one id 3
    rec += bytes([0x00, 1, 0, 0, 0])          # cy -3: one id 1
    rec += bytes([0x00, 1, 0, 0, 0])          # cy -2: one id 1
    rec += bytes([0x00, 1, 0, 0, 0])          # cy -1: one id 1
    arr = bytearray(1 + 1024 + 4 + 12)        # cy  0: width 2
    arr[0] = 0x05                             # header: width 2, palette
    arr[1 + 4 * 0 + 1] = 0x02                 # word 0,  bits 8 -> 194
    arr[1 + 4 * 4] = 0x01                     # word 4,  bits 0 -> 6
    arr[1 + 4 * 64] = 0x01                    # word 64, bits 0 -> 6
    p = 1 + 1024
    arr[p:p + 4] = (3).to_bytes(4, "little")           # palette len
    arr[p + 4:p + 8] = (1).to_bytes(4, "little")       # id 1
    arr[p + 8:p + 12] = (6).to_bytes(4, "little")      # id 6
    arr[p + 12:p + 16] = (194).to_bytes(4, "little",
                                        signed=True)   # id 194
    rec += arr
    rec += bytes([0x00, 6, 0, 0, 0])          # cy  1: one id 6
    rec += b"\xff" * 15                       # the game's tail
    return bytes(rec)


def biome_records_orphans() -> list:
    """The two records no chunk gets use from: an orphan's two
    single-value arrays (every cell counts, none carries) and a
    payload that is not a record (it counts once)."""
    orphan = bytearray(512)
    orphan += bytes([0x00, 1, 0, 0, 0])       # cy -4: one id 1
    orphan += bytes([0x00, 6, 0, 0, 0])       # cy -3: one id 6
    return [
        (5, 5, bytes(orphan)),
        (6, 6, b"not a record, only bytes"),
    ]


def tick_mirror(world: str, ref) -> dict:
    """(cx, cz) -> {'fluid': [...], 'block': [...]} for the chunks
    this fixture converts: records in the scan's order (chunk, then
    0x33 before 0x3a), every entry through the same pinned reference
    the blocks walk uses, t spelled time − currentTick, p 0 — the
    lists mcme must write. A record on a chunk with no subchunks is
    the report's count, not a list; an entry missing x/y/z/time
    drops here the way mcme drops it."""
    from amulet.libs.leveldb import LevelDB
    db = LevelDB(os.path.join(world, "db"))
    out = {(cx, cz): {"fluid": [], "block": []} for cx, cz in CHUNKS}
    try:
        recs = []
        for k, v in db.iterate():
            if len(k) not in (9, 13):
                continue
            tag = k[8] if len(k) == 9 else k[12]
            if tag not in (0x33, 0x3A):
                continue
            cx = int.from_bytes(k[0:4], "little", signed=True)
            cz = int.from_bytes(k[4:8], "little", signed=True)
            recs.append((cx, cz, tag, v))
    finally:
        db.close()
    for cx, cz, tag, payload in sorted(recs):
        bucket = out.get((cx, cz))
        if bucket is None:
            continue          # no subchunks: the report counts it
        root = amulet_nbt.load(io.BytesIO(payload),
                               little_endian=True).tag
        now = root["currentTick"].py_data
        for e in root["tickList"]:
            try:
                t = e["time"].py_data - now
                x = e["x"].py_data
                y = e["y"].py_data
                z = e["z"].py_data
            except KeyError:
                continue      # an entry without the fields drops
            bs = e["blockState"]
            name = str(py(bs["name"]))
            states = dict(bs["states"]) if "states" in bs else {}
            want, _, _ = ref.answer(name, states)
            entry = {"i": want, "p": 0, "t": t, "x": x, "y": y, "z": z}
            if want in ("minecraft:water", "minecraft:lava"):
                bucket["fluid"].append(entry)
            else:
                bucket["block"].append(entry)
    return out


def biome_mirror(world: str, ref) -> dict:
    """(cx, cz) -> {cy: [64 Java biome names]} for the chunks this
    fixture converts: the stored Data3D record decoded the way
    amulet decodes it — one 4x4x4 per height from the world floor,
    cells sampled every fourth block — each id through the
    reference's own biome answer, handed back in Java's container
    order y*16 + z*4 + x. A chunk with no record maps to nothing:
    mcme must carry no biomes key there. Orphan records are the
    report's count, not a section (decisions.md#D19)."""
    from amulet.libs.leveldb import LevelDB
    from amulet.level.formats.leveldb_world.interface.chunk.\
        leveldb_8 import LevelDB8Interface
    iface = LevelDB8Interface()
    db = LevelDB(os.path.join(world, "db"))
    out = {(cx, cz): {} for cx, cz in CHUNKS}
    try:
        for cx, cz in CHUNKS:
            key = (cx).to_bytes(4, "little", signed=True) \
                + (cz).to_bytes(4, "little", signed=True) \
                + bytes([0x2B])
            try:
                payload = db.get(key)
            except KeyError:
                continue                      # no record: no key
            _, arrays = iface._decode_height_3d_biomes(payload, -4)
            for cy, cells in arrays.items():
                names = [""] * 64
                for x in range(4):
                    for y in range(4):
                        for z in range(4):
                            names[y * 16 + z * 4 + x] = ref.biome(
                                int(cells[x][y][z]))
                out[(cx, cz)][cy] = names
    finally:
        db.close()
    return out


def _container_cells(bio) -> list:
    """A section's biomes container resolved back to 64 names in
    Java's order: the palette unpacking read_block does, over the
    container the biome map built — one name is the shorthand with
    no data array, more pack floor(64 / bits) to a long, none
    spanning two."""
    pal = [str(py(bio["palette"][i]))
           for i in range(len(bio["palette"]))]
    data = bio.get("data")
    if data is None:
        longs = None
    else:
        longs = data.py_data
    cells = []
    for i in range(64):
        if longs is None or len(pal) == 1:
            cells.append(pal[0])
            continue
        bits = (len(pal) - 1).bit_length()
        word, bit = divmod(i, 64 // bits)
        idx = (longs[word] >> (bit * bits)) & ((1 << bits) - 1)
        cells.append(pal[idx])
    return cells


def plain(v):
    """The value as plain Python: NBT tags unboxed all the way
    down, so two documents from two editions compare as data."""
    if hasattr(v, "tag") and not hasattr(v, "py_data"):
        v = v.tag        # a NamedTag is the document, not the tag
    v = py(v)
    if isinstance(v, dict):
        return {k: plain(x) for k, x in v.items()}
    if isinstance(v, list):
        return [plain(x) for x in v]
    return v


def _subset(got, want) -> bool:
    """Every field of `want` present and equal in `got`, nested
    dicts included: this check names the counts it pins and lets
    the report grow fields it does not."""
    for k, v in want.items():
        if k not in got:
            return False
        if isinstance(v, dict):
            if not isinstance(got[k], dict) or not _subset(got[k], v):
                return False
        elif got[k] != v:
            return False
    return True


def load_entity_docs(world: str):
    """The store's block-entity records, read from the COPIED
    store: (dimension, cx, cz) -> the documents in record order.
    A record key is x, then z, then the dimension when it is not
    0, then the tag byte."""
    from amulet.libs.leveldb import LevelDB
    from amulet.level.formats.leveldb_world.interface.chunk.leveldb_8 import (
        LevelDB8Interface,
    )

    iface = LevelDB8Interface()
    db = LevelDB(os.path.join(world, "db"))
    out = {}
    try:
        for k, v in db.iterate():
            if len(k) not in (9, 13) or k[-1] != 0x31:
                continue
            cx = int.from_bytes(k[0:4], "little", signed=True)
            cz = int.from_bytes(k[4:8], "little", signed=True)
            dim = (int.from_bytes(k[8:12], "little", signed=True)
                   if len(k) == 13 else 0)
            out[(dim, cx, cz)] = iface._unpack_nbt_list(v)
    finally:
        db.close()
    return out


def carries(doc) -> bool:
    """D15's verdict from the source side: an id the map carries,
    a block address spelled as integers, and a container the world
    has already filled — a LootTable is items mcme has no map
    for, and an empty container where a full one belonged is the
    answer the mapping refuses to write."""
    c = plain(doc)
    return (java_id(c.get("id")) is not None
            and all(isinstance(c.get(k), int) for k in ("x", "y", "z"))
            and "LootTable" not in c)


def _stack_bookkeeping(k, v) -> bool:
    """The fields the game puts on a stack that hold no item data
    Java needs: WasPickedUp's pickup byte; the block identity a
    block item repeats about itself (name/states/version, and
    nothing else — measured, a real world's all 5455); and a tag
    holding exactly {Damage: 0}, the undamaged marker. They drop
    with the stack's answer intact. Anything else beside a stack's
    identity is item data mcme cannot translate."""
    if k == "WasPickedUp":
        return True
    if k == "Block":
        return (isinstance(v, dict)
                and set(v) == {"name", "states", "version"}
                and isinstance(v.get("name"), str)
                and isinstance(v.get("states"), dict)
                and isinstance(v.get("version"), int))
    if k == "tag":
        return (isinstance(v, dict)
                and set(v) == {"Damage"}
                and isinstance(v.get("Damage"), int)
                and v["Damage"] == 0)
    return False


def want_java_doc(doc) -> dict:
    """D15's mapping, spelled from the source document: Java's id
    for the container, and the block's own coordinates; the stacks
    under Slot/id/Count from either of Bedrock's two spellings —
    Name/Count/Damage as the game writes it, id/count/damage as
    the frozen fixture holds it — a nonzero damage becoming Java's
    tag.Damage, and a stack carrying item data mcme cannot
    translate dropping out of the list entirely; a nonempty
    CustomName; a Lock; the smelter timers as ints."""
    c = plain(doc)
    out = {"id": java_id(c["id"]), "x": c["x"], "y": c["y"],
           "z": c["z"]}
    if isinstance(c.get("Items"), list):
        stacks = []
        for s in c["Items"]:
            sid = s.get("Name")
            if not isinstance(sid, str):
                sid = s.get("id")
            count = s.get("Count")
            if not isinstance(count, int):
                count = s.get("count")
            if (not isinstance(sid, str) or not isinstance(count, int)
                    or not isinstance(s.get("Slot"), int)):
                continue
            if any(k not in ("Slot", "Name", "id", "Count", "count",
                             "Damage", "damage")
                   and not _stack_bookkeeping(k, v)
                   for k, v in s.items()):
                continue
            stack = {"Slot": s["Slot"], "id": sid, "Count": count}
            damage = None
            for dk in ("Damage", "damage"):
                if isinstance(s.get(dk), int):
                    damage = s[dk]
                    break
            if damage:
                stack["tag"] = {"Damage": int(damage)}
            stacks.append(stack)
        out["Items"] = stacks
    if c.get("CustomName"):
        out["CustomName"] = c["CustomName"]
    if isinstance(c.get("Lock"), str):
        out["Lock"] = c["Lock"]
    for k in ("BurnTime", "CookTime", "CookTimeTotal"):
        if isinstance(c.get(k), int):
            out[k] = int(c[k])
    return out


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


# The generated table read back out of src/translate/translate.c:
# rows are {"from", state, sval, ival, state2, sval2, ival2, "to"},
# NULL where a condition does not exist.
ROW_RE = re.compile(
    r'\{"([^"]+)",'
    r'\s+(?:"([^"]+)"|NULL),'
    r'\s+(?:"([^"]+)"|NULL),'
    r'\s+(-?\d+),'
    r'\s+(?:"([^"]+)"|NULL),'
    r'\s+(?:"([^"]+)"|NULL),'
    r'\s+(-?\d+),'
    r'\s+"([^"]+)"\},')
BIOME_ROW_RE = re.compile(r'\{\s*(-?\d+),\s*"([^"]+)"\s*\}')
NUMERIC = (amulet_nbt.ByteTag, amulet_nbt.IntTag, amulet_nbt.ShortTag,
           amulet_nbt.LongTag)


def sv(snbt: str):
    """A spec value in its SNBT spelling, back as a tag — the
    reference is tag-type sensitive, so the spelling matters."""
    s = snbt.strip()
    if s.startswith('"'):
        return amulet_nbt.StringTag(s[1:-1])
    if s[-1:] in ("b", "B"):
        return amulet_nbt.ByteTag(int(s[:-1]))
    if s[-1:] in ("s", "S"):
        return amulet_nbt.ShortTag(int(s[:-1]))
    if s[-1:] in ("l", "L"):
        return amulet_nbt.LongTag(int(s[:-1]))
    try:
        return amulet_nbt.IntTag(int(s))
    except ValueError:
        return amulet_nbt.FloatTag(float(s))


def spec_of(ref, name):
    """The bedrock spec's (properties, defaults), or (None, {}) for
    a name the spec table has never heard of — the translation can
    still answer it, and the world's own states are the pin then."""
    ns, base = name.split(":", 1)
    try:
        d = dict(ref._bed.block.get_specification(ns, base))
    except Exception:
        return None, {}
    return (d.get("properties") or None), (d.get("defaults") or {})


def state_spellings(spec_props, conds):
    """Every tag spelling a condition might carry: the spec's own
    where it knows the value (what the state products were checked
    with), and both plausible numeric tags otherwise."""
    per = []
    for state, sval, ival in conds:
        if sval is not None:
            per.append([(state, amulet_nbt.StringTag(sval))])
            continue
        tag = None
        if spec_props and state in spec_props:
            for snbt in spec_props[state]:
                t = sv(snbt)
                if isinstance(t, NUMERIC) and int(t.py_data) == ival:
                    tag = t
                    break
        if tag is not None:
            per.append([(state, tag)])
        else:
            per.append([(state, amulet_nbt.ByteTag(ival)),
                        (state, amulet_nbt.IntTag(ival))])
    out = [{}]
    for opts in per:
        out = [dict(list(d.items()) + [o]) for d in out for o in opts]
    return out


def pin_name_map(ref) -> int:
    """The generated name table re-pinned against the reference:
    every row's conditions answer the row's Java name the way the
    reference answers, and the groups stay in the byte order
    resolve_java_name's binary search walks (decisions.md#D17)."""
    path = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                        os.pardir, "src", "translate", "translate.c")
    with open(path, encoding="utf-8") as f:
        src = f.read()
    if "---- BEGIN GENERATED NAME MAP ----" not in src \
            or "---- END GENERATED NAME MAP ----" not in src:
        raise AssertionError(
            "the generated name map's markers are missing from "
            "translate.c")
    body = src.split("---- BEGIN GENERATED NAME MAP ----", 1)[1]\
        .split("---- END GENERATED NAME MAP ----", 1)[0]
    rows = ROW_RE.findall(body)
    if not rows:
        raise AssertionError("the generated name map holds no rows")
    order = [r[0] for r in rows]
    if order != sorted(order):
        raise AssertionError(
            "the name map is not sorted by name; the lookup's binary "
            "search walks byte order")
    for from_name, st1, sv1, iv1, st2, sv2, iv2, to in rows:
        # findall hands back '' for the NULL branches of a row.
        st1, sv1 = st1 or None, sv1 or None
        st2, sv2 = st2 or None, sv2 or None
        spec_props, defaults = spec_of(ref, from_name)
        base_states = {k: sv(v) for k, v in defaults.items()}
        conds = [] if st1 is None else [(st1, sv1, int(iv1))]
        if st2 is not None:
            conds.append((st2, sv2, int(iv2)))
        answers = set()
        for extra in state_spellings(spec_props, conds):
            states = dict(base_states)
            states.update(extra)
            answers.add(ref.answer(from_name, states)[0])
        if to not in answers:
            raise AssertionError(
                f"{from_name} {tuple(c[0] for c in conds)}: the "
                f"reference answers {sorted(answers)}, the table "
                f"says {to}")
    return len(rows)


def pin_biome_map(ref) -> int:
    """The generated biome table re-pinned against the reference:
    every row's Java name is the reference's own two-leg answer for
    its id, the ids are exactly the ones the Bedrock tables name
    (an id they do not name falls back to plains on both sides,
    mcme counting the cell), and the rows stay in the id order the
    lookup's binary search walks (decisions.md#D19)."""
    path = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                        os.pardir, "src", "translate", "translate.c")
    with open(path, encoding="utf-8") as f:
        src = f.read()
    if "---- BEGIN GENERATED BIOME MAP ----" not in src \
            or "---- END GENERATED BIOME MAP ----" not in src:
        raise AssertionError(
            "the generated biome map's markers are missing from "
            "translate.c")
    body = src.split("---- BEGIN GENERATED BIOME MAP ----", 1)[1] \
        .split("---- END GENERATED BIOME MAP ----", 1)[0]
    rows = BIOME_ROW_RE.findall(body)
    if not rows:
        raise AssertionError("the generated biome map holds no rows")
    order = [int(r[0]) for r in rows]
    if order != sorted(order):
        raise AssertionError(
            "the biome map is not sorted by id; the lookup's binary "
            "search walks id order")
    named = ref.biome_int_ids()
    if order != named:
        missing = sorted(set(named) - set(order))
        extra = sorted(set(order) - set(named))
        raise AssertionError(
            f"the biome map holds {len(order)} ids, the reference "
            f"names {len(named)} — unnamed in the map: {missing}; "
            f"named only by the map: {extra}")
    for i, name in rows:
        want = ref.biome(int(i))
        if name != want:
            raise AssertionError(
                f"biome {i}: the reference answers {want}, the table "
                f"says {name}")
    return len(rows)


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

        # ---- four tick records and three Data3D records,
        # fabricated on the copy (D18, D19) ------------------------
        # Three entries must arrive in the output's lists — the
        # mirror below recomputes them from these records. The
        # orphan's two entries and the payload that is not NBT must
        # be counted and never appear; the entry with no z drops
        # whole. WANT_REPORT pins all seven (3 carried, 4 dropped).
        # The biome records answer to the same report and to the
        # biome mirror below: (0,0)'s cells carried per section, the
        # orphan's and the junk's counted and never carried.
        from amulet.libs.leveldb import LevelDB
        tdb = LevelDB(os.path.join(in_world, "db"))
        try:
            put_tick_record(tdb, 0, 0, 0x33,
                '{currentTick:100,tickList:['
                '{blockState:{name:"minecraft:flowing_water",'
                'states:{liquid_depth:3},version:17959425},'
                'time:100L,x:5,y:65,z:5},'
                '{blockState:{name:"minecraft:sand",'
                'states:{sand_type:"red"},version:17959425},'
                'time:95L,x:6,y:64,z:6},'
                '{blockState:{name:"minecraft:stone",states:{},'
                'version:17959425},time:100L,x:7,y:64}]}')
            put_tick_record(tdb, 0, 0, 0x3A,
                '{currentTick:100,tickList:['
                '{blockState:{name:"minecraft:magma",states:{},'
                'version:17959425},time:120L,x:7,y:64,z:7}]}')
            put_tick_record(tdb, 7, 7, 0x33,
                '{currentTick:50,tickList:['
                '{blockState:{name:"minecraft:gravel",states:{},'
                'version:17959425},time:60L,x:113,y:64,z:113},'
                '{blockState:{name:"minecraft:sand",states:{},'
                'version:17959425},time:61L,x:114,y:64,z:114}]}')
            tdb.put(
                (8).to_bytes(4, "little", signed=True)
                + (8).to_bytes(4, "little", signed=True)
                + bytes([0x33]),
                b"not NBT, only bytes")
            put_biome_record(tdb, 0, 0, biome_record_chunk())
            for cx, cz, payload in biome_records_orphans():
                put_biome_record(tdb, cx, cz, payload)
        finally:
            tdb.close()

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
                kind, layers = subs[(cx, cz, cy)]
                if kind == "legacy":
                    # A legacy subchunk writes no section — the
                    # sections list already proved it — and an
                    # absent section reads back air. The selftest
                    # pins mcme's own reader to the same answer.
                    continue
                blocks, palette = layers[0]
                arr = numpy.asarray(blocks).reshape(16, 16, 16)
                # The second layer, when the subchunk stores one: a
                # cell of water over a block the reference gives the
                # waterlogged property carries as that property on
                # the Java block (decisions.md#D15). Every other
                # second-layer cell drops and counts, so those
                # positions keep the first layer's answer alone.
                l1_arr = None
                l1_palette = None
                if len(layers) > 1:
                    l1_blocks, l1_palette = layers[1]
                    l1_arr = numpy.asarray(l1_blocks).reshape(16, 16, 16)
                for lx in range(16):
                    for ly in range(16):
                        for lz in range(16):
                            name, states = entry_of(
                                palette[int(arr[lx][ly][lz])])
                            want_name, want, default = ref.answer(
                                name, states)
                            if (l1_arr is not None
                                    and entry_of(l1_palette[int(
                                        l1_arr[lx][ly][lz])])[0]
                                    == "minecraft:water"
                                    and ref.waterloggable(want_name)):
                                want = dict(want,
                                            waterlogged="true")
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

        # ---- ticks: the carried lists against the source records
        # (D18) ----------------------------------------------------
        # Both keys must stand in every chunk — empty where no entry
        # lands — and every carried entry must spell the mirror's:
        # the reference's own name for the block, t = time −
        # currentTick (negative is legal, the record's own delay
        # past due), p 0, water and lava under fluid_ticks.
        want_lists = tick_mirror(in_world, ref)
        for cx, cz in CHUNKS:
            region = os.path.join(
                out, "region", f"r.{cx >> 5}.{cz >> 5}.mca")
            tag = walk_chunk(region, cx, cz)
            if tag is None:
                raise AssertionError(
                    f"no tick output chunk ({cx}, {cz})")
            ft = tag.get("fluid_ticks")
            bt = tag.get("block_ticks")
            if ft is None or bt is None:
                raise AssertionError(
                    f"chunk ({cx}, {cz}) does not spell both tick "
                    f"lists (fluid_ticks {ft is not None}, "
                    f"block_ticks {bt is not None})")
            got = {"fluid": [plain(e) for e in ft],
                   "block": [plain(e) for e in bt]}
            if got != want_lists[(cx, cz)]:
                raise AssertionError(
                    f"chunk ({cx}, {cz}) tick lists are {got}, "
                    f"want {want_lists[(cx, cz)]}")
        checks += 1
        _log("ok — both tick lists stand in every chunk and carry "
             "every record's entries with the reference's names, "
             "t = time − currentTick, p 0, fluid split from blocks")

        # ---- biomes: the Data3D record against the output's own
        # containers (D19) ----------------------------------------
        # Every section the record reaches spells the reference's
        # own name for each of its 64 cells, resolved through
        # mcme's palette and packed data in Java's container order —
        # content pinned cell by cell, the container's internal
        # order left mcme's to choose. A section the record does not
        # reach (the strata with no subchunk, the ground above the
        # range, the chunk with no record) carries no key at all:
        # the reference's own shape for a store that held no biome
        # data there.
        want_biomes = biome_mirror(in_world, ref)
        cells_checked = 0
        for cx, cz in CHUNKS:
            region = os.path.join(
                out, "region", f"r.{cx >> 5}.{cz >> 5}.mca")
            tag = walk_chunk(region, cx, cz)
            if tag is None:
                raise AssertionError(
                    f"no biome output chunk ({cx}, {cz})")
            for sec in tag["sections"]:
                cy = sec["Y"].py_data
                want = want_biomes[(cx, cz)].get(cy)
                bio = sec.get("biomes")
                if want is None:
                    if bio is not None:
                        raise AssertionError(
                            f"chunk ({cx}, {cz}) section {cy} carries "
                            f"biomes the record never held")
                    continue
                if bio is None:
                    raise AssertionError(
                        f"chunk ({cx}, {cz}) section {cy} holds no "
                        f"biomes for the record's cells")
                got = _container_cells(bio)
                for i in range(64):
                    cells_checked += 1
                    if got[i] != want[i]:
                        raise AssertionError(
                            f"chunk ({cx}, {cz}) section {cy} cell "
                            f"{i} reads {got[i]}, the reference says "
                            f"{want[i]}")
        if cells_checked != 256:
            raise AssertionError(
                f"checked {cells_checked} biome cells across the "
                f"sections the record reaches, want 256 (4 heights "
                f"x 64)")
        checks += 1
        _log("ok — every section the record reaches carries the "
             "reference's biome for each of its 64 cells, packed "
             "cell by cell, and every other section keeps no key")

        # ---- block entities: the carry against the fixture's own
        # documents (D15) ------------------------------------------
        # The second world is converted as it ships — copied whole,
        # level.dat added — and its report counts the documents the
        # mapping carries, the ones it drops, and the keys of the
        # dimension no chunk is written for.
        be_in = os.path.join(tmp, "be_in")
        shutil.copytree(FIXTURE_ENTITIES, be_in)
        shutil.copy(FIXTURE_LEVEL, os.path.join(be_in, "level.dat"))
        be_out = os.path.join(tmp, "be_out")
        rc, stdout, stderr = run(mcme, "--json", "convert", be_in,
                                 be_out, "--to", "java/1.20")
        if rc != 0:
            raise AssertionError(
                f"convert (entities): exit {rc} ({stderr.strip()})")
        be_report = json.loads(stdout)
        if not _subset(be_report, WANT_ENTITIES_REPORT):
            raise AssertionError(
                f"entities report is {be_report}, want at least "
                f"{WANT_ENTITIES_REPORT}")
        checks += 1
        _log("ok — the entities fixture's report counts two carried "
             "documents, the sign and the frame it drops, the nether "
             "keys as other, and nothing skipped besides")

        # And the output's list against the source's records: each
        # carried document with its mapping's fields spelled out,
        # and no document the verdict drops.
        src_docs = load_entity_docs(be_in)
        for cx, cz in CHUNKS:
            region = os.path.join(
                be_out, "region", f"r.{cx >> 5}.{cz >> 5}.mca")
            tag = walk_chunk(region, cx, cz)
            if tag is None:
                raise AssertionError(
                    f"no entities output chunk ({cx}, {cz})")
            bel = tag.get("block_entities")
            got = [plain(e) for e in (bel or [])]
            want = [want_java_doc(d)
                    for d in src_docs.get((0, cx, cz), [])
                    if carries(d)]
            if got != want:
                raise AssertionError(
                    f"chunk ({cx}, {cz}) block_entities is {got}, "
                    f"want {want}")
        checks += 1
        _log("ok — every carried document arrives with the mapping's "
             "fields (id, x/y/z, Items Slot/id/Count, the smelter "
             "timers) and every dropped one stays away")

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

        # ---- the generated name map, against the reference ----------
        pinned = pin_name_map(ref)
        checks += 1
        _log(f"ok — the name map's {pinned} rows all answer the way "
             "the reference answers, in the order the lookup walks")

        # ---- the generated biome map, against the reference ---------
        pinned_biomes = pin_biome_map(ref)
        checks += 1
        _log(f"ok — the biome map's {pinned_biomes} ids all answer "
             "the way the reference answers, in the order the "
             "lookup walks")

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
