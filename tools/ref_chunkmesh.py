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

# What the fixture rests on: the crafted pack's own classifications,
# the reference's answers (D32's rule through its manager).
FIXTURE_FLAGS = {
    "ring_opaque": 0, "ring_translucent": 1, "atlas_cull": 0,
    "atlas_cube": 0, "atlas_three": 2, "air": 2,
}


def _log(msg: str) -> None:
    print(f"ref_chunkmesh: {msg}", file=sys.stderr)


def parse_spec(spec: str) -> tuple[str, dict[str, str]]:
    """The walk's own spelling — `minecraft:oak_log{pillar_axis:
    "x"}` — back into the (name, props) pair the reference's
    manager wants."""
    name, _, rest = spec.partition("{")
    name = name.split(":", 1)[-1]
    props = {}
    if rest:
        for pair in rest.rstrip("}").split(","):
            k, _, v = pair.partition(":")
            props[k.strip()] = v.strip().strip('"')
    return name, props


class WorldWalk:
    """The world's blocks through ref_java_blocks' own walk: the
    chunk each position falls in, read once, absent chunks air."""

    def __init__(self, region_root: str):
        self.region_root = region_root
        self.tags: dict[tuple[int, int], object] = {}

    def block_at(self, x: int, y: int, z: int) -> str:
        cx, cz = x >> 4, z >> 4
        tag = self.tags.get((cx, cz))
        if tag is None:
            rx, rz = cx >> 5, cz >> 5
            tag = J.walk_chunk(
                os.path.join(self.region_root, f"r.{rx}.{rz}.mca"),
                cx, cz)
            self.tags[(cx, cz)] = tag if tag is not None else False
        if not tag:
            return J.AIR
        return J.block_at(tag, x, y, z)


class BedrockWalk:
    """The world's blocks through amulet-core's own leveldb loader:
    the subchunk payload under its key, the palette through the
    reference's reader (ref_bedrock_write's own glue), absent
    payloads air. First layer only — the fixtures' payloads are
    single-layer; waterlogging is D15's ground."""

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
        layers = self._layer(x >> 4, z >> 4, y >> 4)
        if not layers:
            return J.AIR
        blocks, palette = layers[0]
        entry = palette[B.block_at(blocks, x & 15, y & 15, z & 15)]
        # The store's own spelling, verbatim — the palette carries
        # the namespace and the reader prints it, exactly what
        # `chunk get` shows back.
        name = entry["name"].py_data
        states = entry.get("states")
        props = {} if states is None else {
            k: v.py_data for k, v in states.items()}
        if not props:
            return name
        items = ", ".join(
            f'{k}: "{v}"'
            for k, v in sorted(props.items(), key=lambda kv: kv[0].lower()))
        return f"{name}{{{items}}}"


def around(walk, x: int, y: int, z: int) -> dict[tuple, str]:
    """The 3x3x3 specs around a position, keyed by their offset
    from its centre — the grid the builder is handed."""
    cells = {}
    for dx in (-1, 0, 1):
        for dy in (-1, 0, 1):
            for dz in (-1, 0, 1):
                cells[(dx, dy, dz)] = walk.block_at(
                    x + dx, y + dy, z + dz)
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
    for (dx, dy, dz), spec in cells.items():
        name, props = parse_spec(spec)
        key = A.mcme_spec(name, props)
        if key not in index:
            index[key] = len(palette)
            palette.append(A.make_block(name, props))
        grid[dx + 1, dy + 1, dz + 1] = index[key]
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
    name, props = parse_spec(probe_spec)
    want = A.ref_get_model(mgr, name, props)
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
        n_name, n_props = parse_spec(walk.block_at(
            x + dx, y + dy, z + dz))
        n_t[dname] = A.ref_get_model(mgr, n_name, n_props)["transparent"]

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
        _log(f"ok — java fixture: {wrote} blocks written into the "
             "golden world's scratch copy, by chunk set")

        walk = WorldWalk(os.path.join(world, "region"))
        checks = check_edge(walk, "java", checks)
        for (x, y, z), _block, _sides in PROBES:
            checks = check_world_probe(
                mcme, pack, mgr, opack, img.size[0], world, walk,
                x, y, z, checks, f"java probe ({x}, {y}, {z})")
        _log("ok — java probes: 6 positions answered alike — six "
             "different neighbours on six sides at once (the band's "
             "period), the chained pair on one side (the band's "
             "peak), a neighbour across the chunk boundary, one "
             "across the section edge, the none bucket ringed by "
             "opaque all round, and the world's edge open on the "
             "sixth side")

        bworld = os.path.join(tmp, "bedrock")
        shutil.copytree(FIXTURE_STORE, bworld)
        wrote = write_pattern(mcme, bworld)
        _log(f"ok — bedrock fixture: {wrote} blocks written into "
             "the golden store's scratch copy, by chunk set — the "
             "subchunks it needs created by the write")

        bwalk = BedrockWalk(os.path.join(bworld, "db"))
        checks = check_edge(bwalk, "bedrock", checks)
        for (x, y, z), _block, _sides in PROBES:
            checks = check_world_probe(
                mcme, pack, mgr, opack, img.size[0], bworld, bwalk,
                x, y, z, checks, f"bedrock probe ({x}, {y}, {z})")
        _log("ok — bedrock probes: the same 6 positions answered "
             "alike over the other format's own data — the "
             "subchunk payloads read by amulet-core's loader, the "
             "section-edge probe crossing two of them, the chunk "
             "edge crossing two chunks, and the store's own edge "
             "open on the sixth side")

        checks = check_usage(mcme, world, bworld, pack, tmp, checks)
        _log("ok — usage refusals: 1 / 1 / 1 / 2 / 2 / 2 / 2 / 1 / 2")

        _log(f"ok — {checks} checks")
        return 0
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())
