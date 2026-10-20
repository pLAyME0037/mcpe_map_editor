#!/usr/bin/env python3
"""
Cross-check: `mcme chunk mesh` — the builder's table for one block
position of a real chunk, with the world's OWN six neighbours
standing on its six sides.

The table's inputs are D33's faces, D35's rectangles, D36's colour
and D37's cull — and the cull has always been PER FACE in the
reference: each face reads the neighbour standing in its own
direction (CULL_MAP, chunk_builder_cy.pyx 49-55), so six faces can
six answers. The block-level verb could only carry one neighbour
for all six sides; this verb reads the six out of the world itself,
so the clauses answer per side (D38) — and the cross-sub-chunk
neighbour reads the block-level pin kept out come with them.

The fixture is the golden Java world, copied: the pattern is
written with `chunk set` (pinned by ref_java_blocks) and read back
by that pin's OWN region walk — the sector table walked by hand,
the palette unpacked here, none of mcme's code — so the grid the
reference's builder meshes is the world's own data. The grid is the
3x3x3 around the probe: a one-cell interior, so the builder meshes
the probe ALONE and every cull read lands on the world's cell
beside it — no leak, no field, the streams the probe's own.

The Bedrock half rides the same probes over the golden chunk
store: the pattern written with `chunk set`, which creates the
subchunks it needs, and every cell read back through amulet-core's
own leveldb loader — the payload under its key, the palette
through the reference's own reader — so the grid is the OTHER
format's own data, the cross-subchunk and cross-chunk cells
included.

The whole sub-chunk rides that ground too (D40): one section per
format drives `chunk submesh` over sub-chunk (0, 8, 0) — y 128 to
143, past every probe — filled by six `chunk fill` boxes: the
field of cull cubes, a quarter of it the cullface-less wall whose
1,024 cells keep every face (the opaque stream crossing the
builder's 10,008-row container split), the partial shell around
the translucent core, the air cavern, the second run. The grid is
18x18x18 — the cells with one layer of every neighbour around
them, the walk's own reads again — handed to `create_lod0_chunk`
at the sub-chunk's world origin, and every surviving row of every
cell is compared in the BUILDER's loop order (x, then y, then z),
each cell's rows against the stream its transparency feeds, the
stream totals whole across the split.

The states come with them (D41): the crafted pack gains one
blockstate whose three variant keys — axis=x, axis=y, and the
un-stated default — select three models it already holds, so the
texture table and its page stay D34's, and four probes carry
states through `chunk set`, through both walks' verbatim
spelling, and into the table: two variants, the default, and a
probe whose stateful NEIGHBOUR's variant decides its own
transparency — the clause firing against it standing or dropping
on that state being read. The states' sub-chunk — (0, 6, 0),
D38's probe pair among its cells — goes through `chunk submesh`
on both formats too, the batched read carrying the same specs.

The waterlogging the store holds and no mesh reads (D42): a
two-layer sub-chunk written straight into the scratch store's
own bytes — version 8's layer count, one waterlogged cell and a
plain neighbour beside it — is the fixture no write path of
mcme's reaches. The walk reports the MAIN block by name (the
loader turns the layers above into extra blocks and the builder
reads none of them), `extras_at` spells the layers beneath
verbatim, and the reference's streams over the sub-chunk hold
the main block's rows alone — the water's stream empty. mcme
answers the same over the whole sub-chunk through `chunk
submesh` and at the cell itself through `chunk mesh`, the cull
against the waterlogged neighbour firing on the main block's own
transparency: a read that surfaced the water anywhere would
change the totals or the standing faces, and both sides would
have to agree to be wrong together — neither is.

The spellings close behind them (D43): the spec format's
conventions, pinned where both readers meet it. Keys sort
case-insensitively — the store keeps its insertion order and
both spellings draw the same canonical text from it, the
mixed-case key among the probes proving the rule rather than
assuming it. Values are the store's OWN types, spelled the way
`chunk get` spells them back: strings quoted, a byte bare with
its b, an int bare, and the suffixes the writer carries for the
rest. Two probes per format carry them — one written unsorted
with mixed-case keys, one typed — through `chunk set`, both
walks' verbatim spelling, and the table, the variant still
selecting on axis so a read that mangled the states would mesh
the default and change every row.

The pin's own fidelity closes behind them (D47): every
reference answer over a store now starts from the walk's
PALETTE — the Properties compound (Bedrock's states) handed
over as the tags the format wrote, not the spelling drawn from
them, so nothing wraps a typed state in a StringTag before the
variant matcher reads it — the probe grids and the sub-chunk's
18-cube alike. The probes are a pair: a blockstate whose first
key names `bit=1b` as text, probed with the store's byte — no
key matches a byte (the tag types differ), the un-stated
default answering — and the same key probed with the store's
string "1b", which matches and takes the cull cube. D43's
record named the carve-out: the string path would have matched
"bit=1b" where the real reference does not. The pair pins the
difference instead of recording around it, through both
walks' tags and both formats' stores.

  MIXED        — one opaque cull cube with a different neighbour on
                 each side: opaque below, east and south cull those
                 faces away; a partial above, a translucent north
                 and air west keep theirs (the probe is no partner
                 for the chained pair) — three clause outcomes on
                 six sides at once, at the band's period height.
  PAIR         — the translucent cull cube with its own kind
                 standing north: that one face drops — neighbour
                 and mesh both translucent, the chained pair — and
                 the air sides keep: clause two, one side only, at
                 the band's peak.
  CHUNK EDGE   — the probe on x = 15 with opaque east and west:
                 the east face's neighbour stands in the NEXT
                 chunk (1, 0), stored by the golden world, read
                 across the chunk boundary on both sides.
  SECTION EDGE — the probe at y = 15 with opaque above: the up
                 face's neighbour stands in the next SECTION (y
                 16), created by the write — the next SUBCHUNK on
                 Bedrock, its own payload.
  NONE RINGED  — the cullface-less cube ringed by opaque on all
                 six sides: the none bucket never culls, per side.
  WORLD EDGE   — the probe on x = 31 with opaque on five sides
                 and the sixth facing the chunk neither store has
                 ever seen: east reads AIR — outside the stored
                 world — and that one face stands while five drop,
                 on both formats.
"""

from __future__ import annotations

import argparse
import json
import os
import shutil
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ref_atlasmesh as A          # the table pin's own machinery
import ref_java_blocks as J        # the region walk: no mcme code
import ref_bedrock_write as B      # the store's key and loader glue

FIXTURE_WORLD = J.FIXTURE_WORLD
FIXTURE_STORE = B.FIXTURE_WORLD

# The builder's per-side neighbour lookup — CULL_MAP,
# chunk_builder_cy.pyx 49-55: cull_id 1..6 answers up, down,
# north, east, south, west, and each face reads that offset from
# its own cell. mcme's direction names, its down-first order.
DIR_OFFSETS = {
    "down": (0, -1, 0), "up": (0, 1, 0), "north": (0, 0, -1),
    "east": (1, 0, 0), "south": (0, 0, 1), "west": (-1, 0, 0),
}

# The probes: the position, the block standing there, and the
# neighbours standing around it (a side not named is air, the
# golden sections' own answer). The blocks are the crafted pack's:
# ring_opaque is the opaque cull cube, ring_translucent its
# translucent twin, atlas_three not a full cube — partial — and
# atlas_cube the full cube without cullface, the none bucket's own.
PROBES = [
    ((8, 64, 8), "atlas_cull", {
        "down": "ring_opaque", "up": "atlas_three",
        "north": "ring_translucent", "east": "ring_opaque",
        "south": "atlas_cube", "west": "air"}),
    ((8, 96, 8), "ring_translucent", {"north": "ring_translucent"}),
    ((15, 70, 8), "atlas_cull",
     {"east": "ring_opaque", "west": "ring_opaque"}),
    ((8, 15, 8), "atlas_cull", {"up": "ring_opaque"}),
    ((8, 100, 8), "atlas_cube",
     {d: "ring_opaque" for d in DIR_OFFSETS}),
    # The world's edge: opaque on five sides, the sixth facing the
    # chunk neither store has — east reads AIR, and that one face
    # stands while five drop.
    ((31, 74, 8), "atlas_cull", {
        "down": "ring_opaque", "up": "ring_opaque",
        "north": "ring_opaque", "west": "ring_opaque",
        "south": "ring_opaque"}),
    # The states ride the table: the blockstate's variants select
    # three different models from the same name (D30's first-match
    # over D41's write and read), and one probe's stateful
    # NEIGHBOUR decides its own transparency — the clause firing
    # against it stands or drops on the neighbour's state being
    # read. Single-key states: one key spells the same on every
    # path, walk and SNBT alike.
    ((8, 103, 8), 'atlas_turn{axis: "x"}', {}),
    ((8, 106, 8), 'atlas_turn{axis: "y"}', {}),
    ((8, 109, 8), "atlas_turn", {}),
    ((8, 111, 8), "atlas_cull",
     {"east": 'atlas_turn{axis: "x"}'}),
    # The spellings themselves: written unsorted so the sorted
    # text is a claim, and typed so the walk's report is the
    # store's own tags — a byte bare with its suffix, an int
    # bare, exactly what `chunk get` prints back. The mixed-case
    # key pins the sort's own rule (case-insensitive, both
    # sides); the variant still selects on axis, so a read that
    # mangled the states would mesh the default and change every
    # row.
    ((8, 21, 8), 'atlas_turn{zz: "1", Zz: "2", axis: "x"}', {}),
    ((8, 24, 8), 'atlas_turn{axis: "y", bit: 1b, num: 5}', {}),
    # The typed TINT state (D46): power as the store's own byte —
    # the reference's lookup has no py_str to read it with,
    # AttributeError inside the apply's own try, caught, the
    # missing block answering; mcme's typed NULL answers the
    # same. The raise converged either way even when the pin's
    # side was the walk's text ("5b" missing from the table);
    # since D47 the lookup gets the byte itself, so the store's
    # own branch stands live.
    ((8, 27, 8), 'redstone_wire{power: 5b}', {}),
    # The variant key naming a TYPED state (D47): `bit=1b` as
    # TEXT — the store's byte can not equal the string (the tag
    # types differ, whichever way the matcher reads it), so the
    # default key answers; the store's STRING "1b" matches the
    # key and takes the cull cube. The pair pins the matcher on
    # types rather than spellings, read through the walks' own
    # tags the whole way — D43's record's carve-out closed.
    ((8, 18, 8), 'typed_key{bit: "1b"}', {}),
    ((8, 30, 8), 'typed_key{bit: 1b}', {}),
]

# The cell the world-edge probe rests on: x = 32 is chunk (2, 0),
# which the golden world's region file never stored and the golden
# store's keys never held. Both walks must answer it air.
EDGE_CELL = (32, 74, 8)

# The chunks the pattern intends to own: the golden world's stored
# pair, and the store's own on Bedrock. Every cell of every probe's
# 3x3x3 is written — the probe at the centre, the named sides on
# their axes, air everywhere else — so the grid the reference
# meshes is the pattern on both formats and the fixture's own
# blocks never leak into a palette the crafted pack cannot
# resolve. The world-edge probe's whole column at x = 32 is left
# alone: the chunk is the store's to answer, absent.
PATTERN_CHUNKS = {(0, 0), (1, 0)}

# The axis offsets' inverse: a cell's offset, to its side's name.
OFFSET_DIRS = {off: d for d, off in DIR_OFFSETS.items()}

# ---------------------------------------------------------------------------
# The whole sub-chunk's table (D40)
# ---------------------------------------------------------------------------

# The sub-chunk the whole-table probe fills: chunk (0, 0), y 128
# to 143 — past every probe, in the sections and subchunks none of
# them touch.
SUB = (0, 8, 0)
# The sub-chunk the state probes stand in (D41): y 96 to 111 —
# D38's probe pair at y 96 and 100 among its cells, the four
# state probes at y 103, 106, 109 and 111 below them.
STATE_SUB = (0, 6, 0)
# The sub-chunk the multi-layer fixture stands in (D42): cy 13,
# y 208 to 223 — past every probe, fill and pattern, and a key
# the golden store never held (cy 12 stays absent: it is one of
# usage's own refusal cases). The waterlogged cell and the plain
# neighbour it culls against, in world coordinates.
MULTI_SUB = (0, 13, 0)
WATER_CELL = (8, 216, 8)
CULL_CELL = (9, 216, 8)

# The pattern: six boxes through `chunk fill`, in write order —
# the field first, then the boxes that cut it. Every block is the
# crafted pack's:
#
#   the field        cull cubes everywhere, so every interior face
#                    culls against its own kind and only the
#                    boundaries can leak;
#   the wall         a quarter of the sub-chunk in the
#                    cullface-less cube — the none bucket never
#                    culls, so all 1,024 of its cells keep every
#                    face: the opaque stream's 36,864 rows, the
#                    flush's container split crossed three times
#                    over;
#   the shell        the partial block around the core, so the
#                    core's faces against it fire neither clause;
#   the core         translucent inside the shell — the chained
#                    pair among its own cells, its faces standing
#                    against the partial shell and the padding's
#                    air;
#   the cavern       a wall of air down one side: the field's
#                    faces against it stand, partial, neither
#                    clause;
#   the second run   translucent beside the shell and the field:
#                    the chained pair where its own cells meet,
#                    clause one where the field stands.
SUB_FILLS = [
    ((0, 128, 0), (15, 143, 15), "ring_opaque"),
    ((0, 128, 0), (15, 143, 3), "atlas_cube"),
    ((3, 131, 4), (12, 140, 15), "atlas_three"),
    ((4, 132, 5), (11, 139, 15), "ring_translucent"),
    ((0, 128, 4), (2, 143, 15), "air"),
    ((13, 128, 4), (15, 130, 15), "ring_translucent"),
]

# The split's ground: the builder's array holds ARRAY_VERT_COUNT
# = 10,008 rows and flushes to a new container past it. The wall
# alone guarantees the opaque stream crosses that line — a seam
# that lost or doubled a row would shift every row behind it, and
# the per-row walk over the flat stream would say so.
SPLIT_MIN = 10_008

# What the fixture rests on: the crafted pack's own classifications,
# the reference's answers (D32's rule through its manager).
FIXTURE_FLAGS = {
    "ring_opaque": 0, "ring_translucent": 1, "atlas_cull": 0,
    "atlas_cube": 0, "atlas_three": 2, "air": 2,
}


def _log(msg: str) -> None:
    print(f"ref_chunkmesh: {msg}", file=sys.stderr)


class WorldWalk:
    """The world's blocks through ref_java_blocks' own walk: the
    chunk each position falls in, read once, absent chunks air."""

    def __init__(self, region_root: str):
        self.region_root = region_root
        self.tags: dict[tuple[int, int], object] = {}

    def _chunk(self, cx: int, cz: int):
        tag = self.tags.get((cx, cz))
        if tag is None:
            rx, rz = cx >> 5, cz >> 5
            path = os.path.join(self.region_root, f"r.{rx}.{rz}.mca")
            if not os.path.exists(path):
                # No region file for the column at all: the world's
                # own edge, air — the rule mcme reads a neighbour
                # chunk by (a sub-chunk's grid reaches the chunks
                # beside it, and the fixture world starts at 0,0).
                self.tags[(cx, cz)] = False
                return False
            tag = J.walk_chunk(path, cx, cz)
            self.tags[(cx, cz)] = tag if tag is not None else False
        return tag

    def block_at(self, x: int, y: int, z: int) -> str:
        tag = self._chunk(x >> 4, z >> 4)
        if not tag:
            return J.AIR
        return J.block_at(tag, x, y, z)

    def tags_at(self, x: int, y: int, z: int) -> tuple[str, dict]:
        """The block at a position as the STORE holds it (D47):
        the bare name and the properties as their own tags — the
        palette's compound handed over, no text round-trip
        between it and the reference's lookup."""
        tag = self._chunk(x >> 4, z >> 4)
        entry = J.block_entry(tag, x, y, z) if tag else None
        if entry is None:
            return J.AIR.split(":", 1)[-1], {}
        name = entry["Name"].py_data.split(":", 1)[-1]
        props = entry.get("Properties")
        if props is None or len(props) == 0:
            return name, {}
        return name, dict(props)


class BedrockWalk:
    """The world's blocks through amulet-core's own leveldb loader:
    the subchunk payload under its key, every layer through the
    reference's reader (ref_bedrock_write's own glue), absent
    payloads air. The MAIN block — layer zero — is what the walk
    reports by name: the loader turns the layers above it into
    extra blocks and the builder reads none of them (D42), so the
    main block is the world the mesh sees. `extras_at` reports
    the layers beneath, verbatim."""

    def __init__(self, db_path: str):
        from amulet.libs.leveldb import LevelDB
        from amulet.level.formats.leveldb_world.interface.chunk.leveldb_8 import (  # noqa: E501
            LevelDB8Interface,
        )
        self.db_path = db_path
        self.LevelDB = LevelDB
        self.iface = LevelDB8Interface()
        self.layers: dict[tuple[int, int, int], object] = {}

    def _layer(self, cx: int, cz: int, cy: int):
        key = (cx, cz, cy)
        if key not in self.layers:
            db = self.LevelDB(self.db_path)
            try:
                try:
                    payload = db.get(B.chunk_key(cx, cz, cy))
                except KeyError:
                    payload = None
            finally:
                db.close()
            self.layers[key] = (
                None if payload is None
                else B.decode_payload(self.iface, payload)[1])
        return self.layers[key]

    def block_at(self, x: int, y: int, z: int) -> str:
        """The MAIN block — layer zero, the one the builder
        meshes. The store's own spelling, verbatim — the palette
        carries the namespace and the reader prints it, exactly
        what `chunk get` shows back."""
        main, _extras = self._cell(x, y, z)
        if main is None:
            return J.AIR
        return self.spec_of(main)

    def extras_at(self, x: int, y: int, z: int) -> list[str]:
        """The layers under the main block's, verbatim — the
        store's own waterlogging, spelled the same way. The
        universal format drops an air extra (the loader's own
        filter); the walk reports the store as it stands."""
        main, extras = self._cell(x, y, z)
        if main is None:
            return []
        return [self.spec_of(e) for e in extras]

    def _cell(self, x: int, y: int, z: int):
        layers = self._layer(x >> 4, z >> 4, y >> 4)
        if not layers:
            return None, []
        lx, ly, lz = x & 15, y & 15, z & 15
        entries = [
            palette[B.block_at(blocks, lx, ly, lz)]
            for blocks, palette in layers]
        return entries[0], entries[1:]

    @staticmethod
    def spec_of(entry) -> str:
        name = entry["name"].py_data
        states = entry.get("states")
        if states is None or len(states) == 0:
            return name
        items = ", ".join(
            f"{k}: {J.snbt_value(v)}"
            for k, v in sorted(states.items(),
                               key=lambda kv: kv[0].lower()))
        return f"{name}{{{items}}}"

    def tags_at(self, x: int, y: int, z: int) -> tuple[str, dict]:
        """The cell's main block as the store holds it (D47): the
        bare name and the states as their own tags — the palette's
        compound handed over, no text round-trip between it and
        the reference's lookup."""
        main, _extras = self._cell(x, y, z)
        if main is None:
            return J.AIR.split(":", 1)[-1], {}
        name = main["name"].py_data.split(":", 1)[-1]
        states = main.get("states")
        if states is None or len(states) == 0:
            return name, {}
        return name, dict(states)


def around(walk, x: int, y: int, z: int) -> dict[tuple, tuple]:
    """The 3x3x3's cells around a position, keyed by their offset
    from its centre — the grid the builder is handed: the walk's
    own spelling beside the walk's own tags (D47), the palette
    built from the second and the slot key drawn from the first,
    the store's types standing the whole way to the matcher."""
    cells = {}
    for dx in (-1, 0, 1):
        for dy in (-1, 0, 1):
            for dz in (-1, 0, 1):
                px, py, pz = x + dx, y + dy, z + dz
                cells[(dx, dy, dz)] = (
                    walk.block_at(px, py, pz), *walk.tags_at(px, py, pz))
    return cells


def ref_world_table(opack, walk: WorldWalk, x: int, y: int, z: int):
    """The builder's own table for the block at world (x, y, z)
    with the world's 3x3x3 around it: a one-cell interior, so the
    builder meshes the probe alone and the streams hold only its
    rows. The chunk offset is the probe's world position and the
    sub-chunk's own y is 0 — dy is their sum (chunk_builder_cy.pyx
    358-366) — and the loop adds the offset to every row's
    position (310-313). The palette gets a fresh list (the pack
    caches by id()) and the cache is cleared: id reuse between
    probes would answer with stale geometry."""
    import numpy

    cells = around(walk, x, y, z)
    palette = []
    index: dict[str, int] = {}
    grid = numpy.zeros((3, 3, 3), numpy.uint32)
    for (dx, dy, dz), (spec, name, props) in cells.items():
        if spec not in index:
            index[spec] = len(palette)
            palette.append(A.make_block_tags(name, props))
        grid[dx + 1, dy + 1, dz + 1] = index[spec]
    opack.block_model_manager = {}
    opaque, trans = A._REF.cb.create_lod0_chunk(
        opack, numpy.array([x, y, z], numpy.int64), [(grid, 0)],
        palette, 12)
    rows_o = numpy.frombuffer(opaque[0], numpy.float32).reshape(-1, 12)
    rows_t = numpy.frombuffer(trans[0], numpy.float32).reshape(-1, 12)
    return rows_o, rows_t


def mcme_chunk_mesh(mcme: str, world: str, packdir: str,
                    x: int, y: int, z: int) -> dict:
    rc, out, err = A.run(mcme, "chunk", "mesh", world, packdir,
                         str(x), str(y), str(z), "--json")
    if rc != 0:
        raise AssertionError(
            f"chunk mesh ({x}, {y}, {z}): exit {rc}, "
            f"stderr {err.strip()!r}")
    try:
        return json.loads(out)
    except json.JSONDecodeError:
        raise AssertionError(
            f"chunk mesh ({x}, {y}, {z}): unparseable JSON {out!r}")


def write_pattern(mcme: str, world: str) -> int:
    """The probes' own 3x3x3, written with the world's own verb:
    the probe at the centre, the named sides on their axes, air
    everywhere else — the full `minecraft:name` spelling the stores
    hold, so both formats read the pattern back verbatim. Cells
    outside the pattern's chunks are left alone: the world's edge
    is the store's to answer. On Bedrock the write creates the
    subchunks it needs."""
    wrote = 0
    for (x, y, z), block, sides in PROBES:
        cells = []
        for dx in (-1, 0, 1):
            for dy in (-1, 0, 1):
                for dz in (-1, 0, 1):
                    cx, cy, cz = x + dx, y + dy, z + dz
                    if (cx >> 4, cz >> 4) not in PATTERN_CHUNKS:
                        continue
                    if (dx, dy, dz) == (0, 0, 0):
                        spec = block
                    else:
                        spec = sides.get(
                            OFFSET_DIRS.get((dx, dy, dz)), "air")
                    cells.append((cx, cy, cz, spec))
        for cx, cy, cz, spec in cells:
            full = A.mcme_spec(spec, {})
            rc, out, err = A.run(mcme, "chunk", "set", world,
                                 str(cx), str(cy), str(cz), full)
            if rc != 0 or out != "":
                raise AssertionError(
                    f"set {full} at ({cx}, {cy}, {cz}): exit {rc} "
                    f"stdout {out!r} ({err.strip()})")
            wrote += 1
    return wrote


def check_edge(walk, flavor: str, checks: int) -> int:
    """The world-edge cell the sixth probe rests on: x = 32 is
    chunk (2, 0), which the golden world's region file never
    stored and the golden store's keys never held — both walks
    answer air, mcme reads the same, and the edge rule stands."""
    ex, ey, ez = EDGE_CELL
    got = walk.block_at(ex, ey, ez)
    if got != J.AIR:
        raise AssertionError(
            f"{flavor}: the edge cell ({ex}, {ey}, {ez}) answers "
            f"{got!r}, want air — the chunk neither store has")
    return checks + 1


def check_world_probe(mcme, packdir, mgr, opack, page: int,
                      world: str, walk: WorldWalk, x: int, y: int,
                      z: int, checks: int, why: str) -> int:
    """The reference answers its builder's table over the world's
    own 3x3x3; mcme answers the same document plus the table's
    columns, reading the six neighbours out of the world itself.
    Every row must agree, bucket by bucket, the survivors the
    per-side clauses' own."""
    probe_spec = walk.block_at(x, y, z)
    name, props = walk.tags_at(x, y, z)
    want = A.ref_get_model(mgr, name, props, tags=True)
    got = mcme_chunk_mesh(mcme, world, packdir, x, y, z)

    # The position context the verb names itself by.
    if (got["x"], got["y"], got["z"]) != (x, y, z):
        raise AssertionError(
            f"{why}: coordinates {(got['x'], got['y'], got['z'])}, "
            f"want {(x, y, z)}")
    if got["block"] != probe_spec:
        raise AssertionError(
            f"{why}: block {got['block']!r}, the world holds "
            f"{probe_spec!r}")

    # The mesh document: the block's own, the table columns and the
    # position context stripped off it. Fresh dicts: the strip must
    # not eat the columns below.
    got_mesh = {k: v for k, v in got.items()
                if k not in ("size", "block", "x", "y", "z")}
    got_mesh["dirs"] = {
        dname: {k: v for k, v in d.items()
                if k not in ("bounds", "auvs", "tints")}
        for dname, d in got.get("dirs", {}).items()}
    d = A.mesh_diff(got_mesh, want, probe_spec)
    if d:
        raise AssertionError(f"{why}: {d}")

    # The page the coordinates are in.
    if got["size"] != page:
        raise AssertionError(
            f"{why}: {probe_spec}: page {got['size']}, reference {page}")

    # The builder's table over the world's own cells: the probe at
    # loop zero, its rows the streams' own — a one-cell interior
    # leaks nothing, so the other stream is empty and the total is
    # the survivors alone.
    probe_t = want["transparent"]
    rows_o, rows_t = ref_world_table(opack, walk, x, y, z)
    rows, other = (rows_t, rows_o) if probe_t == 1 else (rows_o, rows_t)
    if len(other) != 0:
        raise AssertionError(
            f"{why}: {probe_spec}: {len(other)} rows in the stream "
            "the other side uses, want 0 — the interior is one cell")

    # The skip clauses, transcribed per side (chunk_builder_cy.pyx
    # 282-289): a face with no cullface never culls; a face drops
    # when the neighbour on ITS side is opaque, or when that
    # neighbour and the mesh are both translucent — transparency
    # alone, nothing else read. Each side's answer is the world's
    # own block through the reference's classification.
    n_t = {}
    for dname, (dx, dy, dz) in DIR_OFFSETS.items():
        n_name, n_props = walk.tags_at(x + dx, y + dy, z + dz)
        n_t[dname] = A.ref_get_model(
            mgr, n_name, n_props, tags=True)["transparent"]

    def culled_out(dname: str) -> bool:
        return dname != "none" and (
            n_t[dname] == 0 or
            (n_t[dname] == 1 and probe_t == 1))

    total = sum(len(v["faces"]) for dname, v in want["dirs"].items()
                if not culled_out(dname))
    if len(rows) != total:
        raise AssertionError(
            f"{why}: {probe_spec}: the builder answered {len(rows)} "
            f"rows, the surviving mesh has {total} "
            f"(sides {n_t}, probe {probe_t})")

    # Bucket by bucket in the builder's emission order: the rows are
    # one flat stream, and the mesh's face counts are the slice.
    walked = 0
    for dname in A.CULL_ORDER:
        if dname not in want["dirs"]:
            continue
        mesh_n = len(want["dirs"][dname]["faces"])
        n = 0 if culled_out(dname) else mesh_n
        g = got["dirs"].get(dname)
        if g is None:
            raise AssertionError(f"{why}: {probe_spec}: mcme has "
                                 f"no {dname}")
        if n == 0:
            # The bucket drops whole: the table's keys do not
            # appear beside the mesh document — there are no rows
            # for them to name.
            for key in ("bounds", "auvs", "tints"):
                if key in g:
                    raise AssertionError(
                        f"{why}: {probe_spec} {dname}: the cull "
                        f"dropped the bucket, mcme still answered "
                        f"{key}")
            continue  # the stream holds no rows for it either
        if len(g["bounds"]) != 4 * n:
            raise AssertionError(
                f"{why}: {probe_spec} {dname}: {len(g['bounds'])} "
                f"bound values for {n} rows")
        if len(g["auvs"]) != 2 * n:
            raise AssertionError(
                f"{why}: {probe_spec} {dname}: {len(g['auvs'])} auv "
                f"values for {n} rows")
        if len(g["tints"]) != 3 * n:
            raise AssertionError(
                f"{why}: {probe_spec} {dname}: {len(g['tints'])} "
                f"tint values for {n} rows")
        verts, tverts, faces = g["verts"], g["tverts"], g["faces"]
        for r in range(n):
            row = rows[walked + r]
            fi = faces[r]
            pos, tv, rect = row[0:3], row[3:5], row[5:9]
            for seq, want_seq, what in (
                # The builder's rows ride the world offset on all
                # THREE axes (dx + x, dy + y, dz + z — pyx
                # 310-313); the document mcme prints is the
                # block's own.
                (pos, (verts[3 * fi] + x, verts[3 * fi + 1] + y,
                       verts[3 * fi + 2] + z), "position"),
                (tv, tverts[2 * fi:2 * fi + 2], "tvert"),
                (rect, g["bounds"][4 * r:4 * r + 4], "bounds"),
            ):
                d = A.f32_diff(seq, want_seq,
                               f"{probe_spec} {dname} row {r} {what}")
                if d:
                    raise AssertionError(f"{why}: {d}")
            # The mapping, from the builder's own row: raw
            # coordinate and rectangle, through the shader's
            # arithmetic, must be the auv mcme printed.
            d = A.f32_diff(g["auvs"][2 * r:2 * r + 2],
                           A.shader_auv(tv, rect),
                           f"{probe_spec} {dname} row {r} auv")
            if d:
                raise AssertionError(f"{why}: {d}")
            # The tint the builder folded — the mesh's tint times
            # the direction's brightness times the band shade at
            # the row's world height, the offset riding it — must
            # be the tints mcme printed.
            d = A.f32_diff(g["tints"][3 * r:3 * r + 3], row[9:12],
                           f"{probe_spec} {dname} row {r} tint")
            if d:
                raise AssertionError(f"{why}: {d}")
        walked += n
    if walked != len(rows):
        raise AssertionError(
            f"{why}: {probe_spec}: {len(rows) - walked} rows past "
            "the mesh")
    return checks + 1


def write_subchunk(mcme: str, world: str) -> int:
    """The sub-chunk's pattern, six boxes through the world's own
    fill verb."""
    filled = 0
    for (x1, y1, z1), (x2, y2, z2), block in SUB_FILLS:
        full = A.mcme_spec(block, {})
        rc, out, err = A.run(mcme, "chunk", "fill", world,
                             str(x1), str(y1), str(z1),
                             str(x2), str(y2), str(z2), full)
        if rc != 0 or out != "":
            raise AssertionError(
                f"fill {full} ({x1}, {y1}, {z1})..({x2}, {y2}, "
                f"{z2}): exit {rc} stdout {out!r} ({err.strip()})")
        filled += (x2 - x1 + 1) * (y2 - y1 + 1) * (z2 - z1 + 1)
    return filled


def subgrid_specs(walk, sub=SUB) -> dict[tuple, tuple]:
    """Every cell the sub-chunk's grid holds, by world position —
    the walk's reads once: its own spelling beside its own tags
    (D47), the comparison drawing the first and the palette the
    second."""
    origin = tuple(c * 16 for c in sub)
    specs = {}
    for i in range(18):
        for j in range(18):
            for k in range(18):
                p = (origin[0] + i - 1, origin[1] + j - 1,
                     origin[2] + k - 1)
                specs[p] = (walk.block_at(*p), *walk.tags_at(*p))
    return specs


def ref_subchunk(opack, specs: dict[tuple, str], sub=SUB):
    """The builder's own answer for the whole sub-chunk: the
    18x18x18 grid — the cells with one layer of every neighbour
    around them — handed to `create_lod0_chunk` at the sub-chunk's
    world origin, and the two streams it answers, flat: the
    flush's containers are concatenated before the reference ever
    returns them, so the split lives in the totals."""
    import numpy

    origin = tuple(c * 16 for c in sub)
    grid = numpy.zeros((18, 18, 18), numpy.uint32)
    palette = [A.make_block_tags("air", {})]
    index = {"minecraft:air": 0}
    for i in range(18):
        for j in range(18):
            for k in range(18):
                spec, name, props = specs[(origin[0] + i - 1,
                                           origin[1] + j - 1,
                                           origin[2] + k - 1)]
                slot = index.get(spec)
                if slot is None:
                    palette.append(A.make_block_tags(name, props))
                    slot = index[spec] = len(palette) - 1
                grid[i, j, k] = slot
    opack.block_model_manager = {}
    opaque, trans = A._REF.cb.create_lod0_chunk(
        opack, numpy.array(list(origin), numpy.int64),
        [(grid, 0)], palette, 12)
    rows_o = numpy.frombuffer(opaque[0], numpy.float32).reshape(-1, 12)
    rows_t = numpy.frombuffer(trans[0], numpy.float32).reshape(-1, 12)
    return rows_o, rows_t


def check_subchunk(mcme, packdir, mgr, opack, page: int, world: str,
                   walk, checks: int, why: str, sub=SUB) -> int:
    """The whole sub-chunk, both sides: the reference's streams
    over the walk's grid, mcme's cells walked in the BUILDER'S own
    loop order — x, then y, then z — every surviving row of every
    cell against the stream its transparency feeds, the cell's
    world position under the row on all three axes, and the
    split's totals whole. The split's assertion belongs to the
    sub-chunk that carries the wall; a quieter one just compares."""
    origin = tuple(c * 16 for c in sub)
    specs = subgrid_specs(walk, sub)
    rows_o, rows_t = ref_subchunk(opack, specs, sub)
    if sub == SUB and len(rows_o) < SPLIT_MIN:
        raise AssertionError(
            f"{why}: the opaque stream holds {len(rows_o)} rows, "
            f"short of the {SPLIT_MIN} that split it — the flush "
            "never fired and the seam stays unpinned")

    got = A.mcme_submesh(mcme, packdir, world,
                         *(str(v + 4) for v in origin))
    if got["size"] != page:
        raise AssertionError(
            f"{why}: page {got['size']}, reference {page}")
    if got["subchunk"] != {"x": sub[0], "y": sub[1], "z": sub[2]}:
        raise AssertionError(
            f"{why}: the answer names sub-chunk {got['subchunk']}, "
            f"want {sub}")
    if (got["opaque"], got["translucent"]) != (
            len(rows_o), len(rows_t)):
        raise AssertionError(
            f"{why}: mcme's stream totals are {got['opaque']} and "
            f"{got['translucent']}, the builder answered "
            f"{len(rows_o)} and {len(rows_t)}")

    cells = {(c["x"], c["y"], c["z"]): c for c in got["cells"]}
    if len(cells) != len(got["cells"]):
        raise AssertionError(f"{why}: two cells at one position")

    docs: dict[str, dict] = {}

    def doc_for(cell) -> dict:
        spec, name, props = cell
        d = docs.get(spec)
        if d is None:
            d = docs[spec] = A.ref_get_model(mgr, name, props,
                                             tags=True)
        return d

    cursor = {"opaque": 0, "translucent": 0}
    answered = []
    for cx in range(16):
        for cy in range(16):
            for cz in range(16):
                wx = origin[0] + cx
                wy = origin[1] + cy
                wz = origin[2] + cz
                spec, _name, _props = specs[(wx, wy, wz)]
                doc = doc_for(specs[(wx, wy, wz)])
                self_t = doc["transparent"]
                stream = ("translucent" if self_t == 1 else "opaque")
                rows = rows_t if self_t == 1 else rows_o
                cell = cells.get((wx, wy, wz))
                if cell is not None and cell["block"] != spec:
                    raise AssertionError(
                        f"{why}: cell ({wx}, {wy}, {wz}): block "
                        f"{cell['block']!r}, the world holds "
                        f"{spec!r}")
                walked = 0
                for dname in A.CULL_ORDER:
                    if dname == "none":
                        culled = False     # the test's own gate:
                        # no cull_id, no clause (pyx 282)
                    else:
                        dx, dy, dz = DIR_OFFSETS[dname]
                        n_t = doc_for(specs[
                            (wx + dx, wy + dy, wz + dz)]
                            )["transparent"]
                        # the skip clauses over the grid's own
                        # neighbour (pyx 287-289)
                        culled = (n_t == 0
                                  or (n_t == 1 and self_t == 1))
                    faces = doc["dirs"].get(dname, {}).get(
                        "faces", [])
                    n = 0 if culled else len(faces)
                    g = cell["dirs"].get(dname) if cell else None
                    if n == 0:
                        if g is not None:
                            raise AssertionError(
                                f"{why}: cell ({wx}, {wy}, {wz}) "
                                f"{dname}: the cull left no rows, "
                                "mcme still answered the bucket")
                        continue
                    if g is None:
                        raise AssertionError(
                            f"{why}: cell ({wx}, {wy}, {wz}) "
                            f"{dname}: mcme has no bucket for {n} "
                            "rows")
                    if (len(g["bounds"]), len(g["auvs"]),
                            len(g["tints"])) != (
                            4 * n, 2 * n, 3 * n):
                        raise AssertionError(
                            f"{why}: cell ({wx}, {wy}, {wz}) "
                            f"{dname}: {len(g['bounds'])}/"
                            f"{len(g['auvs'])}/{len(g['tints'])} "
                            f"values for {n} rows")
                    base = cursor[stream] + walked
                    ddoc = doc["dirs"][dname]
                    verts, tverts = ddoc["verts"], ddoc["tverts"]
                    for r in range(n):
                        row = rows[base + r]
                        fi = faces[r]
                        pos, tv, rect = row[0:3], row[3:5], row[5:9]
                        for seq, want_seq, what in (
                            # The builder's rows ride the cell's
                            # world position on all three axes.
                            (pos, (verts[3 * fi] + wx,
                                   verts[3 * fi + 1] + wy,
                                   verts[3 * fi + 2] + wz),
                             "position"),
                            (tv, tverts[2 * fi:2 * fi + 2], "tvert"),
                            (rect, g["bounds"][4 * r:4 * r + 4],
                             "bounds"),
                        ):
                            d = A.f32_diff(
                                seq, want_seq,
                                f"cell ({wx}, {wy}, {wz}) {dname} "
                                f"row {r} {what}")
                            if d:
                                raise AssertionError(f"{why}: {d}")
                        d = A.f32_diff(
                            g["auvs"][2 * r:2 * r + 2],
                            A.shader_auv(tv, rect),
                            f"cell ({wx}, {wy}, {wz}) {dname} "
                            f"row {r} auv")
                        if d:
                            raise AssertionError(f"{why}: {d}")
                        d = A.f32_diff(
                            g["tints"][3 * r:3 * r + 3], row[9:12],
                            f"cell ({wx}, {wy}, {wz}) {dname} "
                            f"row {r} tint")
                        if d:
                            raise AssertionError(f"{why}: {d}")
                    walked += n
                cursor[stream] += walked
                if walked:
                    answered.append((wx, wy, wz))

    # mcme's cells come in the builder's own emission order — x,
    # then y, then z — the cells with rows among them.
    order = [(c["x"], c["y"], c["z"]) for c in got["cells"]]
    if order != answered:
        at = next((i for i, (a, b) in enumerate(zip(order, answered))
                   if a != b), min(len(order), len(answered)))
        raise AssertionError(
            f"{why}: mcme's {len(order)} cells are not the "
            f"builder's loop order over {len(answered)}: first "
            f"difference at {at}")
    if (cursor["opaque"], cursor["translucent"]) != (
            len(rows_o), len(rows_t)):
        raise AssertionError(
            f"{why}: the streams hold {len(rows_o)} and "
            f"{len(rows_t)} rows, the cells walked "
            f"{cursor['opaque']} and {cursor['translucent']}")
    return checks + 1


def _pal_entry(name: str, version: int):
    from amulet_nbt import CompoundTag, IntTag, NamedTag, StringTag
    return NamedTag(CompoundTag({
        "name": StringTag(name),
        "states": CompoundTag(),
        "version": IntTag(version)}), "")


def _encode_layer(filled: dict[tuple[int, int, int], int],
                  names: list[str]) -> bytes:
    """One storage layer, in the shape `_load_palette_blocks`
    reads: the reference's own packed-array encoder over the
    cells, the palette length, and the entries' NBT — air at
    index zero with the loader's own default air version."""
    import numpy
    import struct
    from amulet.level.formats.leveldb_world.interface.chunk.leveldb_8 import (  # noqa: E501
        LevelDB8Interface,
    )
    arr = numpy.zeros((16, 16, 16), numpy.uint32)
    for (x, y, z), slot in filled.items():
        arr[x, y, z] = slot
    out = LevelDB8Interface._encode_packed_array(arr)
    out += struct.pack("<I", len(names))
    for name in names:
        version = 17694723 if name == J.AIR else B.BLOCK_VERSION
        out += _pal_entry(
            name, version).save_to(compressed=False, little_endian=True)
    return out


def write_two_layers(bworld: str) -> int:
    """The fixture no writer of mcme's reaches: a two-layer
    sub-chunk written straight into the scratch store's own bytes
    — version 8, the layer count the reference's loader reads as
    a main block and one extra. The sub-chunk is one the golden
    store never held (asserted before the write, its own content
    safe), and the fixture stands where no probe, fill or pattern
    reaches. Returns the cells the second layer waterlogs."""
    import struct
    from amulet.libs.leveldb import LevelDB
    key = B.chunk_key(MULTI_SUB[0], MULTI_SUB[2], MULTI_SUB[1])
    main = _encode_layer(
        {(8, 8, 8): 1, (9, 8, 8): 2},
        [J.AIR, "minecraft:ring_opaque", "minecraft:atlas_cull"])
    extra = _encode_layer(
        {(8, 8, 8): 1}, [J.AIR, "minecraft:ring_translucent"])
    payload = b"\x08" + struct.pack("B", 2) + main + extra
    db = LevelDB(os.path.join(bworld, "db"))
    try:
        try:
            db.get(key)
        except KeyError:
            pass
        else:
            raise AssertionError(
                "the multi-layer fixture: the store already holds "
                f"sub-chunk {MULTI_SUB} — the golden world's own "
                "content would stand over")
        db.put(key, payload)
    finally:
        db.close()
    return 1


def check_multi_layer(mcme, packdir, mgr, opack, page: int, world: str,
                      bwalk, checks: int, why: str) -> int:
    """The waterlogging the store holds and no mesh reads: the
    fixture's cell carries a second layer the walk spells
    verbatim, the reference meshes the main block alone — the
    translucent stream whole empty — and mcme answers the same
    over the sub-chunk and at the cell itself, the cull against
    the waterlogged neighbour firing on the main block's own
    transparency."""
    wx, wy, wz = WATER_CELL
    main = bwalk.block_at(wx, wy, wz)
    if main != "minecraft:ring_opaque":
        raise AssertionError(
            f"{why}: the fixture's cell answers {main!r}, want "
            "'minecraft:ring_opaque' — layer zero's own block")
    extras = bwalk.extras_at(wx, wy, wz)
    if extras != ["minecraft:ring_translucent"]:
        raise AssertionError(
            f"{why}: the cell's second layer spells {extras!r}, "
            "want ['minecraft:ring_translucent'] — the store's "
            "water, verbatim")
    cx, cy, cz = CULL_CELL
    if bwalk.block_at(cx, cy, cz) != "minecraft:atlas_cull":
        raise AssertionError(
            f"{why}: the plain neighbour lost its block")
    cextras = bwalk.extras_at(cx, cy, cz)
    if cextras != [J.AIR]:
        raise AssertionError(
            f"{why}: the neighbour's second layer spells "
            f"{cextras!r}, want air — the store pads the layer "
            "whole, and the loader drops an air extra")

    # The reference over the walk's grid: the main block's table,
    # the second layer nowhere in it.
    specs = subgrid_specs(bwalk, MULTI_SUB)
    rows_o, rows_t = ref_subchunk(opack, specs, MULTI_SUB)
    if len(rows_t) != 0:
        raise AssertionError(
            f"{why}: the translucent stream holds {len(rows_t)} "
            "rows — the second layer reached the builder, which "
            "never reads an extra block")
    if len(rows_o) == 0:
        raise AssertionError(
            f"{why}: the opaque stream is empty — the fixture's "
            "own rows vanished")

    # mcme, whole sub-chunk and at the cell itself: the same
    # answer over the main block alone — a read that surfaced the
    # water anywhere would change the totals or the cull.
    checks = check_subchunk(
        mcme, packdir, mgr, opack, page, world, bwalk, checks,
        f"{why} subchunk", MULTI_SUB)
    checks = check_world_probe(
        mcme, packdir, mgr, opack, page, world, bwalk,
        wx, wy, wz, checks, f"{why} cell")
    return checks


def check_usage(mcme: str, world: str, bworld: str, pack: str,
                tmp: str, checks: int) -> int:
    """The verb's own refusals: the target's chunk or subchunk
    absent is exit 2 on both formats (what `chunk get` answers), a
    NEIGHBOUR's is the world's edge — air, checked in the table
    probes above; the pack, the world and the coordinates keep the
    block and world verbs' own answers."""
    for argv, want in (
        (("chunk", "mesh"), 1),
        (("chunk", "mesh", world, pack, "8", "64"), 1),
        (("chunk", "mesh", world, pack, "x", "64", "8"), 1),
        (("chunk", "mesh", os.path.join(tmp, "nope"), pack,
          "8", "64", "8"), 2),
        (("chunk", "mesh", world, os.path.join(tmp, "no-such-pack"),
          "8", "64", "8"), 2),
        (("chunk", "mesh", world, pack, "200", "64", "200"), 2),
        (("chunk", "mesh", world, pack, "8", "64", "8",
          "--dimension", "nether"), 2),
        (("chunk", "mesh", world, pack, "8", "64", "8",
          "--dimension", "7"), 1),
        # a subchunk the store has never held: not stored, exit 2
        (("chunk", "mesh", bworld, pack, "8", "200", "8"), 2),
        # `chunk submesh` keeps every one of those answers: the
        # whole sub-chunk refused exactly where the one block
        # inside it would be.
        (("chunk", "submesh"), 1),
        (("chunk", "submesh", world, pack, "8", "64"), 1),
        (("chunk", "submesh", world, pack, "x", "64", "8"), 1),
        (("chunk", "submesh", os.path.join(tmp, "nope"), pack,
          "8", "64", "8"), 2),
        (("chunk", "submesh", world, os.path.join(tmp, "no-such-pack"),
          "8", "64", "8"), 2),
        (("chunk", "submesh", world, pack, "200", "64", "200"), 2),
        (("chunk", "submesh", world, pack, "8", "64", "8",
          "--dimension", "nether"), 2),
        (("chunk", "submesh", world, pack, "8", "64", "8",
          "--dimension", "7"), 1),
        (("chunk", "submesh", bworld, pack, "8", "200", "8"), 2),
    ):
        rc, out, err = A.run(mcme, *argv)
        if rc != want or out != "":
            raise AssertionError(
                f"{' '.join(argv)}: exit {rc} stdout {out!r}, want "
                f"{want} and nothing printed ({err.strip()})")
    return checks + 1


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--mcme", required=True)
    args = ap.parse_args()
    mcme = args.mcme

    A.load_reference()

    checks = 0
    tmp = tempfile.mkdtemp(prefix="mcme_ref_chunkmesh_")
    try:
        pack = A.craft_pack(tmp)
        mgr = A.make_manager(pack)
        opack, img = A.build_opack(mgr)
        if img.size[0] != img.size[1]:
            raise AssertionError(f"crafted: reference page {img.size}")

        # The fixture's ground: the classifications every probe's
        # expectations ride on, the reference's own answers.
        for block, want_t in FIXTURE_FLAGS.items():
            t = A.ref_get_model(mgr, block, {})["transparent"]
            if t != want_t:
                raise AssertionError(
                    f"fixture: {block} answers transparency {t}, "
                    f"want {want_t}")
        _log("ok — fixture: the crafted pack's flags — ring_opaque "
             "and atlas_cull opaque, ring_translucent translucent, "
             "atlas_cube opaque with no cullface, atlas_three and "
             "air partial")

        world = os.path.join(tmp, "java")
        shutil.copytree(FIXTURE_WORLD, world)
        wrote = write_pattern(mcme, world)
        # The sub-chunk's pattern lands before the walk is built:
        # the walk caches each chunk's tag it reads, and a fill it
        # has already cached would be invisible to it.
        sub_wrote = write_subchunk(mcme, world)
        _log(f"ok — java fixture: {wrote} blocks written into the "
             "golden world's scratch copy by chunk set, and "
             f"{sub_wrote} cells filled into sub-chunk (0, 8, 0) "
             "by chunk fill")

        walk = WorldWalk(os.path.join(world, "region"))
        checks = check_edge(walk, "java", checks)
        for (x, y, z), _block, _sides in PROBES:
            checks = check_world_probe(
                mcme, pack, mgr, opack, img.size[0], world, walk,
                x, y, z, checks, f"java probe ({x}, {y}, {z})")
        _log("ok — java probes: 15 positions answered alike — six "
             "different neighbours on six sides at once (the band's "
             "period), the chained pair on one side (the band's "
             "peak), a neighbour across the chunk boundary, one "
             "across the section edge, the none bucket ringed by "
             "opaque all round, the world's edge open on the sixth "
             "side, the states' own four, the spellings' two: "
             "the unsorted write spelled in the canonical order, "
             "the typed states spelled as the store holds them, "
             "and the typed tint state answering the missing "
             "block (D46), and the variant-key pair: the typed "
             "state taking the default key, the string taking "
             "the one it spells (D47)")
        checks = check_subchunk(
            mcme, pack, mgr, opack, img.size[0], world, walk,
            checks, "java subchunk")
        _log("ok — java subchunk: the whole sub-chunk answered "
             "alike — every surviving row of every cell in the "
             "builder's own loop order, the field's culls and the "
             "chained core's and the cavern's open faces among "
             "them, the wall's 36,864 rows carrying the opaque "
             "stream past the container split")
        checks = check_subchunk(
            mcme, pack, mgr, opack, img.size[0], world, walk,
            checks, "java state subchunk", STATE_SUB)
        _log("ok — java state subchunk: the states ridden through "
             "the batched read — the variants' models, the "
             "default's, and the stateful neighbour's clause "
             "answered alike over the whole sub-chunk")

        bworld = os.path.join(tmp, "bedrock")
        shutil.copytree(FIXTURE_STORE, bworld)
        wrote = write_pattern(mcme, bworld)
        sub_wrote = write_subchunk(mcme, bworld)
        write_two_layers(bworld)
        _log(f"ok — bedrock fixture: {wrote} blocks written into "
             "the golden store's scratch copy by chunk set — the "
             "subchunks it needs created by the write — "
             f"{sub_wrote} cells filled into sub-chunk (0, 8, 0) "
             "by chunk fill, and one waterlogged cell written "
             f"into the store's own bytes as a two-layer "
             "sub-chunk at {MULTI_SUB}")

        bwalk = BedrockWalk(os.path.join(bworld, "db"))
        checks = check_edge(bwalk, "bedrock", checks)
        for (x, y, z), _block, _sides in PROBES:
            checks = check_world_probe(
                mcme, pack, mgr, opack, img.size[0], bworld, bwalk,
                x, y, z, checks, f"bedrock probe ({x}, {y}, {z})")
        _log("ok — bedrock probes: the same 15 positions answered "
             "alike over the other format's own data — the "
             "subchunk payloads read by amulet-core's loader, the "
             "section-edge probe crossing two of them, the chunk "
             "edge crossing two chunks, the store's own edge open "
             "on the sixth side, the states' four spelling back "
             "verbatim through the store's own palette, the "
             "spellings' two over its typed tags, the typed "
             "tint state's missing block (D46), and the "
             "variant-key pair over the payload's own tags (D47)")
        checks = check_subchunk(
            mcme, pack, mgr, opack, img.size[0], bworld, bwalk,
            checks, "bedrock subchunk")
        _log("ok — bedrock subchunk: the same sub-chunk answered "
             "alike over the other format's own data — the same "
             "six boxes filled through its own write path, its "
             "subchunk payloads standing where the walk reads, "
             "the totals whole over the split")
        checks = check_subchunk(
            mcme, pack, mgr, opack, img.size[0], bworld, bwalk,
            checks, "bedrock state subchunk", STATE_SUB)
        _log("ok — bedrock state subchunk: the states ridden "
             "through the other format's layered read — the "
             "variants' models and the default's answered alike "
             "over the whole sub-chunk")
        checks = check_multi_layer(
            mcme, pack, mgr, opack, img.size[0], bworld, bwalk,
            checks, "bedrock multi-layer")
        _log("ok — bedrock multi-layer: the store's second layer "
             "spelled verbatim and read by no mesh — the builder "
             "and mcme both answering over the main block, the "
             "water's stream empty, the cull against the "
             "waterlogged neighbour firing on the main block's "
             "own transparency")

        checks = check_usage(mcme, world, bworld, pack, tmp, checks)
        _log("ok — usage refusals: 1 / 1 / 1 / 2 / 2 / 2 / 2 / 1 / 2 "
             "for `chunk mesh`, the same nine for `chunk submesh`")

        _log(f"ok — {checks} checks")
        return 0
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())
