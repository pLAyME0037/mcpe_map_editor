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
             through untouched. A v1 legacy entry — no `states`,
             a `val` — answers the loader's own `block_data` rule
             at the 1.12 cap (D24): its section is written
             beside the others and every position compares like
             any other. A pre-palette payload (versions 0, 2-7,
             under the pre-1.18 0x2c prefix) decodes into cells
             the loader would spell `block_data` with no version
             of their own (D25), and those anchor at the payload's
             own era instead of the cap: payloads 0-5 answer at
             bedrock 1.1.0, 6-7 at 1.2.0 — the mirror's
             answer_flat against mcme's era rows and their
             fall-through to D24's chain. The second layer carries
             as the Java `waterlogged` property only where the
             reference gives the block that property
             (decisions.md#D15) — the fixture's water stands over
             grass, dirt and stone, none of them waterloggable, so
             every water position drops and is counted. Booleans
             compare true/1 and
             false/0 alike — the two editions spell them
             differently over the same NBT meaning.

  REPORT   — the JSON counts, exact: five chunks across three
             dimensions, twelve subchunks (ten palettized, two
             fabricated flat), 49152 blocks, the nether's own
             document carried, the two fabricated payloads that
             must refuse and count `legacy_subchunks` (a flat one
             cut inside its planes, a version nothing decodes),
             and the skips the fixture
             carries (the key of a dimension
             beyond the game's three, the 3280 water positions) —
             plus the tick records this tool fabricates on the
             copy: four entries carried, five dropped
             (decisions.md#D18).

  DIMENSIONS — three dimensions' own answers (D21): every chunk
             written where the game reads it (region/,
             DIM-1/region, DIM1/region), yPos at the dimension's
             floor (-4 overworld, 0 nether and end), every map
             inside its own dimension's height (384 or 256), and
             the walks, pairing and records holding dimensions
             apart at shared coordinates — the same (0,0) and
             (1,-2) in two dimensions are five chunks with five
             answers, and a dimension number beyond the three
             counts `other`.

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
# The fixture's chunks as three dimensions hold them (D21): the
# overworld's two, the nether's own at (0,0) and (1,-2), the end's
# at the overworld's own (1,-2) — the nether key that ships with
# the store and the rest fabricated on the copy below.
DIM_CHUNKS = {
    0: ((0, 0), (1, -2)),
    1: ((0, 0), (1, -2)),
    2: ((1, -2),),
}
DIM_DIR = {0: "", 1: "DIM-1", 2: "DIM1"}
DIM_FLOOR = {0: -4, 1: 0, 2: 0}   # yPos: the floor section

# The eras a pre-palette payload answers at (D25). Those payloads
# carry no block version of their own, so amulet's block_data
# branch caps at min(chunk_game_version, 1.12) — and the chunk's
# game version comes from the payload byte itself (a 0x2c key's
# payload doubles as the chunk-version record), which through
# chunk_version_to_max_version and PyMCTranslate's nearest-version
# rule resolves to bedrock (1,1,0) for payloads 0-5 and bedrock
# (1,2,0) for 6-7 in any world that can hold them. The keys are
# the packed block-version spelling mcme's decoder stamps into the
# synthesized entry's `version` field: major<<24 | minor<<16.
FLAT_ERA = {
    (1 << 24) | (1 << 16): (1, 1, 999),   # 16842752 -> bedrock_1_1_0
    (1 << 24) | (2 << 16): (1, 2, 999),   # 16908288 -> bedrock_1_2_0
}
FLAT_PAYLOAD_ERAS = {0: (1 << 24) | (1 << 16),
                     2: (1 << 24) | (1 << 16),
                     3: (1 << 24) | (1 << 16),
                     4: (1 << 24) | (1 << 16),
                     5: (1 << 24) | (1 << 16),
                     6: (1 << 24) | (2 << 16),
                     7: (1 << 24) | (2 << 16)}


def region_path(world: str, dim: int, cx: int, cz: int) -> str:
    """The output region file a dimension keeps this chunk in: the
    game's own layout — region/, DIM-1/region, DIM1/region."""
    return os.path.join(world, DIM_DIR[dim], "region",
                        f"r.{cx >> 5}.{cz >> 5}.mca")

TRUEISH = {True, "true", 1, "1"}
FALSEISH = {False, "false", 0, "0"}

WANT_REPORT = {
    "from": "bedrock",
    "to": "java/1.20",
    # five chunks across three dimensions (D21): the overworld's
    # two, the fixture's nether key at (0,0), the nether and end
    # chunks fabricated at (1,-2); twelve subchunks; the two extra
    # parity payloads of 4096 cells apiece over the fixture's
    # 24576, plus the v1 legacy subchunk's own 4096 (D24 — it
    # writes its section now), plus the two fabricated flat
    # payloads of 4096 cells apiece (D25 — they decode).
    "chunks": 5,
    "subchunks": 12,
    "blocks": 49152,
    # the nether's own documents (D22): the container beside the
    # sign, spawner, campfire, gateway, lectern and jukebox the
    # check fabricates there
    "block_entities_carried": 7,
    "ticks_carried": 4,
    # 383 from the Data3D records (D19, D21) plus the end chunk's
    # 60 known cells: the older spelling's broadcast carries the
    # one section it writes, one sampled column's id unknown (D26)
    "biomes_carried": 443,
    # the overworld base 100, the nether 0, and the end chunk's own
    # map since the older spelling's half rides the same verdicts
    "heightmaps_carried": 3,
    "skipped": {
        # one fabricated actor record (D23); the digp index beside
        # it counts nowhere — its documents are the actor records
        "actors": 1,
        # 386 from the Data3D records (D19, D21) plus the older
        # spelling's own drops (D26): the end chunk's 15 broadcast
        # arrays with no section to stand in (960), the unknown
        # column's four fallback cells in the section that writes,
        # the second spelling beside the overworld's answering
        # record (16 arrays of 64), and the orphan one byte short
        # of a record (counted once)
        "biomes": 2375,
        # the nether orphan's document, and the bed whose color
        # keeps it out of the list (D22's coupling count)
        "block_entities": 2,
        "chunk_data": 1,           # its dimension's version key
        # the entity lists (D23): the nether's record that will not
        # decode counts once, the two documents at (7,7) count two,
        # the record at (8,8) holds nothing and counts zero
        "entities": 3,
        # the refused maps (one column past each world's top), the
        # chunks holding no record, and since D26 the second
        # spelling's map beside the overworld's answering record and
        # the orphan's lost with its refused payload — the end
        # chunk's map crosses now, its own record standing
        "heightmaps": 7,
        "items": 0,
        # the v1 subchunk's `{name, val}` palette carries now (D24);
        # the two payloads fabricated to refuse (D25): a flat one
        # cut inside its planes, a version nothing decodes
        "legacy_subchunks": 2,
        "other": 1,                # the dimension beyond the three
        "ticks": 5,
        "water_positions": 3280,
    },
}

# The entities fixture's report, as far as this check names it: the
# whole-document counts D15 added, and the ones that must stay zero
# (decisions.md#D15).
WANT_ENTITIES_REPORT = {
    # The store's nether keys are no longer `other`: its subchunk
    # becomes the nether's own chunk (in DIM-1) and its record's
    # document rides with it — three carried now, the overworld's
    # two beside it (D21).
    "chunks": 3,
    "subchunks": 3,
    "blocks": 12288,
    # D22: the fixture's sign carries — its old-format text has no
    # measured Java spelling, so it arrives with both sides empty
    # beside the two chests and the furnace
    "block_entities_carried": 4,
    "ticks_carried": 0,
    "biomes_carried": 0,
    "heightmaps_carried": 0,
    "skipped": {
        "actors": 0,               # the fixture holds no actor keys
        "biomes": 0,
        # the frame (D15) — the sign no longer among them (D22)
        "block_entities": 1,
        "chunk_data": 0,
        "entities": 0,
        "heightmaps": 3,
        "items": 0,
        "legacy_subchunks": 0,
        "other": 0,
        "ticks": 0,
        "water_positions": 0,
    },
}

# The ids the carry maps — under the game's own savegame spelling
# ("Chest", "BlastFurnace"; every document in a real world uses it)
# and the namespaced spelling the frozen fixture holds
# ("minecraft:chest"). Java's answer is the namespaced id beside
# each, and the family's kind says what travels beside id and
# position (decisions.md#D15, #D22). Every other document drops and
# counts.
CARRY_IDS = {
    # D15: the containers — Items, CustomName, Lock, the smelter
    # timers
    "chest": ("minecraft:chest", "container"),
    "trappedchest": ("minecraft:trapped_chest", "container"),
    "enderchest": ("minecraft:ender_chest", "container"),
    "barrel": ("minecraft:barrel", "container"),
    "hopper": ("minecraft:hopper", "container"),
    "furnace": ("minecraft:furnace", "container"),
    "blastfurnace": ("minecraft:blast_furnace", "container"),
    "smoker": ("minecraft:smoker", "container"),
    "dispenser": ("minecraft:dispenser", "container"),
    "dropper": ("minecraft:dropper", "container"),
    # D22: the non-container families
    "shulkerbox": ("minecraft:shulker_box", "container"),
    "brewingstand": ("minecraft:brewing_stand", "brewing"),
    "mobspawner": ("minecraft:mob_spawner", "spawner"),
    "sign": ("minecraft:sign", "sign"),
    "hangingsign": ("minecraft:hanging_sign", "sign"),
    "comparator": ("minecraft:comparator", "comparator"),
    "beacon": ("minecraft:beacon", "beacon"),
    "endgateway": ("minecraft:end_gateway", "gateway"),
    "campfire": ("minecraft:campfire", "campfire"),
    "sculksensor": ("minecraft:sculk_sensor", "empty"),
    "sculkcatalyst": ("minecraft:sculk_catalyst", "empty"),
    "sculkshrieker": ("minecraft:sculk_shrieker", "empty"),
    "jukebox": ("minecraft:jukebox", "jukebox"),
    "chiseledbookshelf": ("minecraft:chiseled_bookshelf", "shelf"),
    "lectern": ("minecraft:lectern", "empty"),
    "skull": ("minecraft:skull", "empty"),
    "bell": ("minecraft:bell", "empty"),
    "enchanttable": ("minecraft:enchanting_table", "empty"),
    "daylightdetector": ("minecraft:daylight_detector", "empty"),
    "endportal": ("minecraft:end_portal", "empty"),
}


def _carry_entry(doc_id):
    """The mapping's row for this document's id, or None. The
    namespaced spelling is the short one with the prefix stripped,
    compared without case: "minecraft:chest" and "Chest" are the
    same container in two writers' hands."""
    if not isinstance(doc_id, str):
        return None
    short = doc_id
    if short.startswith("minecraft:"):
        short = short[len("minecraft:"):]
    return CARRY_IDS.get(short.lower())


def java_id(doc_id):
    """Java's id for this document's, or None. The namespaced
    spelling is the short one with the prefix stripped, compared
    without case: "minecraft:chest" and "Chest" are the same
    container in two writers' hands."""
    entry = _carry_entry(doc_id)
    return entry[0] if entry else None


def be_kind(doc_id) -> str:
    """What travels beside id and position for this family (D22)."""
    entry = _carry_entry(doc_id)
    return entry[1] if entry else "empty"


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
        # The loader's cap for a block_data entry: min(world,
        # (1, 12, 999)) — mcme's input is bedrock 1.20.80, and
        # PyMCTranslate resolves the cap to bedrock (1, 12, 0) (D24).
        self._cap = tm.get_version("bedrock", (1, 12, 999))
        self._cache: dict = {}
        self._legacy: dict = {}
        self._defaults: dict = {}
        self._waterlog: dict = {}
        self._flat: dict = {}
        self._eras: dict = {}
        self._flat_names: dict = {}

    def _translate(self, name: str, states: dict, bed=None):
        ns, base = name.split(":", 1)
        block = self._Block(ns, base, dict(states))
        start = self._unknown.hits
        universal, _, _ = (bed or self._bed).block.to_universal(block)
        java, _, _ = self._java.block.from_universal(universal)
        unknown = self._unknown.hits > start
        if not isinstance(java, self._Block):
            raise AssertionError(
                f"{name}: the reference answered with an entity")
        out_name = f"{java.namespace}:{java.base_name}"
        props = ({str(k): py(v) for k, v in java.properties.items()}
                 if java.properties else {})
        return out_name, props, unknown

    def answer(self, name: str, states: dict | None,
               val: int | None = None):
        """(java name, properties, default properties) for a bedrock
        block, canonically spelled. An unknown block loses its
        states on mcme's side of the comparison, so the reference's
        pass-through states are set aside. `val` names a legacy
        entry (D24): the loader spells it `block_data` and caps the
        spec at 1.12, and what that rule leaves untranslated passes
        through — kept name, `block_data` set aside the same way."""
        if val is not None:
            key = (name, val)
            if key not in self._legacy:
                out_name, props, unknown = self._translate(
                    name, {"block_data": amulet_nbt.IntTag(val)},
                    bed=self._cap)
                if unknown or "block_data" in props:
                    self._legacy[key] = (name, {}, {})
                else:
                    self._legacy[key] = (out_name, canon(props), {})
            return self._legacy[key]
        if states is None:      # neither `states` nor `val`: no states
            states = {}
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

    def answer_flat(self, name: str, val: int, era: int):
        """(java name, properties, defaults) for one pre-palette
        cell (D25): the val branch's own rule — the loader spells
        the cell `block_data` — anchored at the payload's era
        instead of the 1.12 cap, because a flat subchunk's cells
        carry no version of their own and the chunk-game version
        that anchors them comes from the payload byte. Past the
        anchor the val branch's rules hold: what the spec leaves
        untranslated passes through, and mcme's unknown policy
        keeps the name and drops the states."""
        if era not in FLAT_ERA:
            raise AssertionError(f"unknown flat era {era:#x}")
        key = (era, name, val)
        if key not in self._flat:
            if era not in self._eras:
                self._eras[era] = \
                    self._cap._translation_manager.get_version(
                        "bedrock", FLAT_ERA[era])
            out_name, props, unknown = self._translate(
                name, {"block_data": amulet_nbt.IntTag(val)},
                bed=self._eras[era])
            if unknown or "block_data" in props:
                self._flat[key] = (name, {}, {})
            else:
                self._flat[key] = (out_name, canon(props), {})
        return self._flat[key]

    def flat_name(self, block_id: int) -> str:
        """The name the reference gives a pre-palette cell's id
        (D25): ints_to_block is its own id leg — the 1.12.0
        numerical map, the same Version the cap resolves to and
        the one flat tuples always convert through (bedrock
        __init__.py's `or 17563649`). Ids that map does not hold
        answer as the minecraft:numerical placeholder."""
        if block_id not in self._flat_names:
            b = self._cap.block.ints_to_block(block_id, 0)
            self._flat_names[block_id] = \
                f"{b.namespace}:{b.base_name}"
        return self._flat_names[block_id]

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


def raw_str(v) -> str:
    """A reference property value in the NBT string mcme writes:
    bools need their java spelling (str(True) would be "True"), and
    everything else str()s the way the table records it raw."""
    if isinstance(v, bool):
        return "true" if v else "false"
    return str(v)


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
    """(name, states, val) of a Bedrock palette entry. `val` is set
    for a legacy entry — no `states` to carry, the loader's own
    `block_data` rule (D24) — and `states` is a dict otherwise,
    empty when the entry carries neither field (the loader's other
    branch)."""
    comp = entry.tag if hasattr(entry, "tag") else entry.value
    name = str(py(comp["name"]))
    if "states" in comp:
        return name, dict(comp["states"]), None
    if "val" in comp:
        return name, None, int(py(comp["val"]))
    return name, {}, None


def put_flat(db, cx: int, cz: int, dim: int, cy: int, version: int,
             cells: dict) -> None:
    """A pre-palette subchunk under the pre-1.18 prefix (D25): the
    payload byte, 4096 block ids, then 2048 data nibbles low nibble
    first — the bytes both decoders read. A cell's pair sits at its
    XZY index (i = x<<8 | z<<4 | y); `cells` spells
    {(x, y, z): (id, data)} and every other cell is air."""
    ids = bytearray(4096)
    nib = bytearray(2048)
    for (x, y, z), (bid, data) in cells.items():
        i = (x << 8) | (z << 4) | y
        ids[i] = bid
        if i & 1:
            nib[i >> 1] = (nib[i >> 1] & 0x0F) | (data << 4)
        else:
            nib[i >> 1] = (nib[i >> 1] & 0xF0) | data
    db.put(chunk_key(cx, cz, cy, dim, tag=0x2C),
           bytes([version]) + bytes(ids) + bytes(nib))


def _flat_pair(item):
    """One of amulet's flat palette entries — ((version, block),) —
    as (id, data). The loader's air seed carries a Block instead of
    the pair and is never referenced by the cells."""
    _version, block = item[0]
    if isinstance(block, tuple):
        return int(block[0]), int(block[1])
    return 0, 0


def synthesize_flat(palette, ref, era: int):
    """amulet's flat palette — its seed and the (id, data) tuples —
    as the loader would spell each cell's entry: {name, val,
    version} (D25), the same three fields mcme's decoder composes
    and entry_of reads. Positions stay put — the cells were
    offset past the seed by the loader."""
    out = []
    for item in palette:
        bid, data = _flat_pair(item)
        comp = amulet_nbt.CompoundTag()
        comp["name"] = amulet_nbt.StringTag(ref.flat_name(bid))
        comp["val"] = amulet_nbt.IntTag(data)
        comp["version"] = amulet_nbt.IntTag(era)
        out.append(amulet_nbt.NamedTag(comp))
    return out


def load_input(world: str, ref):
    """The fixture's stored subchunks, read from the COPIED store:
    (dim, cx, cz, cy) -> (kind, layers, era). `kind` is 'ok' for a
    modern palettized payload, 'legacy' for a version-1 payload
    whose `{name, val}` entries answer through the loader's rule
    (D24) — its layers compare like any other's — and 'flat' for
    a pre-palette payload (D25), where `era` names the packed
    block version its cells answer at. layers is the subchunk's
    full list of (blocks, palette) pairs — the second layer is
    the waterlogging half D15 folds into the Java property — with
    a flat palette already normalized into loader-shaped
    {name, val, version} entries. Payloads that do not decode are
    absent: the report counts them (legacy_subchunks). The key
    speaks its own dimension (D21), and either subchunk prefix is
    read — 0x2f from 1.18, 0x2c before it."""
    from amulet.libs.leveldb import LevelDB
    from amulet.level.formats.leveldb_world.interface.chunk.leveldb_8 import (
        LevelDB8Interface,
    )

    iface = LevelDB8Interface()
    db = LevelDB(os.path.join(world, "db"))
    subs = {}
    try:
        for dim, chunks in DIM_CHUNKS.items():
            for cx, cz in chunks:
                for cy in range(-5, 6):
                    payload = None
                    for tag in (0x2F, 0x2C):
                        try:
                            payload = db.get(chunk_key(cx, cz, cy, dim,
                                                       tag=tag))
                            break
                        except KeyError:
                            continue
                    if payload is None:
                        continue
                    try:
                        version, layers = decode_payload(iface, payload)
                    except Exception:
                        continue    # a refusal; the report counts it
                    if version in (0, 2, 3, 4, 5, 6, 7):
                        era = FLAT_PAYLOAD_ERAS[version]
                        blocks, palette = layers[0]
                        subs[(dim, cx, cz, cy)] = (
                            "flat",
                            [(blocks, synthesize_flat(palette, ref, era))],
                            era)
                    elif version == 1:
                        subs[(dim, cx, cz, cy)] = ("legacy", layers, None)
                    else:
                        subs[(dim, cx, cz, cy)] = ("ok", layers, None)
    finally:
        db.close()
    return subs


def _record_key(cx: int, cz: int, dim: int, tag: int) -> bytes:
    """A record's key in its own dimension (D21): x, z, the
    dimension when it is not 0, the tag — 9 bytes spoken, 13."""
    key = (cx).to_bytes(4, "little", signed=True) \
        + (cz).to_bytes(4, "little", signed=True)
    if dim:
        key += (dim).to_bytes(4, "little", signed=True)
    return key + bytes([tag])


def put_tick_record(db, cx: int, cz: int, dim: int, tag: int,
                    snbt: str) -> None:
    """One fabricated tick record on the copy: SNBT in, little-endian
    bytes out, the chunk's own key in its own dimension. The
    fixtures ship no tick records; the closed store the census read
    counted 2235."""
    t = amulet_nbt.from_snbt(snbt)
    db.put(_record_key(cx, cz, dim, tag),
           t.to_nbt(compressed=False, little_endian=True))


def put_biome_record(db, cx: int, cz: int, dim: int,
                     payload: bytes) -> None:
    """One fabricated Data3D record on the copy: bytes as the game
    writes them — a heightmap, then the arrays, then padding — under
    the chunk's own key in its own dimension. The fixtures ship
    none; the closed store the census read held 36,848."""
    db.put(_record_key(cx, cz, dim, 0x2B), payload)


def put_2d_record(db, cx: int, cz: int, dim: int,
                  payload: bytes) -> None:
    """One fabricated record in the older 768-byte spelling (D26):
    the same heightmap half, then 256 raw biome ids, under the
    chunk's own key in its own dimension with the 0x2D tag."""
    db.put(_record_key(cx, cz, dim, 0x2D), payload)


def put_be_record(db, cx: int, cz: int, dim: int,
                  *snbts: str) -> None:
    """One block-entity record on the copy: the documents with
    their own coordinates, in the order given, little-endian, under
    the chunk's key in its own dimension (D21). A record is the
    documents concatenated — the way the game stores them — so one
    call places them all together, in record order."""
    payload = b"".join(
        amulet_nbt.from_snbt(s).to_nbt(compressed=False,
                                       little_endian=True)
        for s in snbts)
    db.put(_record_key(cx, cz, dim, 0x31), payload)


def put_entity_record(db, cx: int, cz: int, dim: int,
                      *snbts: str) -> None:
    """One entity-list record on the copy (D23): the documents
    concatenated little-endian under the chunk's own key in its own
    dimension — the block-entity record's own shape with the 0x32
    tag. No documents put an empty record, the game's spelling of a
    list it holds nothing in; every one of them counts zero, not
    one."""
    payload = b"".join(
        amulet_nbt.from_snbt(s).to_nbt(compressed=False,
                                       little_endian=True)
        for s in snbts)
    db.put(_record_key(cx, cz, dim, 0x32), payload)


def biome_record_chunk() -> bytes:
    """Chunk (0,0)'s record, byte for byte what the selftest writes:
    a heightmap whose columns spell their own coordinates — column i
    (i = z*16 + x) reads 100 + i, inside the world's height — then
    six arrays from the world floor — the
    id-3 rename in the single form, two strata with no subchunk to
    stand in (their cells count, never carry), the uniform id 1, a
    mixed width-2 array whose third id the map does not know (the
    cell holding it becomes the reference's plains fallback and
    counts), and the uniform id 6 on top — then the game's 0xFF
    tail, which both decoders walk off a byte at a time."""
    rec = bytearray(512)                      # heightmap: 100 + i
    for i in range(256):
        rec[i * 2:i * 2 + 2] = (100 + i).to_bytes(2, "little")
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


def biome_map_only_record() -> bytes:
    """Chunk (1,-2)'s record, byte for byte what the selftest
    writes: no arrays at all (a record may hold none) and one column
    past the world's top — 400 above the floor, where the
    overworld's 384 ends — so the map drops and counts while the
    chunk keeps no invented containers (D20)."""
    rec = bytearray(512)
    for i in range(256):
        rec[i * 2:i * 2 + 2] = (100 + i).to_bytes(2, "little")
    bad = 3 * 16 + 5                          # column (x=5, z=3)
    rec[bad * 2:bad * 2 + 2] = (400).to_bytes(2, "little")
    return bytes(rec)


def biome_2d_record_chunk() -> bytes:
    """The end chunk (1,-2) dim 2's record in the older spelling
    (D26), byte for byte what the selftest decodes: the heightmap
    half's columns spell their own coordinates — column i reads i,
    inside the end's own 256 — then 256 raw biome ids whose
    *sampled* columns name themselves. The reference reads the half
    as frombuffer().reshape(16,16).T and broadcasts it: a section's
    cell (x, z) takes table byte 64z + 4x, so those 16 positions
    carry the answers — id x + 4z at each, every one a biome the
    map knows — except column (x=1, z=2), which holds 194: an id
    neither the reference's table nor mcme's knows, so its four
    cells per section fall back to plains and count, the way the
    Data3D fixture's own unknown id counts (D19)."""
    rec = bytearray(512)
    for i in range(256):
        rec[i * 2:i * 2 + 2] = i.to_bytes(2, "little")
    table = bytearray(256)
    for z in range(4):
        for x in range(4):
            table[64 * z + 4 * x] = x + 4 * z
    table[64 * 2 + 4 * 1] = 194                # the unknown id
    rec += bytes(table)
    return bytes(rec)


def biome_2d_precedence_record() -> bytes:
    """A second spelling on the overworld chunk (0,0), beside the
    Data3D record that answers there (D26): the reference's loader
    takes the 3D arrays when they are there and reads this one
    only when they are not, so every cell it holds counts and none
    carries — 16 broadcast arrays of 64, and a map that counts
    with them. All zero: the shape is what this pins, not the
    values."""
    return bytes(768)


def biome_2d_junk_record() -> bytes:
    """The orphan at (10,10) (D26): 767 bytes — one short of the
    map-and-table shape the reference's own reshape answers for —
    so it is not a record: it counts once, its cells and its map
    lost with it, never half-read."""
    return bytes(767)


def pack_heightmap(values) -> list:
    """Java's own packing of a 256-column map, as 1.16+ spells it:
    9 bits to a value, 7 values to a long, none spanning two — 37
    longs (decisions.md#D20)."""
    longs = [0] * 37
    for p, v in enumerate(values):
        longs[p // 7] |= (v & 0x1FF) << ((p % 7) * 9)
    return longs


def nether_biome_record() -> bytes:
    """The nether chunk (0,0)'s record, byte for byte what the
    selftest writes (D21): a heightmap whose columns spell their own
    coordinates — column i reads i, inside the nether's own 256 —
    then two single-value strata from the nether's floor 0, then the
    game's 0xFF tail. The floor stratum lands in the section at Y0;
    the one above stands where the nether holds no section, counted
    and never carried."""
    rec = bytearray(512)
    for i in range(256):
        rec[i * 2:i * 2 + 2] = i.to_bytes(2, "little")
    rec += bytes([0x00, 1, 0, 0, 0])          # cy 0: one id 1
    rec += bytes([0x00, 1, 0, 0, 0])          # cy 1: one id 1
    rec += b"\xff" * 15
    return bytes(rec)


def nether_dropped_map_record() -> bytes:
    """The nether chunk (1,-2)'s record (D21): no arrays, and one
    column at 300 — inside the overworld's 384, past the nether's
    256 — so the map drops while the floor stratum's cells still
    cross beside it (D20's halves, per dimension)."""
    rec = bytearray(512)
    for i in range(256):
        rec[i * 2:i * 2 + 2] = i.to_bytes(2, "little")
    bad = 3 * 16 + 5                          # column (x=5, z=3)
    rec[bad * 2:bad * 2 + 2] = (300).to_bytes(2, "little")
    rec += bytes([0x00, 1, 0, 0, 0])          # cy 0: one id 1
    return bytes(rec)


def nether_orphan_record() -> bytes:
    """The nether orphan at (7,7) (D21): a record whose chunk holds
    no subchunks — its floor cells and its valid map count in the
    nether's own half of the report, never carried."""
    rec = bytearray(512)
    for i in range(256):
        rec[i * 2:i * 2 + 2] = i.to_bytes(2, "little")
    rec += bytes([0x00, 1, 0, 0, 0])          # cy 0: one id 1
    return bytes(rec)


def tick_mirror(world: str, ref, dim: int = 0) -> dict:
    """(cx, cz) -> {'fluid': [...], 'block': [...]} for the chunks
    this dimension converts: records in the scan's order (chunk,
    then 0x33 before 0x3a), every entry through the same pinned
    reference the blocks walk uses, t spelled time − currentTick,
    p 0 — the lists mcme must write. A record on a chunk with no
    subchunks is the report's count, not a list; an entry missing
    x/y/z/time drops here the way mcme drops it. Only this
    dimension's keys are read (D21): the same coordinates in two
    dimensions are two chunks with two records."""
    from amulet.libs.leveldb import LevelDB
    db = LevelDB(os.path.join(world, "db"))
    out = {(cx, cz): {"fluid": [], "block": []}
           for cx, cz in DIM_CHUNKS[dim]}
    try:
        recs = []
        for k, v in db.iterate():
            if len(k) not in (9, 13):
                continue
            kdim = (int.from_bytes(k[8:12], "little", signed=True)
                    if len(k) == 13 else 0)
            if kdim != dim:
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
            # the loader's own branch, mirrored (D24): `states`
            # present or no `val` is modern, else the block_data
            # rule at the 1.12 cap
            if "states" in bs or "val" not in bs:
                want, _, _ = ref.answer(
                    name, dict(bs["states"]) if "states" in bs else {})
            else:
                want, _, _ = ref.answer(name, None,
                                        val=int(py(bs["val"])))
            entry = {"i": want, "p": 0, "t": t, "x": x, "y": y, "z": z}
            if want in ("minecraft:water", "minecraft:lava"):
                bucket["fluid"].append(entry)
            else:
                bucket["block"].append(entry)
    return out


def biome_mirror(world: str, ref, dim: int = 0) -> dict:
    """(cx, cz) -> {cy: [64 Java biome names]} for the chunks this
    fixture converts: the stored biome record decoded the way
    amulet decodes it — each id through the reference's own biome
    answer, handed back in Java's container order y*16 + z*4 + x.
    Either spelling answers (D26): the Data3D record's arrays, one
    4x4x4 per height from the world floor, cells sampled every
    fourth block; or, when the chunk holds no 3D record, the older
    table's own broadcast — 256 raw ids read the way the loader
    reads them (reshape(16,16).T), every section cy 0..15 taking
    cell (x, z) = table byte 64z + 4x, the same value down the
    section — the loader's elif, the 3D record wins when both
    stand. A chunk with no record maps to nothing: mcme must carry
    no biomes key there. Orphan records are the report's count,
    not a section (decisions.md#D19). Only this dimension's keys
    are read, decoded from its own floor — -4 overworld, 0 nether
    and end (D21)."""
    from amulet.libs.leveldb import LevelDB
    from amulet.level.formats.leveldb_world.interface.chunk.\
        leveldb_8 import LevelDB8Interface
    iface = LevelDB8Interface()
    db = LevelDB(os.path.join(world, "db"))
    out = {(cx, cz): {} for cx, cz in DIM_CHUNKS[dim]}
    try:
        for cx, cz in DIM_CHUNKS[dim]:
            key = (cx).to_bytes(4, "little", signed=True) \
                + (cz).to_bytes(4, "little", signed=True)
            if dim:
                key += (dim).to_bytes(4, "little", signed=True)
            key += bytes([0x2B])
            try:
                payload = db.get(key)
            except KeyError:
                key = key[:-1] + bytes([0x2D])
                try:
                    payload = db.get(key)
                except KeyError:
                    continue                  # no record: no key
                table = payload[512:]
                for cy in range(16):          # the broadcast's own
                    names = [""] * 64          # sections, absolute
                    for x in range(4):
                        for y in range(4):
                            for z in range(4):
                                names[y * 16 + z * 4 + x] = ref.biome(
                                    table[64 * z + 4 * x])
                    out[(cx, cz)][cy] = names
                continue
            _, arrays = iface._decode_height_3d_biomes(
                payload, DIM_FLOOR[dim])
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
    down, so two documents from two editions compare as data. A
    tag's unboxing can be an array — Java's int arrays arrive as
    one — and plain Python means the list it holds."""
    if hasattr(v, "tag") and not hasattr(v, "py_data"):
        v = v.tag        # a NamedTag is the document, not the tag
    v = py(v)
    if isinstance(v, numpy.ndarray):
        return v.tolist()
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


def _java_stack(s, with_slot=True):
    """One stack under Java's spelling (D15's rule, D22's
    no-Slot variant): Slot/id/Count from either of Bedrock's two
    spellings — Name/Count/Damage as the game writes it,
    id/count/damage as the frozen fixture holds it — a nonzero
    damage becoming Java's tag.Damage, and a stack holding item
    data mcme cannot translate not half-written: None, and the
    caller counts it. `with_slot` is how the stack sits in a list;
    a jukebox's record stands alone (D22)."""
    sid = s.get("Name")
    if not isinstance(sid, str):
        sid = s.get("id")
    count = s.get("Count")
    if not isinstance(count, int):
        count = s.get("count")
    if (not isinstance(sid, str) or not isinstance(count, int)
            or (with_slot and not isinstance(s.get("Slot"), int))):
        return None
    if any(k not in ("Slot", "Name", "id", "Count", "count",
                     "Damage", "damage")
           and not _stack_bookkeeping(k, v)
           for k, v in s.items()):
        return None
    stack = {}
    if with_slot:
        stack["Slot"] = s["Slot"]
    stack["id"] = sid
    stack["Count"] = count
    damage = None
    for dk in ("Damage", "damage"):
        if isinstance(s.get(dk), int):
            damage = s[dk]
            break
    if damage:
        stack["tag"] = {"Damage": int(damage)}
    return stack


def _items_field(c) -> list:
    """The Items list as Java holds it: every element through the
    stack rule, a stack that cannot translate left out entirely."""
    if not isinstance(c.get("Items"), list):
        return None
    stacks = []
    for s in c["Items"]:
        stack = _java_stack(s)
        if stack is not None:
            stacks.append(stack)
    return stacks


_PAD = "\\\"\\\""


def _sign_side(side) -> dict:
    """One side of a sign as Java spells it (D22, measured
    through the reference on a real world): four messages from the
    text's lines — the trailing newline ends the last line rather
    than starting an empty one, each line its JSON string — padded
    with the escaped emptiness Java's own translator writes for a
    missing line, beside the color black and no glowing outline
    (the game's color int takes no path through the universal
    format the Java mapping reads, so every sign's answer is
    black)."""
    text = ""
    if isinstance(side, dict) and isinstance(side.get("Text"), str):
        text = side["Text"]
    lines = text.split("\n") if text else []
    if lines and lines[-1] == "":
        lines.pop()
    lines = lines[:4]
    msgs = [json.dumps(s, ensure_ascii=False) for s in lines]
    msgs += [_PAD] * (4 - len(msgs))
    return {"color": "black", "has_glowing_text": 0,
            "messages": msgs}


def want_java_doc(doc) -> dict:
    """The mapping's answer, spelled from the source document: the
    family's Java id and the block's own coordinates for every
    carried document, and beside them what each family's Java
    spelling has — the containers' Items/CustomName/Lock and the
    smelter timers (D15); the brewing stand's progress under
    BrewTime and fuel under Fuel; the spawner's seven ranges and
    its one SpawnData; the sign's two text sides and is_waxed; the
    comparator's signal; the beacon's two chosen effects under
    Java's capitals; the gateway's age and exit portal; the four
    cooks' progress as one int array; the jukebox's record without
    a Slot; the bookshelf's last-touched slot; and nothing beside
    id and position for the types whose Java block entity answers
    with them alone (D22)."""
    c = plain(doc)
    out = {"id": java_id(c["id"]), "x": c["x"], "y": c["y"],
           "z": c["z"]}
    kind = be_kind(c["id"])

    if kind == "container":
        items = _items_field(c)
        if items is not None:
            out["Items"] = items
        if c.get("CustomName"):
            out["CustomName"] = c["CustomName"]
        if isinstance(c.get("Lock"), str):
            out["Lock"] = c["Lock"]
        for k in ("BurnTime", "CookTime", "CookTimeTotal"):
            if isinstance(c.get(k), int):
                out[k] = int(c[k])
    elif kind == "brewing":
        items = _items_field(c)
        if items is not None:
            out["Items"] = items
        if c.get("CustomName"):
            out["CustomName"] = c["CustomName"]
        if isinstance(c.get("Lock"), str):
            out["Lock"] = c["Lock"]
        if isinstance(c.get("CookTime"), int):
            out["BrewTime"] = int(c["CookTime"])
        if isinstance(c.get("FuelAmount"), int):
            out["Fuel"] = int(c["FuelAmount"])
    elif kind == "spawner":
        for k in ("Delay", "MaxNearbyEntities", "MaxSpawnDelay",
                  "MinSpawnDelay", "RequiredPlayerRange",
                  "SpawnCount", "SpawnRange"):
            if isinstance(c.get(k), int):
                out[k] = int(c[k])
        if isinstance(c.get("EntityIdentifier"), str):
            out["SpawnData"] = {"id": c["EntityIdentifier"]}
    elif kind == "sign":
        out["front_text"] = _sign_side(c.get("FrontText"))
        out["back_text"] = _sign_side(c.get("BackText"))
        if isinstance(c.get("IsWaxed"), int):
            out["is_waxed"] = int(c["IsWaxed"])
    elif kind == "comparator":
        if isinstance(c.get("OutputSignal"), int):
            out["OutputSignal"] = int(c["OutputSignal"])
    elif kind == "beacon":
        if isinstance(c.get("primary"), int):
            out["Primary"] = int(c["primary"])
        if isinstance(c.get("secondary"), int):
            out["Secondary"] = int(c["secondary"])
    elif kind == "gateway":
        if isinstance(c.get("Age"), int):
            out["Age"] = int(c["Age"])
        ep = c.get("ExitPortal")
        if (isinstance(ep, list) and len(ep) == 3
                and all(isinstance(v, int) for v in ep)):
            out["ExitPortal"] = {"X": int(ep[0]), "Y": int(ep[1]),
                                 "Z": int(ep[2])}
    elif kind == "campfire":
        times = [c.get(f"ItemTime{i}") for i in (1, 2, 3, 4)]
        if all(isinstance(v, int) for v in times):
            out["CookingTimes"] = [int(v) for v in times]
    elif kind == "jukebox":
        ri = c.get("RecordItem")
        if isinstance(ri, dict):
            stack = _java_stack(ri, with_slot=False)
            if stack is not None:
                out["RecordItem"] = stack
    elif kind == "shelf":
        items = _items_field(c)
        if items is not None:
            out["Items"] = items
        if isinstance(c.get("LastInteractedSlot"), int):
            out["last_interacted_slot"] = int(
                c["LastInteractedSlot"])
    # kind == "empty": id and position are all (D22)
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
LEGACY_ROW_RE = re.compile(
    r'\{"([^"]+)", (-?\d+), "([^"]+)", (\d+), \{(.*)\}\},')
FLAT_ROW_RE = re.compile(
    r'\{(\d+), "([^"]+)", (-?\d+), "([^"]+)", (\d+), \{(.*)\}\},')
FLAT_ID_RE = re.compile(r'"([^"]+)",')
LEGACY_DEF_RE = re.compile(r'\{"([^"]+)", "([^"]+)", (\d+), \{(.*)\}\},')
LEGACY_PROP_RE = re.compile(r'\{"([^"]*)","([^"]*)"\}')
STATE_ROW_RE = re.compile(
    r'\{"([^"]+)", "([^"]+)", (\d+), (-?\d+), (?:"([^"]+)"|NULL), '
    r'"([^"]+)", "([^"]+)"\},')
RESCUE_ROW_RE = re.compile(
    r'\{"([^"]+)", "([^"]+)", (\d+), (-?\d+), (?:"([^"]+)"|NULL), '
    r'"([^"]+)", (\d+), (-?\d+), (?:"([^"]+)"|NULL), '
    r'"([^"]+)", "([^"]+)"\},')
TAG_CODE = {amulet_nbt.ByteTag: 1, amulet_nbt.ShortTag: 2,
            amulet_nbt.IntTag: 3, amulet_nbt.LongTag: 4,
            amulet_nbt.FloatTag: 5, amulet_nbt.DoubleTag: 6,
            amulet_nbt.StringTag: 8}
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


def pin_legacy_map(ref) -> int:
    """The generated legacy table re-pinned against the reference:
    every row is the loader-cap rule's own answer for its (name, val),
    every default holds at each byte-domain val its name's rows do
    not claim plus the extremes, a plain name's unclaimed val keeps
    name and no properties, and both tables stay in the (name, val)
    order the lookup's binary search walks (decisions.md#D24)."""
    path = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                        os.pardir, "src", "translate", "translate.c")
    with open(path, encoding="utf-8") as f:
        src = f.read()
    if "---- BEGIN GENERATED LEGACY MAP ----" not in src \
            or "---- END GENERATED LEGACY MAP ----" not in src:
        raise AssertionError(
            "the generated legacy map's markers are missing from "
            "translate.c")
    body = src.split("---- BEGIN GENERATED LEGACY MAP ----", 1)[1] \
        .split("---- END GENERATED LEGACY MAP ----", 1)[0]
    rows_part = body.split("kLegacyMap[] = {", 1)[1].split("\n};", 1)[0]
    defs_part = body.split("kLegacyDefaults[] = {", 1)[1] \
        .split("\n};", 1)[0]
    rows = LEGACY_ROW_RE.findall(rows_part)
    if not rows:
        raise AssertionError("the generated legacy map holds no rows")
    order = [(r[0], int(r[1])) for r in rows]
    if order != sorted(order):
        raise AssertionError(
            "the legacy map is not sorted by (name, val); the "
            "lookup's binary search walks that order")
    claimed = {}
    for nm, v, to, n, props in rows:
        pr = dict(LEGACY_PROP_RE.findall(props))
        if len(pr) != int(n):
            raise AssertionError(
                f"legacy row {nm}/{v}: {n} properties claimed, "
                f"{len(pr)} written")
        want = ref.answer(nm, None, val=int(v))
        if (want[0], want[1]) != (to, canon(pr)):
            raise AssertionError(
                f"legacy {nm}/{v}: the reference answers "
                f"{(want[0], want[1])}, the table says "
                f"{(to, canon(pr))}")
        claimed.setdefault(nm, set()).add(int(v))
    defaults = LEGACY_DEF_RE.findall(defs_part)
    if not defaults:
        raise AssertionError("the legacy map's default table is empty")
    dorder = [d[0] for d in defaults]
    if dorder != sorted(dorder):
        raise AssertionError(
            "the legacy defaults are not sorted by name; the "
            "lookup's binary search walks that order")
    dnames = set(dorder)
    for nm, to, n, props in defaults:
        pr = dict(LEGACY_PROP_RE.findall(props))
        if len(pr) != int(n):
            raise AssertionError(
                f"legacy default {nm}: {n} properties claimed, "
                f"{len(pr)} written")
        want_for = (to, canon(pr))
        unclaimed = [v for v in range(-300, 300)
                     if v not in claimed.get(nm, set())] \
            + [1000, -1000, 65535, -65536, 2**31 - 1, -2**31]
        for v in unclaimed:
            want = ref.answer(nm, None, val=v)
            if (want[0], want[1]) != want_for:
                raise AssertionError(
                    f"legacy default {nm} at {v}: the reference "
                    f"answers {(want[0], want[1])}, the default "
                    f"says {want_for}")
    # a plain name's unclaimed val keeps its name and no properties
    for nm in sorted(claimed):
        if nm in dnames:
            continue
        for v in (1000, -1000, 2**31 - 1, -2**31):
            if v in claimed[nm]:
                continue
            want = ref.answer(nm, None, val=v)
            if (want[0], want[1]) != (nm, {}):
                raise AssertionError(
                    f"legacy {nm}/{v}: the reference answers "
                    f"{(want[0], want[1])}, mcme keeps name and no "
                    f"properties")
    return len(rows)


def pin_flat_map(ref) -> int:
    """The generated flat tables re-pinned against the reference
    (D25): every id the numerical map names is the reference's own
    ints_to_block answer; every era stamp per payload version is
    what amulet's own chunk-version table anchors at, and what the
    mirror's own constant says; every era row is answer_flat's
    answer for its (era, name, val) and differs from what D24's
    chain would say there; and every cell over the id map's whole
    domain that carries no row is exactly what the chain says —
    which answer_flat says is the era's answer too
    (decisions.md#D25)."""
    root = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                        os.pardir)
    with open(os.path.join(root, "src", "world", "bedrock_chunk.c"),
              encoding="utf-8") as f:
        bsrc = f.read()
    with open(os.path.join(root, "src", "translate", "translate.c"),
              encoding="utf-8") as f:
        tsrc = f.read()

    # ---- the id map: 256 names, the reference's own id leg ----
    for marker in ("---- BEGIN GENERATED NUMERICAL MAP ----",
                   "---- END GENERATED NUMERICAL MAP ----",
                   "---- BEGIN GENERATED FLAT ERAS ----",
                   "---- END GENERATED FLAT ERAS ----"):
        if marker not in bsrc:
            raise AssertionError(
                "the generated flat tables' markers are missing "
                f"from bedrock_chunk.c: {marker}")
    ids_part = bsrc.split("---- BEGIN GENERATED NUMERICAL MAP ----", 1)[1] \
        .split("---- END GENERATED NUMERICAL MAP ----", 1)[0] \
        .split("kFlatIds[256] = {", 1)[1].split("\n};", 1)[0]
    ids = FLAT_ID_RE.findall(ids_part)
    if len(ids) != 256:
        raise AssertionError(
            f"the numerical map holds {len(ids)} names, want 256")
    for i, nm in enumerate(ids):
        want = ref.flat_name(i)
        if nm != want:
            raise AssertionError(
                f"numerical id {i}: the table says {nm}, the "
                f"reference answers {want}")

    # ---- the era stamps: amulet's own chunk-version table ----
    eras_part = bsrc.split("---- BEGIN GENERATED FLAT ERAS ----", 1)[1] \
        .split("kFlatEra[8] = {", 1)[1].split("\n};", 1)[0]
    stamps = [int(v) for v in re.findall(r"-?\d+", eras_part)]
    if len(stamps) != 8:
        raise AssertionError(
            f"the flat era table holds {len(stamps)} stamps, want 8")
    from amulet.level.formats.leveldb_world.interface.chunk \
        .leveldb_chunk_versions import chunk_version_to_max_version
    dir_era = {"bedrock_1_1_0": (1 << 24) | (1 << 16),
               "bedrock_1_2_0": (1 << 24) | (2 << 16)}
    world, cap = (1, 20, 80), (1, 12, 999)
    tm = ref._cap._translation_manager
    for v in range(8):
        if v == 1:                     # the palettized version 1
            want_stamp = 0
        else:
            game = min(chunk_version_to_max_version[v][1], world)
            vo = tm.get_version("bedrock", min(game, cap))
            d = vo._version_path.split("/")[-1]
            if d not in dir_era:
                raise AssertionError(
                    f"payload {v}: the reference anchors at {d}, "
                    f"not one of the two measured flat eras — the "
                    f"measurement is invalid, not the table")
            want_stamp = dir_era[d]
            if FLAT_PAYLOAD_ERAS.get(v) != want_stamp:
                raise AssertionError(
                    f"payload {v}: the mirror stamps "
                    f"{FLAT_PAYLOAD_ERAS.get(v):#x}, amulet answers "
                    f"{want_stamp:#x}")
        if stamps[v] != want_stamp:
            raise AssertionError(
                f"payload {v}: the table stamps {stamps[v]:#x}, "
                f"the reference anchors at {want_stamp:#x}")

    # ---- the era rows, and the chain they stand beside ----
    for marker in ("---- BEGIN GENERATED FLAT MAP ----",
                   "---- END GENERATED FLAT MAP ----"):
        if marker not in tsrc:
            raise AssertionError(
                "the generated flat map's markers are missing "
                f"from translate.c: {marker}")
    rows_part = tsrc.split("---- BEGIN GENERATED FLAT MAP ----", 1)[1] \
        .split("kFlatMap[] = {", 1)[1].split("\n};", 1)[0]
    rows = FLAT_ROW_RE.findall(rows_part)
    if not rows:
        raise AssertionError("the generated flat map holds no rows")
    order = [(int(r[0]), r[1], int(r[2])) for r in rows]
    if order != sorted(order):
        raise AssertionError(
            "the flat map is not sorted by (era, name, val); the "
            "lookup's binary search walks that order")

    # The chain exactly as C walks it: the legacy rows, then the
    # five defaults, then the name alone (mcme's unknown-block
    # policy) — read out of the same table pin_legacy_map pins.
    lbody = tsrc.split("---- BEGIN GENERATED LEGACY MAP ----", 1)[1] \
        .split("---- END GENERATED LEGACY MAP ----", 1)[0]
    lrows = LEGACY_ROW_RE.findall(
        lbody.split("kLegacyMap[] = {", 1)[1].split("\n};", 1)[0])
    ldefs = LEGACY_DEF_RE.findall(
        lbody.split("kLegacyDefaults[] = {", 1)[1].split("\n};", 1)[0])
    lmap = {(nm, int(v)): (to, canon(dict(LEGACY_PROP_RE.findall(props))))
            for nm, v, to, n, props in lrows}
    dmap = {nm: (to, canon(dict(LEGACY_PROP_RE.findall(props))))
            for nm, to, n, props in ldefs}

    def chain(name, val):
        hit = lmap.get((name, val))
        if hit is None:
            hit = dmap.get(name)
        return hit if hit is not None else (name, {})

    fmap = {}
    for era, nm, v, to, n, props in rows:
        pr = dict(LEGACY_PROP_RE.findall(props))
        if len(pr) != int(n):
            raise AssertionError(
                f"flat row {era}/{nm}/{v}: {n} properties claimed, "
                f"{len(pr)} written")
        key = (int(era), nm, int(v))
        if key in fmap:
            raise AssertionError(f"flat row written twice: {key}")
        fmap[key] = (to, canon(pr))

    # The whole cell domain of the id map: a rowed cell answers at
    # its era, an unrowed cell answers through the chain — and
    # answer_flat calls both the reference's own answer.
    asked = 0
    for era in sorted(FLAT_ERA):
        for nm in sorted(set(ids)):
            for val in range(16):
                asked += 1
                want = ref.answer_flat(nm, val, era)
                have = fmap.get((era, nm, val), chain(nm, val))
                if (want[0], want[1]) != have:
                    raise AssertionError(
                        f"flat {era:#x}/{nm}/{val}: the reference "
                        f"answers {(want[0], want[1])}, the tables "
                        f"say {have}")
    # And every row is a real delta: without it the chain would
    # say something else.
    for (era, nm, val), row in fmap.items():
        if era not in FLAT_ERA:
            raise AssertionError(f"flat row at unknown era {era:#x}")
        if chain(nm, val) == row:
            raise AssertionError(
                f"flat {era:#x}/{nm}/{val}: a row the cap chain "
                f"already says — deltas only")
    return len(rows)


def pin_state_map(ref) -> int:
    """The generated state tables re-pinned against the reference
    (decisions.md#D27). Every single row is re-asked at its own cell
    — the block's spec defaults plus the condition's value in the
    spec's own tag spelling — and must hold three ways: the row's
    property is the reference's own RAW answer for it (the pin
    re-derives the spelling `power`'s "1" needs, not the canon
    "true"), a value the delta rules say carries no row, and the
    walk's composition — every matching row in table order, then
    the first matching rescue — satisfies props_match the way the
    pin judges every position. Every rescue row's cell is composed
    the same way and must pass. The singles stay grouped by block
    in the byte order the lookup's binary search walks, and a
    rescue's rows stay contiguous."""
    path = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                        os.pardir, "src", "translate", "translate.c")
    with open(path, encoding="utf-8") as f:
        src = f.read()
    for marker in ("---- BEGIN GENERATED STATE MAP ----",
                   "---- END GENERATED STATE MAP ----",
                   "---- BEGIN GENERATED STATE RESCUE ----",
                   "---- END GENERATED STATE RESCUE ----"):
        if marker not in src:
            raise AssertionError(
                "the generated state map's markers are missing from "
                f"translate.c: {marker}")
    map_body = src.split("---- BEGIN GENERATED STATE MAP ----", 1)[1] \
        .split("---- END GENERATED STATE MAP ----", 1)[0]
    rescue_body = src.split("---- BEGIN GENERATED STATE RESCUE ----", 1)[1] \
        .split("---- END GENERATED STATE RESCUE ----", 1)[0]
    singles = [(r[0], r[1], int(r[2]), int(r[3]), r[4] or None,
                r[5], r[6]) for r in STATE_ROW_RE.findall(map_body)]
    if not singles:
        raise AssertionError("the generated state map holds no rows")
    rescues = [(r[0], r[1], int(r[2]), int(r[3]), r[4] or None,
                r[5], int(r[6]), int(r[7]), r[8] or None,
                r[9], r[10]) for r in RESCUE_ROW_RE.findall(rescue_body)]

    seen = []
    for blk, *_ in singles:
        if not seen or seen[-1] != blk:
            if blk in seen:
                raise AssertionError(
                    f"{blk}: the state map's rows for one block are "
                    "not contiguous; the lookup's binary search "
                    "needs grouped rows")
            seen.append(blk)
    if seen != sorted(seen):
        raise AssertionError(
            "the state map is not sorted by block; the lookup's "
            "binary search walks byte order")
    done, prev = set(), None
    for r in rescues:
        if r[:9] != prev:
            if r[:9] in done:
                raise AssertionError(
                    f"{r[0]}: the rescue rows for one answer are not "
                    "contiguous")
            if prev is not None:
                done.add(prev)
            prev = r[:9]

    cache = {}

    def raw_answer(name, states):
        key = (name, repr(sorted((k, type(v).__name__, py(v))
                                 for k, v in states.items())))
        if key not in cache:
            jn, props, unknown = ref._translate(name, states)
            cache[key] = (jn, {} if unknown else props)
        return cache[key]

    def cond_holds(vt, ival, sval, tag):
        if tag is None or TAG_CODE.get(type(tag)) != vt:
            return False
        if vt == 8:
            return str(tag.py_data) == str(sval)
        return int(tag.py_data) == ival

    def walk(name, states):
        props = {}
        for blk, st, vt, ival, sval, to, val in singles:
            if blk == name and cond_holds(vt, ival, sval,
                                          states.get(st)):
                props[to] = val
        for i, r in enumerate(rescues):
            blk, s1, t1, i1, sv1, s2, t2, i2, sv2, _, _ = r
            if blk != name:
                continue
            if not cond_holds(t1, i1, sv1, states.get(s1)):
                continue
            if not cond_holds(t2, i2, sv2, states.get(s2)):
                continue
            j = i
            while j < len(rescues) and rescues[j][:9] == r[:9]:
                props[rescues[j][9]] = rescues[j][10]
                j += 1
            break
        return props

    def cell_states(name, conds):
        """The block's spec defaults plus the conditions, in the
        spec's own tag spellings — the cells the table was measured
        at, re-derived from the spec rather than trusted."""
        spec_props, defaults = spec_of(ref, name)
        states = {k: sv(v) for k, v in defaults.items()}
        for st, vt, ival, sval in conds:
            tag = None
            if sval is not None:
                tag = amulet_nbt.StringTag(sval)
            elif spec_props and st in spec_props:
                for snbt in spec_props[st]:
                    t = sv(snbt)
                    if isinstance(t, NUMERIC) and int(t.py_data) == ival:
                        tag = t
                        break
            if tag is None:
                num = {1: amulet_nbt.ByteTag, 3: amulet_nbt.IntTag}
                if vt not in num:
                    raise AssertionError(
                        f"{name} {st}: no spec spelling for the "
                        f"row's value {ival}")
                tag = num[vt](ival)
            if TAG_CODE.get(type(tag)) != vt:
                raise AssertionError(
                    f"{name} {st}: the spec spells the value a "
                    f"{type(tag).__name__}, the row says tag type "
                    f"{vt} — the condition would never fire")
            states[st] = tag
        return states

    for blk, st, vt, ival, sval, to, val in singles:
        states = cell_states(blk, [(st, vt, ival, sval)])
        _, raw_want = raw_answer(blk, states)
        _, raw_def = raw_answer(blk, {})
        cwant, cdef = canon(raw_want), canon(raw_def)
        if to not in cwant:
            raise AssertionError(
                f"{blk} {st}={sval if sval is not None else ival}: "
                f"the reference's answer has no {to}, the row "
                "writes it")
        if raw_str(raw_want[to]) != val:
            raise AssertionError(
                f"{blk} {st}={sval if sval is not None else ival}: "
                f"the reference's raw {to} is "
                f"{raw_str(raw_want[to])!r}, the row says {val!r}")
        if cwant[to] == cdef.get(to) or cwant[to] == "false":
            raise AssertionError(
                f"{blk} {st}={sval if sval is not None else ival}: "
                f"{to}={val} is a value the delta rules say carries "
                "no row")
        got = canon(walk(blk, states))
        ok, key = props_match(got, cwant, cdef)
        if not ok:
            raise AssertionError(
                f"{blk} {st}={sval if sval is not None else ival}: "
                f"the walk composes {got} against the reference "
                f"{cwant} at {key}")

    for r in rescues:
        blk, s1, t1, i1, sv1, s2, t2, i2, sv2, _, _ = r
        states = cell_states(blk, [(s1, t1, i1, sv1),
                                   (s2, t2, i2, sv2)])
        _, raw_want = raw_answer(blk, states)
        _, raw_def = raw_answer(blk, {})
        got = canon(walk(blk, states))
        ok, key = props_match(got, canon(raw_want), canon(raw_def))
        if not ok:
            raise AssertionError(
                f"{blk} {s1}/{s2} rescue: the walk composes {got} "
                f"against the reference {canon(raw_want)} at {key}")
    return len(singles) + len(rescues)


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

        # ---- the records fabricated on the copy: the
        # overworld's half (D18, D19) then the dimensions' own
        # (D21) ------------------------------------------------
        # Three entries must arrive in the output's lists — the
        # mirror below recomputes them from these records. The
        # orphan's two entries and the payload that is not NBT must
        # be counted and never appear; the entry with no z drops
        # whole. The nether's records beside them: its chunk's own
        # entry carried, its orphan counted. WANT_REPORT pins every
        # count (4 carried, 5 dropped of the ticks). The biome
        # records answer to the same report and to the biome mirror
        # below: (0,0)'s cells carried per section, the orphan's and
        # the junk's counted and never carried, the nether's floor
        # strata carried in DIM-1.
        from amulet.libs.leveldb import LevelDB
        tdb = LevelDB(os.path.join(in_world, "db"))
        try:
            put_tick_record(tdb, 0, 0, 0, 0x33,
                '{currentTick:100,tickList:['
                '{blockState:{name:"minecraft:flowing_water",'
                'states:{liquid_depth:3},version:17959425},'
                'time:100L,x:5,y:65,z:5},'
                '{blockState:{name:"minecraft:sand",'
                'states:{sand_type:"red"},version:17959425},'
                'time:95L,x:6,y:64,z:6},'
                '{blockState:{name:"minecraft:stone",states:{},'
                'version:17959425},time:100L,x:7,y:64}]}')
            put_tick_record(tdb, 0, 0, 0, 0x3A,
                '{currentTick:100,tickList:['
                '{blockState:{name:"minecraft:magma",states:{},'
                'version:17959425},time:120L,x:7,y:64,z:7}]}')
            put_tick_record(tdb, 7, 7, 0, 0x33,
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
            put_biome_record(tdb, 0, 0, 0, biome_record_chunk())
            put_biome_record(tdb, 1, -2, 0,
                             biome_map_only_record())
            for cx, cz, payload in biome_records_orphans():
                put_biome_record(tdb, cx, cz, 0, payload)

            # ---- D21's own half: the nether and the end, and
            # the keys with nowhere to go -----------------------
            # The nether record at (0,0): columns 0..255 sit inside
            # the nether's own 256 and cross, its floor stratum
            # lands in the section at Y0, the one above stands
            # where no section exists (counted, never carried).
            put_biome_record(tdb, 0, 0, 1, nether_biome_record())
            # Two more subchunks from the fixture's own nether
            # payload: the nether's chunk at (1,-2) and the end's
            # at the same coordinates — the sort must hold three
            # dimensions apart even where their chunks share a
            # place.
            parity = tdb.get(chunk_key(0, 0, 0, 1))
            tdb.put(chunk_key(1, -2, 0, 1), parity)
            tdb.put(chunk_key(1, -2, 0, 2), parity)
            # ---- D25: pre-palette subchunks under the pre-1.18
            # prefix, and payloads that must refuse and count ------
            # Two flat payloads in the overworld's own (0,0), at
            # heights where no section and no biome array stand:
            # payload 5 (era 1.1) and payload 7 (era 1.2), each
            # carrying the cells that separate the eras' answers
            # from the 1.12 cap chain — sandstone data 3, jukebox,
            # a log — plus an id the numerical map does not hold,
            # which passes through as minecraft:numerical.
            put_flat(tdb, 0, 0, 0, 3, 5, {
                (0, 0, 0): (24, 3), (1, 0, 0): (84, 0),
                (2, 0, 0): (210, 5), (3, 0, 0): (17, 4),
            })
            put_flat(tdb, 0, 0, 0, 5, 7, {
                (0, 0, 0): (24, 3), (1, 0, 0): (84, 0),
                (2, 0, 0): (17, 1),
            })
            # Two payloads that must refuse and count once each
            # (D12): a flat one cut inside its planes, and a version
            # nothing decodes.
            tdb.put(chunk_key(0, 0, 6, 0, tag=0x2C),
                    bytes([0x05]) + bytes(4096))
            tdb.put(chunk_key(0, 0, 7, 0, tag=0x2C), bytes([0x0A]))
            # ---- D26: the older 768-byte biome spelling ----------
            # The end chunk's own record: its sampled columns cross
            # into the one section's biomes container — one column's
            # id unknown, its cells the reference's plains fallback
            # and counted — and its heightmap half rides the map's
            # own verdicts. The overworld (0,0) holds both
            # spellings: the Data3D record answers, the older table
            # counts beside it, never carried. The orphan at
            # (10,10) is one byte short of the shape and counts
            # once.
            put_2d_record(tdb, 1, -2, 2, biome_2d_record_chunk())
            put_2d_record(tdb, 0, 0, 0, biome_2d_precedence_record())
            put_2d_record(tdb, 10, 10, 0, biome_2d_junk_record())
            # One column at 300 — inside the overworld's 384, past
            # the nether's 256 — so the map drops while the floor
            # stratum's cells still cross (D20's halves per
            # dimension).
            put_biome_record(tdb, 1, -2, 1,
                             nether_dropped_map_record())
            # The nether orphan at (7,7): its cells and its valid
            # map count, never carry.
            put_biome_record(tdb, 7, 7, 1, nether_orphan_record())
            # The nether's tick records: one on the chunk that
            # converts, one orphan whose entries count.
            put_tick_record(tdb, 0, 0, 1, 0x33,
                '{currentTick:50,tickList:['
                '{blockState:{name:"minecraft:magma",states:{},'
                'version:17959425},time:110L,x:11,y:64,z:12}]}')
            put_tick_record(tdb, 6, 6, 1, 0x33,
                '{currentTick:20,tickList:['
                '{blockState:{name:"minecraft:stone",states:{},'
                'version:17959425},time:30L,x:101,y:64,z:101}]}')
            # The nether's block-entity documents: the container
            # whose chunk converts (carried), one whose chunk holds
            # no subchunks (counted) — neither may reach the
            # overworld's lists at the same coordinates — and D22's
            # families beside the first: a sign with both sides, a
            # spawner with its choice, the four cooks' times, a
            # gateway's portal, a lectern that answers with
            # position alone, a jukebox's record, and a bed whose
            # color lives in the document while its name lives in
            # the block (counted, not carried).
            put_be_record(tdb, 0, 0, 1,
                          '{id: "minecraft:chest", x: 3, y: 70, '
                          'z: 4}',
                          '{id: "Sign", x: 5, y: 71, z: 7, '
                          'IsWaxed: 1b, FrontText: {Text: "hello", '
                          'SignTextColor: -16777216}, BackText: '
                          '{Text: "", SignTextColor: -16777216}}',
                          '{id: "MobSpawner", x: 6, y: 71, z: 7, '
                          'Delay: 20s, MaxNearbyEntities: 6s, '
                          'MaxSpawnDelay: 800s, MinSpawnDelay: '
                          '200s, RequiredPlayerRange: 16s, '
                          'SpawnCount: 4s, SpawnRange: 4s, '
                          'EntityIdentifier: "minecraft:zombie", '
                          'DisplayEntityScale: 1.0f}',
                          '{id: "Campfire", x: 7, y: 71, z: 7, '
                          'ItemTime1: 1, ItemTime2: 2, ItemTime3: '
                          '3, ItemTime4: 4}',
                          '{id: "EndGateway", x: 8, y: 71, z: 7, '
                          'Age: 9, ExitPortal: [4, 5, 6], '
                          'EndGatewayBadPosChecked: 1b}',
                          '{id: "Lectern", x: 9, y: 71, z: 7}',
                          '{id: "Jukebox", x: 10, y: 71, z: 7, '
                          'RecordItem: {Name: '
                          '"minecraft:music_disc_cat", Count: 1b, '
                          'Damage: 0s, WasPickedUp: 0b}}',
                          '{id: "Bed", x: 11, y: 71, z: 7, '
                          'color: 5b}')
            put_be_record(tdb, 6, 6, 1,
                          '{id: "minecraft:chest", x: 99, y: 70, '
                          'z: 100}')
            # A dimension number beyond the game's three counts
            # `other`; its chunk version key counts `chunk_data`.
            tdb.put(chunk_key(99, 99, 0, 99), b"nowhere")
            tdb.put(_record_key(0, 0, 1, 0x36), b"chunkver")
            # The entity lists (D23): the nether's record of bytes
            # no document follows counts once — read now, not just
            # counted as a key — two documents under a chunk that
            # never converts count two (nothing crosses for
            # entities, so records count wherever they sit), and a
            # record holding nothing counts zero.
            tdb.put(_record_key(0, 0, 1, 0x32), b"entities")
            put_entity_record(tdb, 7, 7, 0,
                              '{identifier: "minecraft:cow", '
                              'Pos: [0.0f, 64.0f, 0.0f]}',
                              '{identifier: "minecraft:pig", '
                              'Pos: [1.0f, 64.0f, 1.0f]}')
            put_entity_record(tdb, 8, 8, 0)
            # One actor record and the digp index that names it
            # (D23): the record counts where it stands, the index
            # nowhere — its document IS the record, and counting
            # both would count one entity twice — and neither may
            # land in `other`.
            actor_id = bytes([1, 2, 3, 4, 5, 6, 7, 8])
            tdb.put(b"actorprefix" + actor_id, b"actor")
            tdb.put(b"digp"
                    + (7).to_bytes(4, "little", signed=True)
                    + (7).to_bytes(4, "little", signed=True),
                    actor_id)
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
        ref = Reference()
        subs = load_input(in_world, ref)
        mismatches = []
        compared = 0
        all_chunks = [(d, cx, cz) for d, cs in DIM_CHUNKS.items()
                      for cx, cz in cs]
        for dim, cx, cz in all_chunks:
            region = region_path(out, dim, cx, cz)
            tag = walk_chunk(region, cx, cz)
            if tag is None:
                raise AssertionError(
                    f"no output chunk (dim {dim}, {cx}, {cz})")
            stored_cys = sorted(
                cy for (d, a, b, cy) in subs
                if (d, a, b) == (dim, cx, cz))
            out_ys = sorted(s["Y"].py_data for s in tag["sections"])
            # Every stored subchunk has a position the map answers
            # with a block — the v1 legacy one since D24, the flat
            # ones since D25 — so all of them store a section; an
            # all-air one would not, and a payload that refused
            # decode is absent from both sides of this comparison.
            if out_ys != stored_cys:
                raise AssertionError(
                    f"chunk ({cx}, {cz}) stores sections {out_ys}, "
                    f"want every stored subchunk {stored_cys}")

            for cy in stored_cys:
                kind, layers, era = subs[(dim, cx, cz, cy)]
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
                            name, states, val = entry_of(
                                palette[int(arr[lx][ly][lz])])
                            if kind == "flat":
                                want_name, want, default = \
                                    ref.answer_flat(name, val, era)
                            else:
                                want_name, want, default = ref.answer(
                                    name, states, val)
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
        want_lists = {dim: tick_mirror(in_world, ref, dim)
                      for dim in DIM_CHUNKS}
        for dim, cx, cz in all_chunks:
            region = region_path(out, dim, cx, cz)
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
            if got != want_lists[dim][(cx, cz)]:
                raise AssertionError(
                    f"chunk ({cx}, {cz}) tick lists are {got}, "
                    f"want {want_lists[dim][(cx, cz)]}")
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
        want_biomes = {dim: biome_mirror(in_world, ref, dim)
                       for dim in DIM_CHUNKS}
        cells_checked = 0
        for dim, cx, cz in all_chunks:
            region = region_path(out, dim, cx, cz)
            tag = walk_chunk(region, cx, cz)
            if tag is None:
                raise AssertionError(
                    f"no biome output chunk ({cx}, {cz})")
            for sec in tag["sections"]:
                cy = sec["Y"].py_data
                want = want_biomes[dim][(cx, cz)].get(cy)
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
        if cells_checked != 448:
            raise AssertionError(
                f"checked {cells_checked} biome cells across the "
                f"sections the records reach, want 448 (the "
                f"overworld's 4 heights, the nether's 2 floors and "
                f"the end's one section since D26, 64 each)")
        checks += 1
        _log("ok — every section the record reaches carries the "
             "reference's biome for each of its 64 cells, packed "
             "cell by cell, and every other section keeps no key")

        # ---- heightmaps: the record's other half, packed Java's
        # way under the one name it can honestly answer (D20). The
        # reference reads Bedrock's columns into misc["height"] and
        # writes no height_mapC from them, so its own compound comes
        # back empty — this pins mcme's own spec, not a mirror: the
        # 256 fabricated columns, 9 bits to a value, 7 to a long
        # (37 of them, none spanning two), under MOTION_BLOCKING.
        # The maps that cross, spelled from their own
        # dimension's floor (D20, D21, D26): the overworld's record
        # spells 100 + i inside 384, the nether's i inside its own
        # 256, the end chunk's i beside its older-spelling cells —
        # and no key stands where no map crossed: the refused
        # maps (one column past each world's top) and the chunks
        # holding no record at all.
        want_hm = {
            (0, 0, 0): pack_heightmap([100 + i for i in range(256)]),
            (1, 0, 0): pack_heightmap([i for i in range(256)]),
            # the end chunk's map since D26: the older spelling's
            # half, columns spelling their own coordinates inside
            # the end's own 256
            (2, 1, -2): pack_heightmap([i for i in range(256)]),
        }
        for dim, cx, cz in all_chunks:
            region = region_path(out, dim, cx, cz)
            tag = walk_chunk(region, cx, cz)
            if tag is None:
                raise AssertionError(
                    f"no heightmap output chunk (dim {dim}, {cx}, "
                    f"{cz})")
            hmaps = tag.get("Heightmaps")
            want = want_hm.get((dim, cx, cz))
            if want is None:
                if hmaps is not None:
                    raise AssertionError(
                        f"chunk (dim {dim}, {cx}, {cz}) carries a "
                        f"Heightmaps for a map its dimension's "
                        f"height could not spell or held none")
                continue
            if hmaps is None:
                raise AssertionError(
                    f"chunk (dim {dim}, {cx}, {cz}) carries no "
                    f"Heightmaps")
            mb = hmaps.get("MOTION_BLOCKING")
            if mb is None:
                raise AssertionError(
                    "Heightmaps holds no MOTION_BLOCKING")
            got = [int(v) for v in mb.py_data]
            if got != want:
                at = (next((i for i in range(min(len(got),
                                                 len(want)))
                           if got[i] != want[i]), None)
                      if len(got) == len(want) else None)
                raise AssertionError(
                    f"MOTION_BLOCKING has {len(got)} longs, "
                    f"first divergence at {at}: {got} != "
                    f"{want}")
        checks += 1
        _log("ok — each dimension's map half crosses as 37 "
             "non-straddling longs under MOTION_BLOCKING, spelled "
             "from its own floor, and no key stands where no map "
             "crossed")

        # ---- block entities of this world: the nether's own
        # document, and the overworld's empty lists (D21) ----------
        # The same coordinates in two dimensions are two chunks:
        # the nether's document rides in DIM-1 with its mapping's
        # fields, the overworld's lists at (0,0) stay empty, and
        # the record whose chunk holds no subchunks is the report's
        # count, never a list.
        be_src = load_entity_docs(in_world)
        for dim, cx, cz in all_chunks:
            tag = walk_chunk(region_path(out, dim, cx, cz), cx, cz)
            if tag is None:
                raise AssertionError(
                    f"no block-entity output chunk (dim {dim}, "
                    f"{cx}, {cz})")
            got = [plain(e)
                   for e in (tag.get("block_entities") or [])]
            want = [want_java_doc(d)
                    for d in be_src.get((dim, cx, cz), [])
                    if carries(d)]
            if got != want:
                raise AssertionError(
                    f"chunk (dim {dim}, {cx}, {cz}) block_entities "
                    f"is {got}, want {want}")
        checks += 1
        _log("ok — the nether's documents arrive in DIM-1 with "
             "the mapping's fields — D22's families beside the "
             "container — and no list reaches across dimensions "
             "at shared coordinates")

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
        _log("ok — the entities fixture's report counts four "
             "carried documents (the nether's own beside the "
             "overworld's two), the sign among them with empty "
             "sides and the frame still dropped, and nothing "
             "skipped besides")

        # And the output's list against the source's records: each
        # carried document with its mapping's fields spelled out,
        # and no document the verdict drops.
        src_docs = load_entity_docs(be_in)
        # The store's own dimensions' chunks (D21): the nether's
        # at (0,0), written to DIM-1 beside the overworld's two.
        be_chunks = [(0, cx, cz) for cx, cz in CHUNKS] + [(1, 0, 0)]
        for dim, cx, cz in be_chunks:
            region = region_path(be_out, dim, cx, cz)
            tag = walk_chunk(region, cx, cz)
            if tag is None:
                raise AssertionError(
                    f"no entities output chunk (dim {dim}, {cx}, "
                    f"{cz})")
            bel = tag.get("block_entities")
            got = [plain(e) for e in (bel or [])]
            want = [want_java_doc(d)
                    for d in src_docs.get((dim, cx, cz), [])
                    if carries(d)]
            if got != want:
                raise AssertionError(
                    f"chunk (dim {dim}, {cx}, {cz}) block_entities "
                    f"is {got}, want {want}")
        checks += 1
        _log("ok — every carried document arrives with the mapping's "
             "fields (id, x/y/z, and beside them what each family "
             "spells: Items, timers, messages, SpawnData) and "
             "every dropped one stays away")

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

        # ---- the generated state map, against the reference -----
        pinned_state = pin_state_map(ref)
        checks += 1
        _log(f"ok — the state map's {pinned_state} rows all answer "
             "the way the reference answers at the cells they fire "
             "at, raw spellings included")

        # ---- the generated biome map, against the reference ---------
        pinned_biomes = pin_biome_map(ref)
        checks += 1
        _log(f"ok — the biome map's {pinned_biomes} ids all answer "
             "the way the reference answers, in the order the "
             "lookup walks")

        # ---- the generated legacy map, against the reference -------
        pinned_legacy = pin_legacy_map(ref)
        checks += 1
        _log(f"ok — the legacy map's {pinned_legacy} rows and their "
             "five defaults all answer the way the reference "
             "answers, in the order the lookup walks")

        # ---- the generated flat tables, against the reference ----
        pinned_flat = pin_flat_map(ref)
        checks += 1
        _log(f"ok — the flat map's {pinned_flat} era rows, all 256 "
              "numerical ids, both era stamps per payload version "
              "and every unrowed cell answer the way the reference "
              "answers, in the order the lookup walks")

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
