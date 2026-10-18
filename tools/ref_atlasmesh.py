#!/usr/bin/env python3
"""
Cross-check: `mcme atlasmesh` — one block state, its texture
coordinates and its colour in atlas space, and the rows that survive
the builder's cull when a neighbour stands beside it, answered by
mcme and by the editor's own chunk builder plus its resource pack's
bounds rule deciding the same questions (decisions.md#D35, D36, D37).

How the reference is observed. The table is the editor's chunk
builder (chunk_builder_cy.pyx) run headless: one block placed in an
18-cube grid — air filling the ring when nothing stands beside the
probe, the neighbour block filling the whole grid, padding included,
when something does — meshed at loop zero so its
rows are the stream; each 12-float row carries position, texture
coordinate, the texture rectangle — written per THREE rows, one
triangle, stepping the block mesh's per-triangle texture index — and
the vertex tint, which the builder folds twice before the table is
done: the direction's brightness, then the band shade over the
vertex's world height (D36). The rectangles come from the
editor's OpenGL resource pack's own texture_bounds rule (exact key,
else the missing image's rectangle) over the atlas the editor's own
textureatlas module builds from the input mcme is handed: the
manager's missing_no sentinel seeded first, then the pack's textures
sorted — D34's shape, D35's sentinel. Both modules' package chain
imports wx (a display we do not have), so the pin loads the three
reference files themselves by path and runs the reference's own code.
One call is replaced: get_block_model becomes the manager's own —
that is the very call the pack makes after its universal-to-java
translation, and these blocks are already in the pack's own format
(D33's record), so translation is the single step bypassed, here
recorded. The pack's model manager caches by id(), so the pin hands
each probe a fresh palette and clears the cache: id reuse between
probes would otherwise answer a probe with the previous block's
geometry.

The world height the shade reads is the pin's to set: the builder
adds dy = chunk_offset[1] plus the sub-chunk's own y — the pin's
sub-chunk y is 0 — to the vertex before shading, so the pin drives
the builder at offset [0, y, 0] and mcme with `--y y`. The probe at
grid (1, 1, 1) meshes at loop zero inside the grid's one-cell
padding, so nothing else moves with y; the rows' positions ride the
offset, and the position comparison subtracts it back.

The mapping the coordinates live in is the fragment stage's own
arithmetic (render_chunk_330.frag), transcribed: mix(b0, b1,
mod(t, 1.0)) per axis — a floor-based mod, so a coordinate below
zero wraps up rather than clamping. It is transcribed because no
Python observes it: the builder's table carries the raw, unclamped
coordinate and the rectangle; the mapping happens per fragment at
render time. The transcription is checked against mcme's auvs row
for row — the prototype run that derived this pin reproduced mcme's
answers with it, including the negative wrap.

The cull clauses (chunk_builder_cy.pyx 279-289) are this pin's
domain since D37: a face whose direction has a cullface drops when
the neighbour's transparency is opaque, or when neighbour and self
are both translucent — the chained `== 1 == 1`, transparency alone,
nothing else of the neighbour read. A face with no cullface never
culls: the test runs under `if cull_id:`, so a full cube without
cullface keeps every face even embedded in stone. To drive it, the
neighbour fills the WHOLE grid, the padding included so every cull
read lands on it, except the probe's own cell. The field culls its
own faces against itself under both clauses; the one face that can
keep is the one turned toward the probe, and only when the probe's
transparency is neither 0 nor a matching 1. Three of the probe's
six neighbours are the grid's padding — read for the cull, never
meshed — so that leak is exactly 18 rows, in the field's stream,
behind the probe's own in loop order. When that stream is also the
probe's own — an opaque field beside a partial probe — the leak
rides the probe's tail: the probe is the grid's first cell, so its
rows are the stream's head, and the totals pin the split.

What is compared:

  CRAFTED PACK — eight probes over four textures of three sizes, so
    every rectangle is distinct: the ring block itself, an empty
    mesh answering no rows; a full cube without cullface (the none
    bucket, opaque stream); the same cube with a cullface per face
    (six buckets, the cull test walked beside air); a full cube on
    an alpha texture (the translucent stream, the other array); one
    bucket holding three faces on three textures (the 3-row triangle
    step, three rectangles inside one face stream); UVs past both
    edges (the mod wraps them up); a texture the pack lacks (the
    sentinel's rectangle through the missing machinery); and a
    multipart merge (two cubes' rows concatenated in one bucket).
    Behind them stand two field blocks — full cubes with a cullface
    per face, each on its own 2x2 texture, opaque and translucent —
    the neighbours the cull probes stand beside.

  WORLD HEIGHTS — the cull cube re-run over eight block heights for
    the wave the shade reads out of: 16 (mid-ramp), 32 (the peak,
    where t == 1 and the reference's `shade > 1` fold has not fired
    yet), 48 (past the top, folded back), 63 (just under the far
    side of the wave), 64 and 96 (the period, the next peak), and
    two below zero — -1, where numpy's remainder walks the wave
    C's fmod would answer negative and slide under the trough, and
    -32, at the peak on the same parting. Every row's tint must
    agree, and so must the positions, the offset subtracted back.

  CULL CLAUSES — nine probes with a neighbour standing: the cull
    cube and the glass against the opaque and the translucent field
    (the first clause fires, neither fires, the chained pair fires),
    the cullface-less cube standing under both (the exemption), the
    missing BLOCK standing opaque on both sides (a neighbour the
    pack does not have — the loader's own answer, which mcme's
    prelude shares), and the pack's own air answering the default.
    A culled bucket answers the mesh document's dir and no table
    keys: there are no rows for bounds, auvs or tints to name.

  VANILLA FIX — the reader's own shipped pack, completed with an
    opaque RGBA image behind every texture its models can name and
    given the air ring it does not ship, one probe per blockstate
    file at its first variant key (the full key sweep is D33's
    ground; D35's table rides on the meshes it pinned). Blocks the
    reference tints are skipped by rule and counted — D32 answered
    the default tint, and under the fold mcme's default would part
    company with the reference's colour. The block tints stay out of
    this pin's domain alike.

  VANILLA BESIDE — the same sweep with stone standing beside every
    probe: an opaque neighbour culls every cullface bucket away over
    real data while the none buckets stand, and a partial probe's
    own rows ride ahead of the leak the field's three meshed cells
    leave behind. One field only: the completed pack answers no
    block translucent — the shipped alpha PNGs (water, lava, the
    banners) are palette images, which the reference's texture rule
    reads opaque — so the chained pair lives in the crafted probes,
    exact, and real data walks clause one and the exemption.

  BROKEN IMAGE — a pack whose only texture is not a PNG: the
    reference's manager never finishes loading (Pillow refuses the
    file in its transparency-flag pass), so there is no table to
    compare; mcme builds the mesh — its index keeps the file opaque
    with a warning (D32's record) — and refuses by name at the
    atlas, exit 6. Each side is pinned to its own refusal.

  USAGE — missing arguments exit 1, a pack directory that does not
    exist exit 2, an empty namespace or name exit 1, a non-string
    state exit 1, `--y` or `--beside` without a value exit 1, a
    non-integer `--y` exit 1, a `--beside` spec the prelude refuses
    exit 1, a flag given twice exit 1 — the block prelude's own
    answers (D33) and the flags' own refusals.

The comparison tolerance is float32: the builder's table is float32,
so its values are compared with a 1e-6 slack. The rectangles and
coordinates are dyadic and would compare exact — pages are powers of
two, coordinates divide by 16 — but the fold is not dyadic at all:
the brightness steps of 0.15 and the shade's 0.9 + 0.2 round
through the table's storage where mcme keeps doubles, and the
storage precision is the honest bound. Element rotation is not
dyadic either.

Usage:
    venv/bin/python tools/ref_atlasmesh.py --mcme ./mcme
"""

from __future__ import annotations

import argparse
import glob
import importlib.util
import itertools
import json
import math
import os
import shutil
import subprocess
import sys
import tempfile
import types

# The builder's cull-id order: the stream walks it, bucket by
# bucket. mcme's names match word for word ("none" is the key the
# reader answers for a face with no cullface).
CULL_ORDER = ("none", "up", "down", "north", "east", "south", "west")

# The table is float32 on the reference side; in-domain values are
# dyadic and compare exact, but the storage sets the bound.
F32_TOL = 1e-6

_REF = types.SimpleNamespace()  # the loaded reference modules


def _log(msg: str) -> None:
    print(f"ref_atlasmesh: {msg}", file=sys.stderr)


def run(mcme: str, *args: str) -> tuple[int, str, str]:
    """Run mcme, return (exit code, stdout, stderr)."""
    proc = subprocess.run(
        [mcme, *args], capture_output=True, text=True, timeout=120)
    return proc.returncode, proc.stdout, proc.stderr


# ---------------------------------------------------------------------------
# The reference side
# ---------------------------------------------------------------------------

def pkg_file(rel: str) -> str:
    spec = importlib.util.find_spec("amulet_map_editor")
    if spec is None or not spec.submodule_search_locations:
        raise AssertionError("amulet_map_editor is not installed")
    path = os.path.join(spec.submodule_search_locations[0], rel)
    if not os.path.isfile(path):
        raise AssertionError(f"no reference file at {path}")
    return path


def load_by_path(name: str, path: str):
    """Load a reference file as its own module. The name's last
    component must match the file's: a compiled extension looks its
    PyInit_ symbol up by it."""
    spec = importlib.util.spec_from_file_location(name, path)
    mod = importlib.util.module_from_spec(spec)
    sys.modules[name] = mod
    spec.loader.exec_module(mod)
    return mod


def load_reference() -> None:
    """The editor's own code, by file path: the package chain
    imports wx (needs a display we do not have). textureatlas first
    — the resource pack module imports it out of its package, which
    the pin replaces with a shim carrying the by-path module."""
    _REF.ta = load_by_path(
        "mcme_ref_textureatlas", pkg_file("api/opengl/textureatlas.py"))
    shim = types.ModuleType("amulet_map_editor.api.opengl")
    shim.textureatlas = _REF.ta
    sys.modules["amulet_map_editor.api.opengl"] = shim
    _REF.rp = load_by_path(
        "mcme_ref_resource_pack",
        pkg_file("api/opengl/resource_pack/resource_pack.py"))
    spec = importlib.util.find_spec("amulet_map_editor")
    root = spec.submodule_search_locations[0]
    exts = sorted(glob.glob(os.path.join(
        root, "api", "opengl", "mesh", "level", "chunk",
        "chunk_builder_cy*.so")))
    if not exts:
        raise AssertionError("no chunk_builder_cy extension in the editor")
    _REF.cb = load_by_path("mcme_ref_chunk.chunk_builder_cy", exts[0])


def make_manager(packdir: str):
    """The reference manager over one pack, load and all."""
    os.environ.setdefault("CACHE_DIR", tempfile.mkdtemp(prefix="mcme_cache_"))
    from minecraft_model_reader.api.resource_pack.java import (
        JavaResourcePack, JavaResourcePackManager)

    return JavaResourcePackManager([JavaResourcePack(packdir)], load=True)


def make_block(name: str, props: dict[str, str]):
    from minecraft_model_reader.api import Block
    import amulet_nbt

    return Block("minecraft", name,
                 {k: amulet_nbt.TAG_String(v) for k, v in props.items()})


def ref_input(mgr) -> tuple[str, ...]:
    """The input both sides are handed: the manager's own table
    shape — its missing_no sentinel seeded FIRST (asserted, so the
    seed can not silently stop working), then the pack's textures
    sorted."""
    from minecraft_model_reader.api.image import missing_no_path

    if missing_no_path not in mgr.textures:
        raise AssertionError("the manager's table has no missing_no sentinel")
    paths = [p for p in mgr.textures if p != missing_no_path]
    return (missing_no_path, *sorted(paths))


def ref_get_model(mgr, name: str, props: dict[str, str]) -> dict:
    """The reference's block-level mesh document (D33's, reused
    field for field — this pin re-pins it on the new verb and then
    rides its buckets)."""
    from minecraft_model_reader.api.image import missing_no_path

    m = mgr._get_model(make_block(name, props))

    def norm(path):
        if path == missing_no_path or path == getattr(mgr, "missing_no", None):
            return "missing_no"
        return path

    dirs = {}
    for key, verts in m.verts.items():
        dname = "none" if key is None else key
        dirs[dname] = {
            "verts": [float(v) for v in verts],
            "tverts": [float(v) for v in m.texture_coords[key]],
            "tint_verts": [float(v) for v in m.tint_verts[key]],
            "faces": [int(v) for v in m.faces[key]],
            "texture_indexes": [int(v) for v in m.texture_index[key]],
        }
    return {
        "face_mode": int(m.face_mode),
        "textures": [norm(p) for p in m.textures],
        "transparent": int(m.is_transparent),
        "dirs": dirs,
    }


def ref_tint(name: str, props: dict[str, str]):
    """The reference's own block-tint function — the exclusion rule."""
    from minecraft_model_reader.api.resource_pack.java.resource_pack_manager import (  # noqa: E501
        _get_block_tint)

    try:
        return tuple(_get_block_tint(make_block(name, props)))
    except Exception:
        return None


def in_tint_domain(name: str, props: dict[str, str]) -> bool:
    """D32 answers every block with the default tint; a block the
    reference colours is out of the pin's domain."""
    t = ref_tint(name, props)
    return t is not None and all(
        math.isclose(a, b, rel_tol=0, abs_tol=1e-12)
        for a, b in zip(t, (1.0, 1.0, 1.0)))


def build_opack(mgr):
    """The editor's OpenGL resource pack, driven headless: its own
    texture_bounds rule verbatim over the atlas the pin just built —
    the dict setup() would have filled from its own cache — and its
    get_block_model replaced by the manager's own, the call the pack
    makes after translation. Returns the pack and the atlas image."""
    opack = _REF.rp.OpenGLResourcePack(mgr, None)  # translator never read
    img, bounds = _REF.ta.create_atlas(ref_input(mgr))
    opack._texture_bounds = dict(bounds)
    if mgr.missing_no not in opack._texture_bounds:
        raise AssertionError("the atlas input lost the sentinel key")
    opack.get_block_model = mgr.get_block_model
    return opack, img


def ref_table(opack, name: str, props: dict[str, str],
              y: int = 0, beside: tuple[str, dict] | None = None):
    """The builder's own table for one block at world height y (the
    chunk offset sets it; the sub-chunk's own y is 0 — dy is their
    sum). The probe sits at grid (1, 1, 1) — meshed at loop zero,
    its rows the stream's own. When something stands beside the
    probe the WHOLE grid is that block, the padding included so
    every cull read lands on it, except the probe's own cell: a
    uniform field, the cull clauses' own shape. The palette gets a
    fresh list (the pack caches by id()) and the cache is cleared:
    id reuse between probes would answer with stale geometry."""
    import numpy

    grid = numpy.zeros((18, 18, 18), numpy.uint32)
    if beside is None:
        grid[1, 1, 1] = 1  # air ring: not full, keeps every face
    else:
        grid[:, :, :] = 2
        grid[1, 1, 1] = 1
    palette = [make_block("air", {}), make_block(name, props)]
    if beside is not None:
        palette.append(make_block(beside[0], beside[1]))
    opack.block_model_manager = {}
    opaque, trans = _REF.cb.create_lod0_chunk(
        opack, numpy.array([0, y, 0], numpy.int64), [(grid, 0)],
        palette, 12)
    rows_o = numpy.frombuffer(opaque[0], numpy.float32).reshape(-1, 12)
    rows_t = numpy.frombuffer(trans[0], numpy.float32).reshape(-1, 12)
    return rows_o, rows_t


def shader_auv(tvert, rect) -> tuple[float, float]:
    """The fragment stage's mapping, transcribed from
    render_chunk_330.frag: mix(b0, b1, mod(t, 1.0)) per axis. GLSL's
    mod is floor-based, so the negative coordinate wraps up."""
    return (
        rect[0] + (rect[2] - rect[0]) * (tvert[0] - math.floor(tvert[0])),
        rect[1] + (rect[3] - rect[1]) * (tvert[1] - math.floor(tvert[1])),
    )


# ---------------------------------------------------------------------------
# The mcme side
# ---------------------------------------------------------------------------

def mcme_spec(name: str, props: dict[str, str]) -> str:
    spec = f"minecraft:{name}"
    if props:
        inner = ",".join(f'{k}:"{v}"' for k, v in props.items())
        spec += "{" + inner + "}"
    return spec


def mcme_atlasmesh(mcme: str, packdir: str, name: str,
                   props: dict[str, str], y: int = 0,
                   beside: str | None = None) -> dict:
    argv = [mcme, "atlasmesh", packdir, mcme_spec(name, props),
            "--json", "--y", str(y)]
    if beside is not None:
        argv += ["--beside", beside]
    rc, out, err = run(*argv)
    if rc != 0:
        raise AssertionError(
            f"atlasmesh {mcme_spec(name, props)} --y {y}"
            f"{'' if beside is None else ' --beside ' + beside}: "
            f"exit {rc}, stderr {err.strip()!r}")
    try:
        return json.loads(out)
    except json.JSONDecodeError:
        raise AssertionError(
            f"atlasmesh {mcme_spec(name, props)} --y {y}: "
            f"unparseable JSON {out!r}")


def mcme_submesh(mcme: str, packdir: str, world: str,
                 x: str, y: str, z: str) -> dict:
    """`chunk submesh`'s whole answer — the sub-chunk's cells, the
    streams' totals, the page."""
    argv = [mcme, "chunk", "submesh", world, packdir, x, y, z,
            "--json"]
    rc, out, err = run(*argv)
    if rc != 0:
        raise AssertionError(
            f"chunk submesh {world} {x} {y} {z}: exit {rc}, "
            f"stderr {err.strip()!r}")
    try:
        return json.loads(out)
    except json.JSONDecodeError:
        raise AssertionError(
            f"chunk submesh {world} {x} {y} {z}: unparseable JSON "
            f"{out[:200]!r}")


def mesh_diff(got, want, path: str = "mesh") -> str | None:
    """Where two mesh documents part company, or None for equal.
    Same rule as the mesh pin: exact for two ints, a 1e-9 tolerance
    when a float is involved."""
    if isinstance(want, dict) and isinstance(got, dict):
        if set(got) != set(want):
            return (f"{path}: keys {sorted(map(str, got))} "
                    f"!= {sorted(map(str, want))}")
        for key in want:
            d = mesh_diff(got[key], want[key], f"{path}.{key}")
            if d:
                return d
        return None
    if isinstance(want, list) and isinstance(got, list):
        if len(got) != len(want):
            return f"{path}: {len(got)} entries, want {len(want)}"
        for i, (g, w) in enumerate(zip(got, want)):
            d = mesh_diff(g, w, f"{path}[{i}]")
            if d:
                return d
        return None
    if isinstance(want, bool) or isinstance(got, bool):
        if got is not want:
            return f"{path}: {got!r} != {want!r}"
        return None
    if isinstance(want, (int, float)) and isinstance(got, (int, float)):
        if isinstance(want, int) and isinstance(got, int):
            if got != want:
                return f"{path}: {got!r} != {want!r}"
        elif not math.isclose(got, want, rel_tol=1e-9, abs_tol=1e-9):
            return f"{path}: {got!r} != {want!r}"
        return None
    if got != want:
        return f"{path}: {got!r} != {want!r}"
    return None


def f32_diff(got, want, what: str) -> str | None:
    """Two float32-derived sequences at the storage's precision."""
    for i, (a, b) in enumerate(zip(map(float, got), map(float, want))):
        if not math.isclose(a, b, rel_tol=F32_TOL, abs_tol=F32_TOL):
            return f"{what}[{i}]: {a!r} != {b!r}"
    return None


# ---------------------------------------------------------------------------
# One probe, both sides
# ---------------------------------------------------------------------------

def check_probe(mcme, packdir, mgr, opack, page: int, name: str,
                props: dict[str, str], checks: int, why: str,
                y: int = 0, beside: str | None = None,
                ring: tuple[str, dict] | None = None,
                beside_t: int = 2) -> int:
    """The reference answers its mesh document and its builder's
    table; mcme answers the same document plus the table's new
    columns, and every row must agree. At world height y the rows
    carry the offset in their positions and their tints; the
    document mcme prints is the block's own, so the position
    comparison subtracts the offset back. With beside, the rows are
    the cull clauses' survivors: `beside` names the neighbour for
    mcme's --beside, `ring` the block filling the builder's field,
    and beside_t the transparency both answer for it — the only
    thing the clauses read."""
    spec = mcme_spec(name, props)
    want = ref_get_model(mgr, name, props)
    got = mcme_atlasmesh(mcme, packdir, name, props, y, beside)

    # The mesh document: D33's ground, re-pinned on this verb so the
    # new columns are read off a mesh both sides agree on. Fresh
    # dicts: the strip must not eat the table columns below.
    got_mesh = {k: v for k, v in got.items() if k != "size"}
    got_mesh["dirs"] = {
        dname: {k: v for k, v in d.items()
                if k not in ("bounds", "auvs", "tints")}
        for dname, d in got.get("dirs", {}).items()}
    d = mesh_diff(got_mesh, want, spec)
    if d:
        raise AssertionError(f"{why}: {d}")

    # The page the coordinates are in.
    if got["size"] != page:
        raise AssertionError(
            f"{why}: {spec}: page {got['size']}, reference {page}")

    # The builder's table, split into its two streams — one holds
    # the probe's rows, the other the field's leak, if any.
    #
    # A uniform field culls its own faces against itself under both
    # clauses; the one face that can keep is the one turned toward
    # the probe, and only when the probe's transparency is neither 0
    # (the first clause) nor a matching 1 (the chained pair). The
    # probe has six neighbours; three of them are the grid's
    # padding — read for the cull, never meshed — so the leak is
    # exactly the three interior cells' probe-facing faces: 18 rows,
    # in the field's stream, behind the probe's own in loop order.
    # When that stream is also the probe's own — an opaque field
    # beside a partial probe — the leak rides the probe's tail: the
    # probe is the grid's first cell, so its rows are the stream's
    # head, and the totals below pin the split.
    probe_t = want["transparent"]
    rows_o, rows_t = ref_table(opack, name, props, y, ring)
    leaky = beside_t in (0, 1) and probe_t != 0 and not (
        beside_t == 1 and probe_t == 1)
    leak_in_other = leaky and (beside_t == 0) == (probe_t == 1)
    rows, other = ((rows_t, rows_o) if probe_t == 1
                   else (rows_o, rows_t))
    other_n = 18 if leak_in_other else 0
    if len(other) != other_n:
        raise AssertionError(
            f"{why}: {spec}: {len(other)} rows in the stream the "
            f"{'translucent' if probe_t == 1 else 'opaque'} side "
            f"does not use, want {other_n}")
    head_n = 18 if leaky and not leak_in_other else 0

    # The skip clauses, transcribed (chunk_builder_cy.pyx 282-289):
    # a face with no cullface never culls; a face drops when the
    # neighbour is opaque, or when neighbour and self are both
    # translucent — transparency alone, nothing else read.
    def culled_out(dname: str) -> bool:
        return dname != "none" and (
            beside_t == 0 or
            (beside_t == 1 and probe_t == 1))

    total = sum(len(v["faces"]) for dname, v in want["dirs"].items()
                if not culled_out(dname))
    if len(rows) != total + head_n:
        raise AssertionError(
            f"{why}: {spec}: the builder answered {len(rows)} rows, "
            f"the surviving mesh has {total} plus {head_n} leaked")

    # Bucket by bucket in the builder's emission order: the rows are
    # one flat stream, and the mesh's face counts are the slice.
    walked = 0
    for dname in CULL_ORDER:
        if dname not in want["dirs"]:
            continue
        mesh_n = len(want["dirs"][dname]["faces"])
        n = 0 if culled_out(dname) else mesh_n
        g = got["dirs"].get(dname)
        if g is None:
            raise AssertionError(f"{why}: {spec}: mcme has no {dname}")
        if n == 0:
            # The bucket drops whole: the table's keys do not
            # appear beside the mesh document — there are no rows
            # for them to name.
            for key in ("bounds", "auvs", "tints"):
                if key in g:
                    raise AssertionError(
                        f"{why}: {spec} {dname}: the cull dropped "
                        f"the bucket, mcme still answered {key}")
            continue  # the stream holds no rows for it either
        if len(g["bounds"]) != 4 * n:
            raise AssertionError(
                f"{why}: {spec} {dname}: {len(g['bounds'])} bound "
                f"values for {n} rows")
        if len(g["auvs"]) != 2 * n:
            raise AssertionError(
                f"{why}: {spec} {dname}: {len(g['auvs'])} auv values "
                f"for {n} rows")
        if len(g["tints"]) != 3 * n:
            raise AssertionError(
                f"{why}: {spec} {dname}: {len(g['tints'])} tint values "
                f"for {n} rows")
        verts, tverts, faces = g["verts"], g["tverts"], g["faces"]
        for r in range(n):
            row = rows[walked + r]
            fi = faces[r]
            pos, tv, rect = row[0:3], row[3:5], row[5:9]
            for seq, want_seq, what in (
                # The builder's rows ride the world offset; the
                # document mcme prints is the block's own.
                (pos, (verts[3 * fi], verts[3 * fi + 1] + y,
                       verts[3 * fi + 2]), "position"),
                (tv, tverts[2 * fi:2 * fi + 2], "tvert"),
                (rect, g["bounds"][4 * r:4 * r + 4], "bounds"),
            ):
                d = f32_diff(seq, want_seq,
                             f"{spec} {dname} row {r} {what}")
                if d:
                    raise AssertionError(f"{why}: {d}")
            # The mapping, from the builder's own row: raw
            # coordinate and rectangle, through the shader's
            # arithmetic, must be the auv mcme printed.
            d = f32_diff(g["auvs"][2 * r:2 * r + 2], shader_auv(tv, rect),
                         f"{spec} {dname} row {r} auv")
            if d:
                raise AssertionError(f"{why}: {d}")
            # The tint the builder folded — the mesh's tint times the
            # direction's brightness times the band shade at the
            # row's world height — must be the tints mcme printed.
            d = f32_diff(g["tints"][3 * r:3 * r + 3], row[9:12],
                         f"{spec} {dname} row {r} tint")
            if d:
                raise AssertionError(f"{why}: {d}")
        walked += n
    if walked + head_n != len(rows):
        raise AssertionError(
            f"{why}: {spec}: {len(rows) - walked - head_n} rows past "
            "the mesh and the leak")
    return checks + 1


def check_usage(mcme: str, pack: str, tmp: str, checks: int) -> int:
    """The block prelude's own refusals (D33), on the new verb."""
    rc, _out, _err = run(mcme, "atlasmesh")
    if rc != 1:
        raise AssertionError(f"`atlasmesh` alone: exit {rc}, want 1")
    checks += 1
    rc, _out, _err = run(mcme, "atlasmesh",
                         os.path.join(tmp, "no-such-pack"),
                         "minecraft:atlas_cube")
    if rc != 2:
        raise AssertionError(f"missing pack dir: exit {rc}, want 2")
    checks += 1
    rc, _out, _err = run(mcme, "atlasmesh", pack, ":atlas_cube")
    if rc != 1:
        raise AssertionError(f"empty namespace: exit {rc}, want 1")
    checks += 1
    rc, _out, _err = run(mcme, "atlasmesh", pack, "minecraft:")
    if rc != 1:
        raise AssertionError(f"empty name: exit {rc}, want 1")
    checks += 1
    rc, out, _err = run(mcme, "atlasmesh", pack,
                        'minecraft:atlas_cube{a:1}')
    if rc != 0:
        raise AssertionError(f"typed state: exit {rc}, want 0 — the "
                             "reference meshes on, the typed state "
                             "matching nothing (D43)")
    checks += 1
    rc2, out2, _err = run(mcme, "atlasmesh", pack,
                          "minecraft:atlas_cube")
    if rc2 != 0 or out.split("\n", 1)[1] != out2.split("\n", 1)[1]:
        raise AssertionError("typed state changed the mesh past the "
                             "echo")
    checks += 1
    rc, _out, _err = run(mcme, "atlasmesh", pack,
                         "minecraft:atlas_cube", "--y")
    if rc != 1:
        raise AssertionError(f"`--y` without a value: exit {rc}, want 1")
    checks += 1
    rc, _out, _err = run(mcme, "atlasmesh", pack,
                         "minecraft:atlas_cube", "--y", "nope")
    if rc != 1:
        raise AssertionError(f"`--y nope`: exit {rc}, want 1")
    checks += 1
    rc, _out, _err = run(mcme, "atlasmesh", pack,
                         "minecraft:atlas_cube", "--beside")
    if rc != 1:
        raise AssertionError(
            f"`--beside` without a value: exit {rc}, want 1")
    checks += 1
    rc, _out, _err = run(mcme, "atlasmesh", pack,
                         "minecraft:atlas_cube", "--beside", ":stone")
    if rc != 1:
        raise AssertionError(
            f"`--beside :stone`: exit {rc}, want 1")
    return checks + 1


def check_broken(mcme: str, packdir: str, checks: int) -> int:
    """The pack whose only texture is not a PNG: the reference's
    manager never finishes loading — Pillow refuses the file in its
    transparency-flag pass — so there is no table to compare. mcme's
    index keeps the file opaque with a warning (D32), the mesh
    builds, and the atlas refuses by name: exit 6. Each side is
    pinned to its own refusal."""
    os.environ.setdefault("CACHE_DIR", tempfile.mkdtemp(prefix="mcme_cache_"))
    from minecraft_model_reader.api.resource_pack.java import (
        JavaResourcePack, JavaResourcePackManager)

    try:
        JavaResourcePackManager([JavaResourcePack(packdir)], load=True)
    except Exception:
        pass  # the reference's own refusal, at load
    else:
        raise AssertionError("broken pack: the reference loaded it")
    rc, _out, err = run(mcme, "atlasmesh", packdir,
                        "minecraft:atlas_bad", "--json")
    if rc != 6:
        raise AssertionError(
            f"broken pack: mcme exit {rc}, want 6 ({err.strip()!r})")
    return checks + 1


# ---------------------------------------------------------------------------
# The crafted pack
# ---------------------------------------------------------------------------

RED = (255, 0, 0, 255)
MAGENTA = (255, 0, 255, 255)
YELLOW = (255, 255, 0, 255)
CYAN_ALPHA = (0, 255, 255, 128)

SIX = ("down", "up", "north", "east", "south", "west")


def cube(tex: str, cullface: bool) -> dict:
    """A full cube, optionally with a cullface per face."""
    faces = {d: {"texture": "#all"} for d in SIX}
    if cullface:
        faces = {d: {"texture": "#all", "cullface": d} for d in SIX}
    return {"textures": {"all": tex},
            "elements": [{"from": [0, 0, 0], "to": [16, 16, 16],
                          "faces": faces}]}


def up_cube(tex: str, uv=None) -> dict:
    """A partial cube with a single up face."""
    face = {"texture": "#a"}
    if uv is not None:
        face["uv"] = uv
    return {"textures": {"a": tex},
            "elements": [{"from": [2, 2, 2], "to": [14, 14, 14],
                          "faces": {"up": face}}]}


def write_png(path: str, mode: str, color, size) -> None:
    from PIL import Image

    os.makedirs(os.path.dirname(path), exist_ok=True)
    img = Image.new(mode, size)
    img.putdata([color] * (size[0] * size[1]))
    img.save(path, "PNG")


def write_json(path: str, doc) -> None:
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w") as f:
        json.dump(doc, f)


def craft_pack(root: str) -> str:
    """The crafted pack: four textures of three sizes (every
    rectangle distinct), the probes, and air — the ring block needs a
    model whose mesh has no faces and that is not a full block, so
    the builder keeps every probe face and mcme's table stands beside
    the reference's. The air model is an empty elements list with no
    textures: with textures and no elements the reference answers
    the missing BLOCK, a full cube that would cull the ring."""
    pack = os.path.join(root, "crafted")
    mdir = os.path.join(pack, "assets", "mc", "models", "block")
    bsdir = os.path.join(pack, "assets", "minecraft", "blockstates")
    texdir = os.path.join(pack, "assets", "minecraft", "textures", "block")
    os.makedirs(mdir)
    os.makedirs(bsdir)
    write_json(os.path.join(pack, "pack.mcmeta"),
               {"pack": {"pack_format": 9, "description": "atlasmesh probes"}})

    write_png(os.path.join(texdir, "red.png"), "RGBA", RED, (4, 4))
    write_png(os.path.join(texdir, "magenta.png"), "RGBA", MAGENTA, (2, 4))
    write_png(os.path.join(texdir, "wide.png"), "RGBA", YELLOW, (8, 2))
    write_png(os.path.join(texdir, "alpha.png"), "RGBA", CYAN_ALPHA, (4, 4))
    # The field blocks' own textures: 2x2, a size no probe uses, so
    # the leak filter recognizes their rows by the rectangle.
    write_png(os.path.join(texdir, "ring.png"), "RGBA", RED, (2, 2))
    write_png(os.path.join(texdir, "ringa.png"), "RGBA", CYAN_ALPHA,
              (2, 2))

    models = {
        # The ring block: no elements, no textures — the empty mesh,
        # transparency partial, so the cull test keeps probe faces.
        "empty": {"elements": []},
        # A full cube without cullface: the none bucket, the opaque
        # stream.
        "cube_red": cube("block/red", False),
        # The same with a cullface per face: six buckets, and the
        # cull test walked against the air ring.
        "cube_cull": cube("block/red", True),
        # A full cube on an alpha texture: the translucent stream,
        # the builder's other array.
        "cube_alpha": cube("block/alpha", False),
        # The merge partner, never probed alone.
        "cube_mag": cube("block/magenta", False),
        # Three faces in one bucket on three textures: the 3-row
        # triangle step, three rectangles inside one face stream.
        "three_up": {
            "textures": {"a": "block/red", "b": "block/magenta",
                         "c": "block/wide"},
            "elements": [
                {"from": [0, 0, 0], "to": [16, 4, 8],
                 "faces": {"up": {"texture": "#a", "uv": [0, 0, 16, 16]}}},
                {"from": [0, 0, 8], "to": [16, 4, 16],
                 "faces": {"up": {"texture": "#b", "uv": [0, 0, 16, 16]}}},
                {"from": [2, 6, 2], "to": [14, 10, 14],
                 "faces": {"up": {"texture": "#c", "uv": [0, 0, 16, 16]}}}]},
        # UVs past both edges: the coordinates wrap, they do not
        # clamp (D35's unclamped record, through the table).
        "wrap_uv": up_cube("block/red", uv=[-4, 0, 20, 16]),
        # A texture the pack lacks: the sentinel's rectangle.
        "missing_part": up_cube("block/absent"),
        # The field blocks the cull probes stand beside: full
        # cubes with a cullface per face, opaque and translucent,
        # each on its own texture (D37).
        "ring_cube": cube("block/ring", True),
        "ring_cubea": cube("block/ringa", True),
    }
    for name, doc in models.items():
        write_json(os.path.join(mdir, name + ".json"), doc)

    def variants(m: str) -> dict:
        return {"variants": {"": {"model": f"mc:block/{m}"}}}

    blockstates = {
        "air": variants("empty"),
        "atlas_cube": variants("cube_red"),
        "atlas_cull": variants("cube_cull"),
        "atlas_glass": variants("cube_alpha"),
        "atlas_three": variants("three_up"),
        "atlas_wrap": variants("wrap_uv"),
        "atlas_missing": variants("missing_part"),
        # Multipart: two cubes' rows concatenate inside the buckets.
        "atlas_merge": {"multipart": [
            {"apply": {"model": "mc:block/cube_red"}},
            {"apply": {"model": "mc:block/cube_mag"}}]},
        "ring_opaque": variants("ring_cube"),
        "ring_translucent": variants("ring_cubea"),
        # States select the model (D41): three keys, first-match in
        # file order — axis=x, then axis=y, the un-stated default
        # last — each pointing at a model the pack already holds, so
        # the texture table and its page stay exactly D34's.
        "atlas_turn": {"variants": {
            "axis=x": {"model": "mc:block/three_up"},
            "axis=y": {"model": "mc:block/cube_red"},
            "": {"model": "mc:block/ring_cube"}}},
    }
    for name, doc in blockstates.items():
        write_json(os.path.join(bsdir, name + ".json"), doc)
    return pack


CRAFTED_PROBES = [
    ("air", {}),            # the ring block itself: an empty mesh
    ("atlas_cube", {}),     # the none bucket, opaque stream
    ("atlas_cull", {}),     # six buckets, the cull test beside air
    ("atlas_glass", {}),    # the translucent stream
    ("atlas_three", {}),    # three rectangles inside one bucket
    ("atlas_wrap", {}),     # uv past both edges: wrap, not clamp
    ("atlas_missing", {}),  # a texture the pack lacks: the sentinel
    ("atlas_merge", {}),    # multipart: two cubes, one bucket
]


# The cull probes: the block, the neighbour mcme is told (--beside),
# the block filling the builder's field, and the transparency both
# sides answer for that neighbour — the only thing the clauses read.
# The missing neighbour is the missing BLOCK: the loader answers it
# opaque and so does mcme's prelude, so the field stands in for it.
CULL_PROBES = [
    ("atlas_cull", "minecraft:ring_opaque", "ring_opaque", 0),
    ("atlas_cull", "minecraft:ring_translucent", "ring_translucent", 1),
    ("atlas_glass", "minecraft:ring_opaque", "ring_opaque", 0),
    ("atlas_glass", "minecraft:ring_translucent", "ring_translucent", 1),
    ("atlas_cube", "minecraft:ring_opaque", "ring_opaque", 0),
    ("atlas_cube", "minecraft:ring_translucent", "ring_translucent", 1),
    ("atlas_cull", "minecraft:doesnotexist", "ring_opaque", 0),
    ("atlas_cube", "minecraft:doesnotexist", "ring_opaque", 0),
    ("atlas_cull", "minecraft:air", "air", 2),
]


def craft_broken(root: str) -> str:
    """A pack whose only texture is not a PNG."""
    pack = os.path.join(root, "broken")
    mdir = os.path.join(pack, "assets", "mc", "models", "block")
    bsdir = os.path.join(pack, "assets", "minecraft", "blockstates")
    texdir = os.path.join(pack, "assets", "minecraft", "textures", "block")
    os.makedirs(mdir)
    os.makedirs(bsdir)
    os.makedirs(texdir)
    write_json(os.path.join(pack, "pack.mcmeta"),
               {"pack": {"pack_format": 9, "description": "broken"}})
    with open(os.path.join(texdir, "bad.png"), "w") as f:
        f.write("this is not a png")
    write_json(os.path.join(mdir, "cube_bad.json"),
               cube("block/bad", False))
    write_json(os.path.join(bsdir, "atlas_bad.json"),
               {"variants": {"": {"model": "mc:block/cube_bad"}}})
    return pack


# ---------------------------------------------------------------------------
# The reader's own shipped pack, behind completed textures
# ---------------------------------------------------------------------------

def resolve_texture(tex_map: dict, key) -> str | None:
    """The reference's find_texture: the path get_texture_path would
    be handed, None where the model refuses on its way there."""
    seen: set[str] = set()
    while True:
        if isinstance(key, str) and key.startswith("#"):
            key = key[1:]
        if not isinstance(key, str):
            if isinstance(key, dict):
                sprite = key.get("sprite")
                return sprite if isinstance(sprite, str) else None
            return None
        if key in seen:
            return None  # a loop: the model refuses anyway
        seen.add(key)
        val = tex_map.get(key)
        if val is None:
            return key  # absent: the key answers itself
        key = val


def harvest(doc: dict, out: set[str]) -> None:
    """Every texture path one model's fold can name."""
    if not isinstance(doc, dict):
        return
    tex_map = doc.get("textures")
    if not isinstance(tex_map, dict):
        tex_map = {}

    def add(key) -> None:
        target = resolve_texture(tex_map, key)
        if target and "#" not in target:
            out.add(target)

    for val in tex_map.values():
        add(val)
    elements = doc.get("elements")
    if isinstance(elements, list):
        for element in elements:
            if not isinstance(element, dict):
                continue
            faces = element.get("faces")
            if not isinstance(faces, dict):
                continue
            for face in faces.values():
                if isinstance(face, dict):
                    add(face.get("texture"))


def complete_vanilla(src: str, dst: str) -> int:
    """A copy of the fix pack with an opaque RGBA image behind every
    texture its models can name, and with the air ring it does not
    ship: no blockstate, so the reference would answer the missing
    BLOCK — a full cube that culls every probe face. The air model is
    the same empty elements list the crafted pack uses. Returns the
    images written."""
    shutil.copytree(src, dst)
    mgr = make_manager(src)
    targets: set[str] = set()
    for path in sorted(glob.glob(os.path.join(
            dst, "assets", "*", "models", "**", "*.json"), recursive=True)):
        _, namespace, _, *rel = os.path.normpath(
            os.path.relpath(path, dst)).split(os.sep)
        ref = f"{namespace}:" + "/".join(rel)[:-5]
        try:
            doc = mgr._recursive_load_block_model(ref)
        except Exception:
            continue  # refuses at fold; the block-level flow skips it
        harvest(doc, targets)

    from PIL import Image

    img = Image.new("RGBA", (2, 2))
    img.putdata([(255, 255, 255, 255)] * 4)
    wrote = 0
    for target in sorted(targets):
        parts = target.split(":", 1)
        if len(parts) == 2:
            namespace, rel = parts
        else:
            namespace, rel = "minecraft", target
        if not namespace or rel == "missing_no":
            continue  # the sentinel stays the sentinel
        full = os.path.normpath(os.path.join(
            dst, "assets", namespace, "textures", rel + ".png"))
        if not full.startswith(dst + os.sep) or os.path.exists(full):
            continue
        os.makedirs(os.path.dirname(full), exist_ok=True)
        img.save(full, "PNG")
        wrote += 1

    write_json(os.path.join(dst, "assets", "minecraft", "blockstates",
                            "air.json"),
               {"variants": {"": {"model": "minecraft:block/empty"}}})
    write_json(os.path.join(dst, "assets", "minecraft", "models", "block",
                            "empty.json"),
               {"elements": []})
    return wrote


def vanilla_probes(packdir: str) -> list[tuple[str, dict[str, str]]]:
    """One probe per blockstate file, at its first variant key — D35's
    table rides on the meshes D33 pinned, and one real mesh per file
    carries the rows; the full key sweep is the block pin's ground.
    Multipart files keep the block pin's cartesian of when-values
    under a per-file budget."""
    probes: list[tuple[str, dict[str, str]]] = []
    root = os.path.join(packdir, "assets")
    for namespace in sorted(os.listdir(root)):
        if namespace != "minecraft":
            continue  # probes speak `minecraft:<name>`; this pack is vanilla
        bsdir = os.path.join(root, namespace, "blockstates")
        if not os.path.isdir(bsdir):
            continue
        for fname in sorted(os.listdir(bsdir)):
            if not fname.endswith(".json"):
                continue
            name = fname[:-5]
            with open(os.path.join(bsdir, fname), encoding="utf-8") as fh:
                try:
                    doc = json.load(fh)
                except ValueError:
                    continue
            variants = doc.get("variants") if isinstance(doc, dict) else None
            multipart = doc.get("multipart") if isinstance(doc, dict) else None
            if isinstance(variants, dict):
                key = sorted(variants)[0]
                props = {}
                for pair in key.split(","):
                    if "=" in pair:
                        k, v = pair.split("=", 1)
                        props[k] = v
                probes.append((name, props))
            elif isinstance(multipart, list):
                # Every (prop -> value) the whens mention.
                seen: dict[str, set[str]] = {}

                def collect(when) -> None:
                    if isinstance(when, dict):
                        if "OR" in when or "AND" in when:
                            for arm in when.get("OR", when.get("AND", [])):
                                collect(arm)
                        else:
                            for k, v in when.items():
                                if isinstance(v, bool):
                                    v = "true" if v else "false"
                                elif not isinstance(v, str):
                                    v = str(v)
                                seen.setdefault(k, set()).add(v)
                    # non-dict when: reference skips the case entirely

                for case in multipart:
                    if isinstance(case, dict) and "when" in case:
                        collect(case["when"])
                if seen:
                    names = sorted(seen)
                    values = [sorted(seen[k]) for k in names]
                    combos = list(itertools.product(*values))
                    if len(combos) > 36:
                        for n in names:
                            for v in seen[n]:
                                probes.append((name, {n: v}))
                    else:
                        for combo in combos:
                            probes.append(
                                (name, dict(zip(names, combo))))
    return probes


# ---------------------------------------------------------------------------

def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--mcme", required=True)
    args = ap.parse_args()

    import minecraft_model_reader
    vanilla_fix = os.path.join(
        os.path.dirname(minecraft_model_reader.__file__),
        "api", "resource_pack", "java", "java_vanilla_fix")

    load_reference()

    checks = 0
    tmp = tempfile.mkdtemp(prefix="mcme_ref_atlasmesh_")
    try:
        # --- crafted pack: the streams, the buckets, the step, the
        # wrap and the sentinel, through the builder's own table ---
        pack = craft_pack(tmp)
        mgr = make_manager(pack)
        opack, img = build_opack(mgr)
        if img.size[0] != img.size[1]:
            raise AssertionError(f"crafted: reference page {img.size}")
        for name, props in CRAFTED_PROBES:
            checks = check_probe(args.mcme, pack, mgr, opack,
                                 img.size[0], name, props, checks,
                                 "crafted")
        _log("ok — crafted pack: 8 probes — the empty ring block, the "
             "none and cull buckets, the translucent stream, three "
             "rectangles in one bucket, UVs wrapped not clamped, the "
             "sentinel behind a missing texture, and a multipart "
             "merge — every row's rectangle, auv and tint alike")

        # --- the band shade over the world height: the wave the
        # builder reads out of the vertex's y, at every turn ---
        for y in (16, 32, 48, 63, 64, 96, -1, -32):
            checks = check_probe(args.mcme, pack, mgr, opack,
                                 img.size[0], "atlas_cull", {}, checks,
                                 f"world y {y}", y)
        _log("ok — world heights: the cull cube at 16 / 32 / 48 / 63 / "
             "64 / 96 / -1 / -32 — mid-ramp, the peak where t == 1, "
             "folded back past the top, the near-trough, the period, "
             "the next peak, and two below zero where numpy's modulo "
             "parts ways with C's fmod")

        # --- the cull clauses: which rows SURVIVE a neighbour ---
        for probe, beside, ring_name, beside_t in CULL_PROBES:
            # Both sides must answer the neighbour's transparency
            # alike — the field block's flag, and the spec mcme is
            # told (the loader's missing BLOCK when the pack does
            # not hold it).
            want_t = ref_get_model(mgr, ring_name, {})["transparent"]
            if want_t != beside_t:
                raise AssertionError(
                    f"cull {probe} beside {beside}: the field block "
                    f"{ring_name} answers transparency {want_t}, "
                    f"want {beside_t}")
            told_t = ref_get_model(mgr, beside.split(":", 1)[1],
                                   {})["transparent"]
            if told_t != beside_t:
                raise AssertionError(
                    f"cull {probe} beside {beside}: the reference "
                    f"answers transparency {told_t}, want {beside_t}")
            checks = check_probe(args.mcme, pack, mgr, opack,
                                 img.size[0], probe, {}, checks,
                                 f"cull beside {beside}",
                                 beside=beside, ring=(ring_name, {}),
                                 beside_t=beside_t)
        _log("ok — cull clauses: 9 probes — the cull cube and the "
             "glass against opaque and translucent neighbours, the "
             "cullface-less cube standing under both, the missing "
             "BLOCK opaque on both sides, and the pack's own air "
             "answering the default")

        # --- the usage refusals, the block prelude's own ---
        checks = check_usage(args.mcme, pack, tmp, checks)
        _log("ok — usage refusals: 1 / 2 / 1 / 1 / 1 / 1 / 1 / 1, "
             "and the typed state meshing the stateless answer "
             "(D43)")

        # --- the pack whose texture is not an image: each side
        # pinned to its own refusal ---
        broken = craft_broken(tmp)
        checks = check_broken(args.mcme, broken, checks)
        _log("ok — broken image: the reference never finishes loading "
             "(Pillow refuses the file), mcme builds the mesh and "
             "refuses by name at the atlas, exit 6")

        # --- the reader's own shipped pack, real game data ---
        if not os.path.isdir(vanilla_fix):
            raise AssertionError(f"missing reference pack {vanilla_fix}")
        completed = os.path.join(tmp, "vanilla_completed")
        wrote = complete_vanilla(vanilla_fix, completed)
        mgr = make_manager(completed)
        opack, img = build_opack(mgr)
        if img.size[0] != img.size[1]:
            raise AssertionError(f"vanilla_fix: reference page {img.size}")
        probes = vanilla_probes(completed)
        tinted = 0
        for name, props in probes:
            if not in_tint_domain(name, props):
                tinted += 1
                continue
            checks = check_probe(args.mcme, completed, mgr, opack,
                                 img.size[0], name, props, checks,
                                 "vanilla_fix")
        _log(f"ok — java_vanilla_fix: {len(probes) - tinted} probes "
             f"answered alike over real game data — banners, chests, "
             f"heads and statues; {tinted} skipped as blocks the "
             f"reference tints (the block tints stay out of domain — "
             f"under the fold mcme's default would part company with "
             f"the reference's colour), {wrote} opaque images behind "
             f"the pack and the air ring it does not ship")

        # --- the same sweep with a neighbour standing: opaque
        # stone culls every cullface bucket away over real data —
        # the completed pack answers no block translucent (the
        # shipped alpha PNGs are palette images, which the
        # reference's texture rule reads opaque), so the chained
        # pair lives in the crafted probes and real data walks
        # clause one and the exemption ---
        beside_t = ref_get_model(mgr, "stone", {})["transparent"]
        if beside_t != 0:
            raise AssertionError(
                f"vanilla beside stone: transparency {beside_t}, want 0")
        ran = 0
        for name, props in probes:
            if not in_tint_domain(name, props):
                continue
            ran += 1
            checks = check_probe(
                args.mcme, completed, mgr, opack, img.size[0],
                name, props, checks, "vanilla beside stone",
                beside="minecraft:stone", ring=("stone", {}),
                beside_t=beside_t)
        _log(f"ok — java_vanilla_fix beside stone (opaque): {ran} probes "
             "answered alike — the cull clauses over real game data, "
             "the leak riding the partial probes' tails")

        _log(f"ok — {checks} checks")
        return 0
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())
