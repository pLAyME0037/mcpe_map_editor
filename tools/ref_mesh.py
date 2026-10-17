#!/usr/bin/env python3
"""
Cross-check: `mcme mesh` — one model as the game would build it,
answered by mcme and by the reference reader's own
`_load_block_model` deciding the same question (decisions.md#D32).

How the reference is observed. `_load_block_model` IS the function
mcme mirrors: it folds the model, resolves its textures against the
pack's image table, lays the elements through its cube tables,
buckets every face by cull direction and settles the block's
transparency. The pin calls it directly with its default tint
(1, 1, 1) — mcme's own — and compares the BlockMesh it returns
field for field: face_mode, the texture list (the reference's own
missing image normalized to the token mcme prints), the
transparency enum, and each present bucket's flat arrays. The
floats compare with a 1e-9 tolerance: everything is dyadic except
rotation, where numpy's trigonometry and libm's may differ by a
ULP. A reference that raises is its refusal — mcme must exit 3 for
the same model.

What is compared:

  CRAFTED PACK — real PNGs behind each transparency outcome (RGBA
    opaque, RGBA with an alpha below 255, RGB-mode) so the ladder
    decides on both sides: a full unrotated six-face element
    turning translucent then opaque, RGB-mode opaque by
    definition, a full block whose texture the pack lacks refused
    by the reference's own KeyError and by mcme by name; the
    missing BLOCK for textures without elements, an empty
    elements list counting as none, the empty mesh for neither;
    the reference's cube tables vertex for vertex through its
    box-derived default UVs, a face rotation cutting the corner
    lut — the negative cut wrapping to its positive twin, 45
    degrees not cutting at all; cullface routing the bucket while
    the face keeps its own corners, an unknown direction and a
    non-string sharing the none bucket; the #chain and its
    self-loop refusal, the sprite pass-through, a face without a
    texture refused by both; the namespace-blind texture dedup
    across two namespaces; an image under a useless group skipped
    whole; element rotation — axis and origin, rescale, the
    1.21.11 multi-axis overrides, an axis naming no direction
    standing still — and the garbage shapes (a two-number origin,
    a ragged or two-number from, a non-number uv, elements not a
    list) each raising there and refused here.

  RECORDED DIFFERENCES — each side pinned to its own observable:
    a textures value that is not a dict (the reference raises
    while FOLDING, on its `.items()`; mcme's fold drops the value
    the way it drops a non-string parent and the mesh answers the
    empty document), and a from with four numbers against a
    three-number to (both refuse there: numpy's array is
    inhomogeneous) beside the even four-and-four case (its numpy
    builds the box from the first three columns, mcme refuses
    past three).

  VANILLA FIX — every model file in the reader's own shipped pack,
    by `namespace:path` and, for a sample, by bare path. The pack
    ships only 22 PNGs, so a COPY of it first gets an opaque image
    behind every texture its models can name — through the
    textures dict or straight from a face: without them every
    full-cube model dies in the reference's transparency table.
    The original pack is never written to. Some of its models the
    reference itself cannot mesh — a texture key that references
    itself with a face on it, elements without a textures dict —
    and those are its refusals: mcme must exit 3 on the same
    files.

  USAGE — missing arguments exit 1, a pack directory that does not
    exist exit 2, a malformed reference exit 1.

Usage:
    venv/bin/python tools/ref_mesh.py --mcme ./mcme
"""

from __future__ import annotations

import argparse
import glob
import json
import math
import os
import shutil
import subprocess
import sys
import tempfile

BARE_PROBE_COUNT = 25

EMPTY_MESH = {"face_mode": 3, "textures": [], "transparent": 2, "dirs": {}}


def _log(msg: str) -> None:
    print(f"ref_mesh: {msg}", file=sys.stderr)


def run(mcme: str, *args: str) -> tuple[int, str, str]:
    """Run mcme, return (exit code, stdout, stderr)."""
    proc = subprocess.run(
        [mcme, *args], capture_output=True, text=True, timeout=120)
    return proc.returncode, proc.stdout, proc.stderr


# ---------------------------------------------------------------------------
# The reference side
# ---------------------------------------------------------------------------

def make_manager(packdir: str):
    """The reference manager over one pack, load and all."""
    os.environ.setdefault("CACHE_DIR", tempfile.mkdtemp(prefix="mcme_cache_"))
    from minecraft_model_reader.api.resource_pack.java import (
        JavaResourcePack, JavaResourcePackManager)

    return JavaResourcePackManager([JavaResourcePack(packdir)], load=True)


def ref_mesh(mgr, ref: str) -> dict:
    """The reference's mesh document for one model.

    Raises whatever its fold or `_load_block_model` raises — the
    probes below treat that as the reference's refusal.
    """
    from minecraft_model_reader.api.image import missing_no_path

    m = mgr._load_block_model(ref)

    def norm(path):
        if path == missing_no_path or path == getattr(mgr, "missing_no", None):
            return "missing_no"
        return path

    dirs = {}
    for key, verts in m.verts.items():
        name = "none" if key is None else key
        dirs[name] = {
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


# ---------------------------------------------------------------------------
# The mcme side
# ---------------------------------------------------------------------------

def mcme_call(mcme: str, packdir: str, ref: str
              ) -> tuple[int, dict | None, str]:
    rc, out, err = run(mcme, "mesh", packdir, ref, "--json")
    if rc == 0:
        try:
            return rc, json.loads(out), err
        except json.JSONDecodeError:
            raise AssertionError(f"mesh {ref}: unparseable JSON {out!r}")
    return rc, None, err


def mcme_mesh(mcme: str, packdir: str, ref: str) -> dict:
    rc, doc, err = mcme_call(mcme, packdir, ref)
    if rc != 0:
        raise AssertionError(
            f"mesh {ref}: exit {rc}, stderr {err.strip()!r}")
    assert doc is not None
    return doc


def mesh_diff(got, want, path: str = "mesh") -> str | None:
    """Where two mesh documents part company, or None for equal.

    Numbers: exact for two ints, a 1e-9 tolerance when a float is
    involved — rotation goes through numpy's trigonometry and
    libm's, which may differ by a ULP; everything else is dyadic
    and could not pass a tolerance it fails.
    """
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
        # The schemas carry no booleans; a stray one must not read
        # as the 1 or 0 it equals in Python.
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


# ---------------------------------------------------------------------------
# Probe shapes
# ---------------------------------------------------------------------------

def check_probe(mcme, packdir, mgr, ref, checks, why) -> int:
    try:
        want = ref_mesh(mgr, ref)
    except Exception as e:
        raise AssertionError(
            f"{why}: ref raised {type(e).__name__} on {ref}: {e}")
    got = mcme_mesh(mcme, packdir, ref)
    d = mesh_diff(got, want, ref)
    if d:
        raise AssertionError(f"{why}: {d}")
    return checks + 1


def check_fail(mcme, packdir, mgr, ref, exc, checks, why) -> int:
    try:
        ref_mesh(mgr, ref)
    except exc:
        pass
    else:
        raise AssertionError(
            f"{why}: ref accepted {ref}, expected {exc.__name__}")
    rc, _doc, err = mcme_call(mcme, packdir, ref)
    if rc != 3:
        raise AssertionError(
            f"{why}: {ref}: mcme exit {rc}, want 3 "
            f"(stderr {err.strip()!r})")
    return checks + 1


def check_vanilla(mcme, packdir, mgr, ref, checks, why) -> tuple[int, str]:
    """Real game data: the reference's answer, or its refusal. The
    shipped pack holds models it cannot mesh — a texture key that
    references itself, elements without a textures dict — and
    those are its refusals: mcme must exit 3 on the same files."""
    try:
        want = ref_mesh(mgr, ref)
    except Exception as e:
        rc, _doc, err = mcme_call(mcme, packdir, ref)
        if rc != 3:
            raise AssertionError(
                f"{why}: {ref}: ref raises {type(e).__name__} ({e}), "
                f"mcme exit {rc} (stderr {err.strip()!r})")
        return checks + 1, "refused"
    got = mcme_mesh(mcme, packdir, ref)
    d = mesh_diff(got, want, ref)
    if d:
        raise AssertionError(f"{why}: {d}")
    return checks + 1, "answered"


def check_recorded(mcme, packdir, mgr, ref, exc, checks, why,
                   recorded: dict) -> int:
    """The reference raises; mcme answers — and its answer is the
    recorded document, asserted both ways (the D31 pattern)."""
    try:
        ref_mesh(mgr, ref)
    except exc:
        pass
    else:
        raise AssertionError(
            f"{why}: ref accepted {ref}, expected {exc.__name__}")
    got = mcme_mesh(mcme, packdir, ref)
    d = mesh_diff(got, recorded, ref)
    if d:
        raise AssertionError(f"{why}: recorded answer: {d}")
    return checks + 1


def check_recorded_build(mcme, packdir, mgr, ref, checks, why) -> int:
    """The opposite direction: the reference's answer is pinned —
    its numpy still builds a box from the first three columns of a
    four-number from, while mcme refuses past three."""
    want = ref_mesh(mgr, ref)  # must succeed: it BUILDS
    faces = want["dirs"].get("none", {}).get("faces", [])
    if not faces:
        raise AssertionError(f"{why}: ref built no faces for {ref}")
    rc, _doc, err = mcme_call(mcme, packdir, ref)
    if rc != 3:
        raise AssertionError(
            f"{why}: {ref}: mcme exit {rc}, want 3 "
            f"(stderr {err.strip()!r})")
    return checks + 1


# ---------------------------------------------------------------------------
# The crafted pack
# ---------------------------------------------------------------------------

def write_png(path: str, mode: str, pixels) -> None:
    from PIL import Image

    os.makedirs(os.path.dirname(path), exist_ok=True)
    img = Image.new(mode, (2, 2))
    img.putdata(list(pixels))
    img.save(path, "PNG")


def crafted_models() -> dict[str, dict | str]:
    cube_faces = {d: {"texture": "#all"}
                  for d in ("down", "up", "north", "east", "south", "west")}

    def cube(tex: str) -> dict:
        return {"textures": {"all": tex},
                "elements": [{"from": [0, 0, 0], "to": [16, 16, 16],
                              "faces": dict(cube_faces)}]}

    def slab(rotation: dict) -> dict:
        return {"textures": {"all": "block/opaque"},
                "elements": [{"from": [0, 0, 0], "to": [16, 8, 16],
                              "rotation": rotation,
                              "faces": {"up": {"texture": "#all"}}}]}

    def uvrot(rotation) -> dict:
        return {"texture": "#a", "uv": [0, 0, 16, 8],
                "rotation": rotation}

    return {
        # --- the transparency ladder and the missing texture ---
        "block/full_opaque": cube("block/opaque"),
        "block/full_alpha": cube("block/alpha"),
        "block/full_rgb": cube("block/rgb"),
        "block/full_missing": cube("block/nonexistent"),
        "block/partial": {
            "textures": {"a": "block/nonexistent"},
            "elements": [{"from": [0, 0, 0], "to": [16, 8, 16],
                          "faces": {"up": {"texture": "#a"},
                                    "north": {"texture": "#a"},
                                    "east": {"texture": "#a"}}}]},
        # --- bucket routing: cullface names it, the face keeps
        # its own corners; unknown and non-string share none ---
        "block/cull": {
            "textures": {"all": "block/opaque"},
            "elements": [{"faces": {
                "down": {"texture": "#all"},
                "up": {"texture": "#all", "cullface": "down"},
                "north": {"texture": "#all", "cullface": "up"},
                "east": {"texture": "#all", "cullface": "sideways"},
                "south": {"texture": "#all", "cullface": 42},
                "west": {"texture": "#all", "cullface": "west"}}}]},
        # --- the uv corner cut: 90, the negative twin of -90,
        # 45 not cutting, 270 cutting ---
        "block/uvrot": {
            "textures": {"a": "block/opaque"},
            "elements": [{"from": [0, 0, 0], "to": [8, 8, 8],
                          "faces": {
                              "north": uvrot(90),
                              "south": uvrot(-90),
                              "east": uvrot(45),
                              "west": uvrot(270)}}]},
        # --- chains, sprites, refusals ---
        "block/chain": {"textures": {"a": "#b", "b": "block/opaque"},
                        "elements": [{"faces": {"north": {"texture": "#a"}}}]},
        "block/selfloop": {"textures": {"a": "#a"},
                           "elements": [{"faces": {"north": {"texture": "#a"}}}]},
        "block/sprite": {"textures": {},
                         "elements": [{"faces": {
                             "north": {"texture": {"sprite": "block/opaque"}}}}]},
        "block/no_face_texture": {
            "textures": {"a": "block/opaque"},
            "elements": [{"faces": {"up": {"uv": [0, 0, 16, 16]}}}]},
        "block/baduv": {"textures": {"a": "block/opaque"},
                        "elements": [{"faces": {
                            "up": {"texture": "#a", "uv": [0, 0, 1]}}}]},
        # --- the missing BLOCK, and the empty answers ---
        "block/tex_noelem": {"textures": {"all": "block/opaque"}},
        "block/empty_elems": {"textures": {"all": "block/opaque"},
                              "elements": []},
        "block/particle_only": {"textures": {"particle": "block/opaque"}},
        "block/elemsstr": {"textures": {"particle": "block/opaque"},
                           "elements": ""},
        "block/notex_elem": {"elements": [{"from": [0, 0, 0],
                                           "to": [8, 8, 8]}]},
        "block/empty": {},
        "block/bad": "{not json at all",
        # --- element rotation: axis, rescale, overrides, no axis ---
        "block/elementrot": slab({"origin": [8, 8, 8], "axis": "y",
                                  "angle": 90}),
        "block/elementrot_rescale": slab({"origin": [8, 8, 8], "axis": "y",
                                          "angle": 45, "rescale": True}),
        "block/elementrot_multi": slab({"origin": [8, 8, 8], "axis": "x",
                                        "angle": 90, "y": 45}),
        "block/elementrot_axis42": slab({"origin": [8, 8, 8], "axis": 42,
                                         "angle": 90}),
        # --- dedup and the useless image group ---
        "block/dupe": {"textures": {},
                       "elements": [{"faces": {
                           "up": {"texture": "block/dupe"},
                           "north": {"texture": "other:block/dupe"}}}]},
        "block/skipme": {"textures": {"s": "gui/skipme"},
                         "elements": [{"faces": {"up": {"texture": "#s"}}}]},
        # --- garbage: each of these raises in the reference ---
        "block/elements_garbage": {"textures": {"a": "block/opaque"},
                                   "elements": 42},
        "block/bad_origin": {
            "textures": {"all": "block/opaque"},
            "elements": [{"from": [0, 0, 0], "to": [16, 16, 16],
                          "rotation": {"origin": [8, 8], "axis": "y",
                                       "angle": 90},
                          "faces": {"up": {"texture": "#all"}}}]},
        "block/bad_from_ragged": {"textures": {"a": "block/opaque"},
                                  "elements": [{"from": [0, 0],
                                                "faces": {
                                                    "up": {"texture": "#a"}}}]},
        "block/bad_from_short": {"textures": {"a": "block/opaque"},
                                 "elements": [{"from": [0, 0],
                                               "to": [16, 8],
                                               "faces": {
                                                   "up": {"texture": "#a"}}}]},
        "block/texstr_elem": {"textures": "side",
                              "elements": [{"faces": {
                                  "up": {"texture": "#a"}}}]},
        "block/elemsnull": {"elements": None},
        "block/elemsnum": {"elements": 0},
        # --- recorded differences: each side pinned below ---
        "block/texstr": {"textures": "side"},
        "block/texnum": {"textures": 42},
        "block/texnull": {"textures": None},
        "block/from4_ragged": {"textures": {"a": "block/opaque"},
                               "elements": [{"from": [0, 0, 0, 4],
                                             "faces": {"north": {"texture": "#a"}}}]},
        "block/from4_even": {"textures": {"a": "block/opaque"},
                             "elements": [{"from": [0, 0, 0, 4],
                                           "to": [16, 16, 16, 16],
                                           "faces": {"north": {"texture": "#a"}}}]},
    }


def craft_pack(root: str) -> str:
    pack = os.path.join(root, "crafted")
    # Model keys carry their own "block/..." — the directory holds
    # the namespace root only.
    mdir = os.path.join(pack, "assets", "mc", "models")
    os.makedirs(mdir, exist_ok=True)
    with open(os.path.join(pack, "pack.mcmeta"), "w") as f:
        json.dump({"pack": {"pack_format": 9,
                            "description": "mesh probes"}}, f)

    # Textures live under minecraft: a bare face reference resolves
    # there, the way the game's own packs do.
    opaque = [(255, 255, 255, 255), (255, 0, 0, 255),
              (0, 255, 0, 255), (0, 0, 255, 255)]
    alpha = [(255, 255, 255, 255), (255, 0, 0, 255),
             (0, 255, 0, 128), (0, 0, 255, 255)]
    rgb = [(255, 255, 255), (255, 0, 0), (0, 255, 0), (0, 0, 255)]
    texdir = os.path.join(pack, "assets", "minecraft", "textures")
    write_png(os.path.join(texdir, "block", "opaque.png"), "RGBA", opaque)
    write_png(os.path.join(texdir, "block", "alpha.png"), "RGBA", alpha)
    write_png(os.path.join(texdir, "block", "rgb.png"), "RGB", rgb)
    # Under a useless image group: the loader skips it whole.
    write_png(os.path.join(texdir, "gui", "skipme.png"), "RGBA", opaque)
    # Two namespaces, one dedup key.
    write_png(os.path.join(texdir, "block", "dupe.png"), "RGBA", opaque)
    write_png(os.path.join(pack, "assets", "other", "textures",
                           "block", "dupe.png"), "RGBA", alpha)

    for rel, content in crafted_models().items():
        path = os.path.join(mdir, rel + ".json")
        os.makedirs(os.path.dirname(path), exist_ok=True)
        if isinstance(content, str):  # the unparseable file
            with open(path, "w") as f:
                f.write(content)
        else:
            with open(path, "w") as f:
                json.dump(content, f)
    return pack


CRAFTED_ANSWERS = [
    "mc:block/full_opaque",
    "mc:block/full_alpha",
    "mc:block/full_rgb",
    "mc:block/partial",
    "mc:block/cull",
    "mc:block/uvrot",
    "mc:block/chain",
    "mc:block/sprite",
    "mc:block/tex_noelem",
    "mc:block/empty_elems",
    "mc:block/particle_only",
    "mc:block/elemsstr",
    "mc:block/empty",
    "mc:block/bad",
    "mc:block/elementrot",
    "mc:block/elementrot_rescale",
    "mc:block/elementrot_multi",
    "mc:block/elementrot_axis42",
    "mc:block/dupe",
    "mc:block/skipme",
    "block/absent",                  # bare: the minecraft namespace
    "minecraft:block/full_opaque",   # named fully: models live in mc
    "absent_namespace:block/full_opaque",
]

# (ref, the exception class the reference raises there)
CRAFTED_FAILURES = [
    ("mc:block/full_missing", KeyError),       # the transparency table
    ("mc:block/selfloop", RuntimeError),       # a texture key cycle
    ("mc:block/notex_elem", KeyError),         # java_model["textures"]
    ("mc:block/no_face_texture", AttributeError),
    ("mc:block/baduv", ValueError),
    ("mc:block/elements_garbage", TypeError),
    ("mc:block/bad_origin", ValueError),       # two-number unpack
    ("mc:block/bad_from_ragged", ValueError),  # numpy: inhomogeneous
    ("mc:block/bad_from_short", IndexError),   # numpy: no third column
    ("mc:block/from4_ragged", ValueError),     # 4 vs the default 3
    ("mc:block/texstr_elem", AttributeError),  # fold: .items()
    ("mc:block/elemsnull", TypeError),         # for element in None
    ("mc:block/elemsnum", TypeError),          # for element in 0
]

CRAFTED_RECORDED = [
    "mc:block/texstr",
    "mc:block/texnum",
    "mc:block/texnull",
]


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
    """Every texture path one model's fold can name: the textures
    dict's values walked through their chains, and each face's own
    texture (which may name a key the dict never held)."""
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
    texture its models can name. The pack ships 22 PNGs; without
    the rest every full-cube model dies in the reference's
    transparency table. Returns the images written. The original is
    never written to — the manager below only reads it, to fold."""
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
            continue  # refuses at fold; the sweep probes it as itself
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
    return wrote


def vanilla_probes(packdir: str) -> list[str]:
    """Every model file the reference's own glob would find, by
    `namespace:path`; a sample also by bare path."""
    probes: list[str] = []
    for path in sorted(glob.glob(os.path.join(
            packdir, "assets", "*", "models", "**", "*.json"),
            recursive=True)):
        _, namespace, _, *rel = os.path.normpath(
            os.path.relpath(path, packdir)).split(os.sep)
        probes.append(f"{namespace}:" + "/".join(rel)[:-5])
    step = max(1, len(probes) // BARE_PROBE_COUNT)
    bare = [p.split(":", 1)[1] for p in probes[::step][:BARE_PROBE_COUNT]]
    return probes + bare


# ---------------------------------------------------------------------------

def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--mcme", required=True)
    args = ap.parse_args()

    import minecraft_model_reader
    vanilla_fix = os.path.join(
        os.path.dirname(minecraft_model_reader.__file__),
        "api", "resource_pack", "java", "java_vanilla_fix")

    checks = 0
    tmp = tempfile.mkdtemp(prefix="mcme_ref_mesh_")
    try:
        # --- crafted pack: every mesh quirk, decided by both readers ---
        pack = craft_pack(tmp)
        mgr = make_manager(pack)
        for ref in CRAFTED_ANSWERS:
            checks = check_probe(args.mcme, pack, mgr, ref, checks,
                                 "crafted")
        _log(f"ok — crafted pack: {len(CRAFTED_ANSWERS)} answer probes, "
             "cubes, buckets, cuts, chains and rotations alike on both sides")

        for ref, exc in CRAFTED_FAILURES:
            checks = check_fail(args.mcme, pack, mgr, ref, exc, checks,
                                "crafted")
        _log(f"ok — crafted refusals: {len(CRAFTED_FAILURES)} probes, "
             "each exception class the reference's own")

        for ref in CRAFTED_RECORDED:
            checks = check_recorded(args.mcme, pack, mgr, ref,
                                    AttributeError, checks, "recorded",
                                    recorded=EMPTY_MESH)
        checks = check_recorded_build(args.mcme, pack, mgr,
                                      "mc:block/from4_even", checks,
                                      "recorded")
        _log("ok — recorded differences: a non-dict textures (the reference "
             "raises folding it, mcme drops it and answers the empty "
             "document), a four-number from (its numpy builds the first "
             "three columns, mcme refuses past three)")

        # --- the reader's own shipped pack, real game data ---
        if not os.path.isdir(vanilla_fix):
            raise AssertionError(f"missing reference pack {vanilla_fix}")
        completed = os.path.join(tmp, "vanilla_completed")
        wrote = complete_vanilla(vanilla_fix, completed)
        mgr = make_manager(completed)
        probes = vanilla_probes(completed)
        answered = refused = 0
        for ref in probes:
            checks, kind = check_vanilla(
                args.mcme, completed, mgr, ref, checks, "vanilla_fix")
            if kind == "refused":
                refused += 1
            else:
                answered += 1
        _log(f"ok — java_vanilla_fix: {len(probes)} probes — {answered} "
             f"answered alike, {refused} refused by both (self-looping "
             "texture keys, elements without a textures dict), "
             f"{wrote} opaque images behind them")

        # --- usage refusals ---
        rc, _out, _err = run(args.mcme, "mesh")
        if rc != 1:
            raise AssertionError(f"`mesh` alone: exit {rc}, want 1")
        checks += 1
        rc, _out, _err = run(args.mcme, "mesh",
                             os.path.join(tmp, "no-such-pack"),
                             "block/full_opaque")
        if rc != 2:
            raise AssertionError(f"missing pack dir: exit {rc}, want 2")
        checks += 1
        rc, _out, _err = run(args.mcme, "mesh", pack, ":block/full_opaque")
        if rc != 1:
            raise AssertionError(f"empty namespace: exit {rc}, want 1")
        checks += 1
        rc, _out, _err = run(args.mcme, "mesh", pack, "minecraft:")
        if rc != 1:
            raise AssertionError(f"empty path: exit {rc}, want 1")
        checks += 1
        _log("ok — usage refusals: 1 / 2 / 1 / 1")

        _log(f"ok — {checks} checks")
        return 0
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())
