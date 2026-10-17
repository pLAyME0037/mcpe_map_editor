#!/usr/bin/env python3
"""
Cross-check: `mcme blockmesh` — one block state as the game would
mesh it, answered by mcme and by the reference reader's own
`_get_model` deciding the same question (decisions.md#D33).

How the reference is observed. `_get_model` IS the function mcme
mirrors: it walks the blockstate selection, loads each winning
apply's model, turns it by the apply's x/y and merges what survives
— catching every load failure per apply. The pin calls it directly
with a Block and compares the BlockMesh it returns field for field
(same normalization and 1e-9 tolerance as the mesh pin: the
reference's missing image becomes the token mcme prints). A probe
whose reference answer the block-level flow can never fail — every
raise there is caught — so the mcme side must answer 0 always, with
the same mesh.

The block tint is out of D33's domain: the reference colours the
vertices of redstone wire, the stems and the BlockTints names by
block, while mcme answers the default (1, 1, 1) tint everywhere.
The pin computes the reference's own tint function per probe and
SKIPS exactly the probes it colours (or where it raises on a
missing property) — a documented exclusion, not a silent one.

What is compared:

  CRAFTED PACK — x and y as quarter-turns over real geometry, the
    culling buckets remapped by both passes at once, a negative x
    and a negative non-multiple (Python's floor: -45 is step -1);
    the guard's quirks — an out-of-range roty with rotx zero
    leaving the mesh untouched, an out-of-range rotx (x 360)
    raising there, the apply skipped and the missing block
    answering; the refusal chain (a first matching key whose model
    refuses, the "" key after it still winning; every key refusing
    falling to the missing block); a model-less apply standing in
    as the missing block, unturned; a list apply's first element;
    uvlock read and ignored; a non-number x skipping the apply;
    multipart merging — bucket concatenation, face and texture
    index offsets, the sorted texture set — an empty merge as the
    EMPTY mesh (not the missing block); and the three missing
    shapes (no file, no key) beside it.

  RECORDED DIFFERENCE — one probe pins each side's own order: a
    merge whose parts name the missing texture sorts the texture
    list by different keys (the reference by its package path,
    mcme by the token "missing_no"), so both sides' lists are
    asserted in their own order, sets and geometry alike.

  VANILLA FIX — every blockstate in the reader's own shipped pack,
    one probe per variant key plus the empty probe and, for
    multipart, the cartesian of when-values under a per-file
    budget, against real game data behind completed textures. The
    tinted blocks are skipped by the rule above and counted.

  USAGE — missing arguments exit 1, a pack directory that does not
    exist exit 2, an empty namespace or name exit 1, a non-string
    state exit 1.

Usage:
    venv/bin/python tools/ref_getmodel.py --mcme ./mcme
"""

from __future__ import annotations

import argparse
import glob
import itertools
import json
import math
import os
import shutil
import subprocess
import sys
import tempfile

# Probe budget per vanilla multipart file: the cartesian of its
# when-values, or this many at most (single-prop probes beyond it).
FILE_PROBE_CAP = 36


def _log(msg: str) -> None:
    print(f"ref_getmodel: {msg}", file=sys.stderr)


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


def make_block(name: str, props: dict[str, str]):
    from minecraft_model_reader.api import Block
    import amulet_nbt

    return Block("minecraft", name,
                 {k: amulet_nbt.TAG_String(v) for k, v in props.items()})


def ref_get_model(mgr, name: str, props: dict[str, str]) -> dict:
    """The reference's block-level mesh document. Raises only where
    `_get_model` itself raises — outside its per-apply catches."""
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
    """The reference's own block-tint function — the exclusion rule.
    Returns the (r, g, b) tuple, or None where the function itself
    raises (redstone wire without its power property)."""
    from minecraft_model_reader.api.resource_pack.java.resource_pack_manager import (  # noqa: E501
        _get_block_tint)

    try:
        return tuple(_get_block_tint(make_block(name, props)))
    except Exception:
        return None


DEFAULT_TINT = (1.0, 1.0, 1.0)


def in_tint_domain(name: str, props: dict[str, str]) -> bool:
    """D33 answers every block with the default tint; a block the
    reference colours is out of the pin's domain."""
    t = ref_tint(name, props)
    return t is not None and all(
        math.isclose(a, b, rel_tol=0, abs_tol=1e-12)
        for a, b in zip(t, DEFAULT_TINT))


# ---------------------------------------------------------------------------
# The mcme side
# ---------------------------------------------------------------------------

def mcme_spec(name: str, props: dict[str, str]) -> str:
    spec = f"minecraft:{name}"
    if props:
        inner = ",".join(f'{k}:"{v}"' for k, v in props.items())
        spec += "{" + inner + "}"
    return spec


def mcme_call(mcme: str, packdir: str, name: str,
              props: dict[str, str]) -> tuple[int, dict | None, str]:
    rc, out, err = run(mcme, "blockmesh", packdir,
                       mcme_spec(name, props), "--json")
    if rc == 0:
        try:
            return rc, json.loads(out), err
        except json.JSONDecodeError:
            raise AssertionError(
                f"blockmesh {name}: unparseable JSON {out!r}")
    return rc, None, err


def mcme_mesh(mcme: str, packdir: str, name: str,
              props: dict[str, str]) -> dict:
    rc, doc, err = mcme_call(mcme, packdir, name, props)
    if rc != 0:
        raise AssertionError(
            f"blockmesh {mcme_spec(name, props)}: exit {rc}, "
            f"stderr {err.strip()!r}")
    assert doc is not None
    return doc


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


# ---------------------------------------------------------------------------
# Probe shapes
# ---------------------------------------------------------------------------

def check_probe(mcme, packdir, mgr, name, props, checks, why) -> int:
    """The reference answers (its per-apply catches make the
    block-level flow total) and mcme answers the same mesh."""
    want = ref_get_model(mgr, name, props)
    got = mcme_mesh(mcme, packdir, name, props)
    d = mesh_diff(got, want, mcme_spec(name, props))
    if d:
        raise AssertionError(f"{why}: {d}")
    return checks + 1


def check_recorded_order(mcme, packdir, mgr, name, props, checks,
                         why) -> int:
    """The recorded difference: a merge naming the missing texture
    sorts its list by different keys on each side — the reference
    by the package path its sentinel image ships at, mcme by the
    token it prints. Each side's ORDER is pinned by its own rule
    (raw paths sorted there, tokens sorted here); the texture SETS
    and the geometry agree, with the indexes remapped through the
    difference."""
    from minecraft_model_reader.api.image import missing_no_path

    block = make_block(name, props)
    m = mgr._get_model(block)

    def norm(path):
        if path == missing_no_path or path == getattr(mgr, "missing_no", None):
            return "missing_no"
        return path

    want_raw = list(m.textures)
    want = ref_get_model(mgr, name, props)
    got = mcme_mesh(mcme, packdir, name, props)

    # The reference's rule: the raw list is numpy.unique's sorted
    # set — lexicographic on the paths it holds, the sentinel's own
    # package path among them.
    if want_raw != sorted(want_raw):
        raise AssertionError(f"{why}: ref list not sorted: {want_raw}")
    # mcme's rule: sorted on the strings it stores, the sentinel
    # printed as the token.
    if got["textures"] != sorted(got["textures"]):
        raise AssertionError(f"{why}: mcme list not sorted: "
                             f"{got['textures']}")

    # The sets (normalized) agree, and the sentinel is in both.
    if sorted(map(norm, want_raw)) != sorted(got["textures"]):
        raise AssertionError(
            f"{why}: texture sets differ: ref {want_raw} "
            f"mcme {got['textures']}")
    if "missing_no" not in got["textures"]:
        raise AssertionError(f"{why}: mcme holds no sentinel: "
                             f"{got['textures']}")
    if "missing_no" not in want["textures"]:
        raise AssertionError(f"{why}: ref holds no sentinel: "
                             f"{want['textures']}")

    # Geometry, with mcme's indexes remapped into the reference's
    # order through the shared strings.
    remap = {i: want["textures"].index(t)
             for i, t in enumerate(got["textures"])}
    got_fixed = dict(got)
    got_fixed["textures"] = want["textures"]
    got_fixed["dirs"] = {
        k: dict(v, texture_indexes=[remap[i] for i in v["texture_indexes"]])
        for k, v in got["dirs"].items()}
    d = mesh_diff(got_fixed, want, mcme_spec(name, props))
    if d:
        raise AssertionError(f"{why}: {d}")
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


def crafted_models() -> dict[str, dict]:
    cube_faces = {d: {"texture": "#all"}
                  for d in ("down", "up", "north", "east", "south", "west")}

    def cube6(tex: str) -> dict:
        return {"textures": {"all": tex},
                "elements": [{"from": [0, 0, 0], "to": [16, 16, 16],
                              "faces": dict(cube_faces)}]}

    def partial(tex: str, faces) -> dict:
        return {"textures": {"a": tex},
                "elements": [{"from": [2, 2, 2], "to": [14, 14, 14],
                              "faces": {d: {"texture": "#a"}
                                        for d in faces}}]}

    return {
        # One up face on a partial cube: the none bucket, geometry
        # that moves visibly under a turn.
        "block/tex": partial("block/alpha", ["up"]),
        # The same shape on the other texture, for merges.
        "block/texb": partial("block/opaque", ["up"]),
        # A richer part: three faces, still the none bucket.
        "block/partial3": partial("block/opaque", ["up", "north", "east"]),
        # A full cube culling every direction: six buckets to
        # remap.
        "block/cull6": cube6("block/opaque"),
        # A full six-face cube on a texture the pack lacks: the
        # reference's transparency-table read raises — it refuses
        # to mesh, and the block-level flow skips it.
        "block/full_missing": cube6("block/nonexistent"),
    }


def crafted_blockstates() -> dict[str, dict]:
    v = "variants"
    return {
        "rot_x": {v: {"": {"model": "mc:block/tex", "x": 90}}},
        "rot_y": {v: {"": {"model": "mc:block/cull6", "y": 90}}},
        "rot_xy": {v: {"": {"model": "mc:block/cull6", "x": 90,
                            "y": 90}}},
        # A negative step (in the table) and a negative
        # non-multiple: Python floors -45 to step -1, and the
        # reference turns by it.
        "rot_neg": {v: {"": {"model": "mc:block/tex", "x": -90}}},
        "rot_neg45": {v: {"": {"model": "mc:block/tex", "x": -45}}},
        # y 450 is step 5 — out of the cull map, and with rotx zero
        # the reference's guard returns the mesh untouched.
        "rot_ywide": {v: {"": {"model": "mc:block/tex", "y": 450}}},
        # x 360 is step 4 — out of the map with a nonzero rotx,
        # where the reference's lookup raises: the apply is
        # skipped, nothing matches after it, and the missing block
        # answers.
        "rot_xbad": {v: {"": {"model": "mc:block/tex", "x": 360}}},
        # The first matching key's model refuses; the "" key after
        # it must still win.
        "rot_skip": {v: {"a=1": {"model": "mc:block/full_missing"},
                         "": {"model": "mc:block/tex", "x": 90}}},
        "rot_allrefuse": {v: {"": {"model": "mc:block/full_missing",
                                   "x": 90}}},
        # No model: the missing block stands in, unturned.
        "rot_nomodel": {v: {"": {"x": 90}}},
        # A list apply answers its first element.
        "rot_list": {v: {"": [{"model": "mc:block/tex"},
                              {"model": "mc:block/texb"}]}},
        # uvlock is read and ignored — the reference's standing
        # TODO.
        "rot_uv": {v: {"": {"model": "mc:block/tex", "uvlock": True}}},
        # A non-number x raises there and skips the apply.
        "rot_badx": {v: {"": {"model": "mc:block/tex", "x": "90"}}},
        # Multipart: two applies and one behind a when only a
        # matching block reaches, the last turned as it merges.
        "merge": {"multipart": [
            {"apply": {"model": "mc:block/texb"}},
            {"when": {"a": "1"}, "apply": {"model": "mc:block/partial3"}},
            {"apply": {"model": "mc:block/texb", "y": 90}}]},
        "merge_empty": {"multipart": [
            {"when": {"a": "1"}, "apply": {"model": "mc:block/tex"}}]},
        # A model-less apply merges the missing block in — the
        # recorded sort-order difference lives here.
        "merge_nomodel": {"multipart": [
            {"apply": {"model": "mc:block/texb"}},
            {"apply": {}}]},
        # A file holding neither key is the missing block.
        "neither": {"foo": 1},
    }


def craft_pack(root: str) -> str:
    pack = os.path.join(root, "crafted")
    mdir = os.path.join(pack, "assets", "mc", "models")
    bsdir = os.path.join(pack, "assets", "minecraft", "blockstates")
    os.makedirs(mdir, exist_ok=True)
    os.makedirs(bsdir, exist_ok=True)
    with open(os.path.join(pack, "pack.mcmeta"), "w") as f:
        json.dump({"pack": {"pack_format": 9,
                            "description": "getmodel probes"}}, f)

    # Textures live under minecraft: a bare face reference resolves
    # there, the way the game's own packs do.
    opaque = [(255, 255, 255, 255), (255, 0, 0, 255),
              (0, 255, 0, 255), (0, 0, 255, 255)]
    alpha = [(255, 255, 255, 255), (255, 0, 0, 255),
             (0, 255, 0, 128), (0, 0, 255, 255)]
    texdir = os.path.join(pack, "assets", "minecraft", "textures")
    write_png(os.path.join(texdir, "block", "opaque.png"), "RGBA", opaque)
    write_png(os.path.join(texdir, "block", "alpha.png"), "RGBA", alpha)

    for rel, content in crafted_models().items():
        path = os.path.join(mdir, rel + ".json")
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, "w") as f:
            json.dump(content, f)
    for name, content in crafted_blockstates().items():
        with open(os.path.join(bsdir, name + ".json"), "w") as f:
            json.dump(content, f)
    return pack


CRAFTED_PROBES = [
    ("rot_x", {}), ("rot_y", {}), ("rot_xy", {}),
    ("rot_neg", {}), ("rot_neg45", {}),
    ("rot_ywide", {}), ("rot_xbad", {}),
    # The refusal chain, with and without the matching state — the
    # variants rule matches a property the block lacks, so both
    # reach the same two keys in the same order.
    ("rot_skip", {"a": "1"}), ("rot_skip", {}),
    ("rot_allrefuse", {}), ("rot_nomodel", {}),
    ("rot_list", {}), ("rot_uv", {}), ("rot_badx", {}),
    ("merge", {}), ("merge", {"a": "1"}),
    ("merge_empty", {}),
    ("neither", {}),
    ("absent", {}),                      # no file: the missing block
]

# Blocks the reference colours: D33 answers the default (1, 1, 1)
# tint everywhere, so the pin's exclusion must catch each of these —
# pinned by asserting the filter skips them, not by hoping it does.
TINTED_PROBES = [
    ("grass_block", {}),                 # BlockTints by name
    ("redstone_wire", {"power": "9"}),   # RedstoneTints by property
    ("redstone_wire", {}),               # the property is missing: the
                                         # reference's lookup raises
    ("pumpkin_stem", {"age": "4"}),      # StemTints by property
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
    texture its models can name. The pack ships 22 PNGs; without
    the rest every full-cube model dies in the reference's
    transparency table. Returns the images written."""
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
    return wrote


def vanilla_probes(packdir: str) -> list[tuple[str, dict[str, str]]]:
    """One probe per variant key plus the empty probe; for multipart,
    the cartesian of when-values under a per-file budget — the block
    pin's own generator, over real game data."""
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
            probes.append((name, {}))
            variants = doc.get("variants") if isinstance(doc, dict) else None
            multipart = doc.get("multipart") if isinstance(doc, dict) else None
            if isinstance(variants, dict):
                for key in variants:
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
                    if len(combos) > FILE_PROBE_CAP:
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

    checks = 0
    tmp = tempfile.mkdtemp(prefix="mcme_ref_getmodel_")
    try:
        # --- crafted pack: turns, guards, chains and merges,
        # decided by both readers ---
        pack = craft_pack(tmp)
        mgr = make_manager(pack)
        for name, props in CRAFTED_PROBES:
            checks = check_probe(args.mcme, pack, mgr, name, props,
                                 checks, "crafted")
        _log(f"ok — crafted pack: {len(CRAFTED_PROBES)} probes — turns "
             "and bucket remaps, the guard's two quirks, the refusal "
             "chain, model-less applies and multipart merges alike")

        # --- the recorded difference: the sentinel's sort position
        checks = check_recorded_order(args.mcme, pack, mgr,
                                      "merge_nomodel", {}, checks,
                                      "recorded")
        _log("ok — recorded difference: a merge naming the missing "
             "texture sorts its list by different keys — the "
             "reference by its package path, mcme by the token it "
             "prints — and the indexes follow each side's own order")

        # --- the tint exclusion itself: the blocks the reference
        # colours must be caught by the filter (no pack files needed —
        # the tint decision is the Block's own) ---
        for name, props in TINTED_PROBES:
            if in_tint_domain(name, props):
                raise AssertionError(
                    f"tint filter: {name} {props} reached the "
                    "comparison — the reference colours it and mcme "
                    "answers the default tint")
            checks += 1
        _log("ok — tint filter: 4 blocks the reference colours — grass "
             "by name, redstone wire and stems by property, and the "
             "missing-property case where its lookup raises — are "
             "excluded by rule")

        # --- the reader's own shipped pack, real game data ---
        if not os.path.isdir(vanilla_fix):
            raise AssertionError(f"missing reference pack {vanilla_fix}")
        completed = os.path.join(tmp, "vanilla_completed")
        wrote = complete_vanilla(vanilla_fix, completed)
        mgr = make_manager(completed)
        probes = vanilla_probes(completed)
        tinted = 0
        for name, props in probes:
            if not in_tint_domain(name, props):
                tinted += 1
                continue
            checks = check_probe(args.mcme, completed, mgr, name, props,
                                 checks, "vanilla_fix")
        _log(f"ok — java_vanilla_fix: {len(probes) - tinted} probes "
             f"answered alike — selection, turns, refusals skipped "
             f"and merges over real game data; {tinted} skipped as "
             f"blocks the reference tints (redstone wire, stems and "
             f"the BlockTints names — D33 answers the default tint), "
             f"{wrote} opaque images behind the pack")

        # --- usage refusals ---
        rc, _out, _err = run(args.mcme, "blockmesh")
        if rc != 1:
            raise AssertionError(f"`blockmesh` alone: exit {rc}, want 1")
        checks += 1
        rc, _out, _err = run(args.mcme, "blockmesh",
                             os.path.join(tmp, "no-such-pack"),
                             "rot_x")
        if rc != 2:
            raise AssertionError(f"missing pack dir: exit {rc}, want 2")
        checks += 1
        rc, _out, _err = run(args.mcme, "blockmesh", pack, ":rot_x")
        if rc != 1:
            raise AssertionError(f"empty namespace: exit {rc}, want 1")
        checks += 1
        rc, _out, _err = run(args.mcme, "blockmesh", pack, "minecraft:")
        if rc != 1:
            raise AssertionError(f"empty name: exit {rc}, want 1")
        checks += 1
        rc, _out, _err = run(args.mcme, "blockmesh", pack,
                             "minecraft:rot_x{a:1}")
        if rc != 1:
            raise AssertionError(f"non-string state: exit {rc}, want 1")
        checks += 1
        _log("ok — usage refusals: 1 / 2 / 1 / 1 / 1")

        _log(f"ok — {checks} checks")
        return 0
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())
