#!/usr/bin/env python3
"""
Cross-check: `mcme model` — one model file with its parent chain
folded in, answered by mcme and by the reference reader's own
`_recursive_load_block_model` deciding the same question
(decisions.md#D31).

How the reference is observed. The fold is pure JSON work — no
texture, no mesh — so the reference's `_recursive_load_block_model`
is called directly: it IS the function mcme mirrors, and it returns
the folded dict. Every probe compares mcme's printed document
(parsed back) with the reference's own return value, dict for
dict.

What is compared:

  CRAFTED PACK — textures merging per key down a parent chain
    (keys the child does not name survive from the parent),
    elements overriding wholesale (nearest ancestor wins the whole
    list, an EMPTY list included), every other key — display,
    gui_light, parent — discarded, an unknown parent folding onto
    the empty object, a missing file answering `{}` at any level,
    a bare model reference defaulting to the minecraft namespace,
    a cross-namespace parent, an unparseable file both readers
    skip, a top-level array folding to `{}`, and texture values
    riding along at face value (a number, a boolean, a null).

  RECORDED DIFFERENCES — three shapes where the two sides are
    allowed to differ, each pinning mcme's documented answer
    against the reference's observable behavior: a `parent` that
    is not a string (the reference raises on its `.split`; mcme
    reads it as no parent), a parent CYCLE (the reference raises
    RecursionError; mcme refuses by name, exit 3), and a chain
    deeper than mcme's cap (the reference answers, up to Python's
    recursion limit; mcme refuses, exit 3).

  VANILLA FIX — every model file in the reader's own shipped
    java_vanilla_fix pack, by `namespace:path` and, for a sample,
    by bare path: real game data through both folds.

  USAGE — missing arguments exit 1, a pack directory that does not
    exist exit 2, an empty namespace or path exit 1.

Usage:
    venv/bin/python tools/ref_model.py --mcme ./mcme
"""

from __future__ import annotations

import argparse
import glob
import json
import os
import subprocess
import sys
import tempfile

# Bare-path probes sampled across the vanilla pack (a path without
# `namespace:` defaults to minecraft on both sides).
BARE_PROBE_COUNT = 25


def _log(msg: str) -> None:
    print(f"ref_model: {msg}", file=sys.stderr)


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


def ref_fold(mgr, ref: str) -> dict:
    """The reference's own fold of one model reference."""
    return mgr._recursive_load_block_model(ref)


# ---------------------------------------------------------------------------
# The mcme side
# ---------------------------------------------------------------------------

def mcme_fold(mcme: str, packdir: str, ref: str) -> dict:
    rc, out, err = run(mcme, "model", packdir, ref, "--json")
    if rc != 0:
        raise AssertionError(
            f"model {ref}: exit {rc}, stderr {err.strip()!r}")
    try:
        return json.loads(out)
    except json.JSONDecodeError:
        raise AssertionError(f"model: stdout is not JSON: {out!r}")


def check_probe(mcme: str, packdir: str, mgr, ref: str,
                checks: int, why: str) -> int:
    want = ref_fold(mgr, ref)
    got = mcme_fold(mcme, packdir, ref)
    if got != want:
        raise AssertionError(
            f"{ref} ({why}): mcme folds to {json.dumps(got, sort_keys=True)} "
            f"!= ref {json.dumps(want, sort_keys=True)}")
    return checks + 1


# ---------------------------------------------------------------------------
# Fixtures and probes
# ---------------------------------------------------------------------------

def write_pack(root: str, files: dict, ns_files: dict[str, dict] | None = None) -> str:
    """A pack directory: pack.mcmeta plus named model files under
    assets/minecraft/models/. A string value is written raw (for
    the unparseable file). `ns_files` adds extra namespaces, each a
    {path: content} mapping."""
    mdir = os.path.join(root, "assets", "minecraft", "models")
    os.makedirs(mdir, exist_ok=True)
    with open(os.path.join(root, "pack.mcmeta"), "w",
              encoding="utf-8") as fh:
        json.dump({"pack": {"pack_format": 9,
                            "description": "ref_model fixture"}}, fh)
    for name, content in files.items():
        path = os.path.join(root, "assets", "minecraft", "models",
                            name + ".json")
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, "w", encoding="utf-8") as fh:
            if isinstance(content, str):
                fh.write(content)
            else:
                json.dump(content, fh)
    for ns, mapping in (ns_files or {}).items():
        for name, content in mapping.items():
            path = os.path.join(root, "assets", ns, "models",
                                name + ".json")
            os.makedirs(os.path.dirname(path), exist_ok=True)
            with open(path, "w", encoding="utf-8") as fh:
                json.dump(content, fh)
    return root


def crafted_files() -> dict:
    files = {
        "block/base": {
            "textures": {"top": "t/base_top", "side": "t/base_side",
                         "bottom": "t/base_bottom"},
            "elements": [{"from": [0, 0, 0], "to": [16, 16, 16]}],
            "display": {"thirdperson": {"rotation": [0, 0, 0]}}},
        "block/child": {
            "parent": "block/base",
            "textures": {"top": "t/child_top", "side": "t/child_side"},
            "elements": [{"from": [1, 1, 1], "to": [2, 2, 2]}]},
        "block/noelem": {
            "parent": "block/base",
            "textures": {"top": "t/noelem_top"}},
        "block/wipe": {"parent": "block/base", "elements": []},
        "block/grand": {
            "parent": "block/noelem",
            "textures": {"side": "t/grand_side"}},
        "block/onlydisplay": {
            "display": {"gui": {"rotation": [1, 0, 0]}},
            "gui_light": "front"},
        "block/orphan": {
            "parent": "block/absent", "textures": {"a": "t/a"}},
        "block/oddvalue": {"textures": {"num": 42, "flag": True,
                                        "none": None}},
        "block/array": [1, 2],
        "block/bad": "{not json at all",
        "block/usesm": {"parent": "m:block/x", "textures": {"u": "t/u"}},
        "block/defns": {"parent": "block/leaf"},
        "block/leaf": {"textures": {"leaf": "t/leaf"}},
        # --- recorded differences: not probed for parity ---
        "block/nonstrparent": {"parent": 42, "textures": {"a": "t/a"}},
        "block/loopa": {"parent": "block/loopb"},
        "block/loopb": {"parent": "block/loopa"},
    }
    for i in range(71):
        files[f"block/deep{i}"] = (
            {"parent": f"block/deep{i + 1}"} if i < 70
            else {"textures": {"x": "t/x"}})
    return files


def crafted_probes() -> list[str]:
    return [
        "block/base",
        "block/child",
        "minecraft:block/child",  # the same model, named fully
        "block/noelem",
        "block/wipe",
        "block/grand",
        "block/onlydisplay",
        "block/orphan",
        "block/oddvalue",
        "block/array",
        "block/bad",
        "block/usesm",
        "block/defns",
        "block/leaf",
        "block/absent",                     # no file: {}
        "absent_namespace:block/base",      # no such namespace: {}
    ]


def vanilla_probes(packdir: str) -> list[str]:
    """Every model file the reference's own glob would find, by
    `namespace:path`; a sample also by bare path."""
    probes: list[str] = []
    for path in sorted(glob.glob(
            os.path.join(packdir, "assets", "*", "models", "**",
                         "*.json"), recursive=True)):
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
    tmp = tempfile.mkdtemp(prefix="mcme_ref_model_")
    try:
        # --- crafted pack: every fold quirk, decided by both readers ---
        pack = write_pack(os.path.join(tmp, "crafted"), crafted_files(),
                          ns_files={"m": {"block/x": {"textures":
                                                      {"x": "t/x"}}}})
        mgr = make_manager(pack)
        for ref in crafted_probes():
            checks = check_probe(args.mcme, pack, mgr, ref, checks,
                                 "crafted")
        _log(f"ok — crafted pack: {len(crafted_probes())} probes, "
             "parent folds and discard quirks alike on both sides")

        # --- recorded differences: each side's observable behavior ---
        # A `parent` that is not a string: the reference raises on
        # its .split; mcme reads it as no parent (D31).
        try:
            ref_fold(mgr, "block/nonstrparent")
        except AttributeError:
            pass
        else:
            raise AssertionError("ref accepted a non-string parent")
        got = mcme_fold(args.mcme, pack, "block/nonstrparent")
        if got != {"textures": {"a": "t/a"}}:
            raise AssertionError(
                f"non-string parent: mcme {got} != the recorded answer")
        checks += 1

        # A parent cycle: the reference raises RecursionError; mcme
        # refuses by name, exit 3.
        try:
            ref_fold(mgr, "block/loopa")
        except RecursionError:
            pass
        else:
            raise AssertionError("ref accepted a parent cycle")
        rc, _out, err = run(args.mcme, "model", pack, "block/loopa")
        if rc != 3:
            raise AssertionError(
                f"cycle: mcme exit {rc}, want 3 (stderr {err.strip()!r})")
        checks += 1

        # A chain deeper than mcme's cap: the reference answers (its
        # reach is Python's recursion limit); mcme refuses, exit 3.
        want = ref_fold(mgr, "block/deep0")
        if want != {"textures": {"x": "t/x"}}:
            raise AssertionError(f"deep chain: ref folded to {want}")
        rc, _out, err = run(args.mcme, "model", pack, "block/deep0")
        if rc != 3:
            raise AssertionError(
                f"deep chain: mcme exit {rc}, want 3 (stderr {err.strip()!r})")
        checks += 1
        _log("ok — recorded differences: non-string parent, cycle, "
             "depth cap — each pinned against the reference's own behavior")

        # --- the reader's own shipped pack, real game data ---
        if not os.path.isdir(vanilla_fix):
            raise AssertionError(f"missing reference pack {vanilla_fix}")
        mgr = make_manager(vanilla_fix)
        probes = vanilla_probes(vanilla_fix)
        for ref in probes:
            checks = check_probe(args.mcme, vanilla_fix, mgr, ref,
                                 checks, "vanilla_fix")
        _log(f"ok — java_vanilla_fix: {len(probes)} probes across its "
             "models, the reference's folds answered by mcme")

        # --- usage refusals ---
        rc, _out, _err = run(args.mcme, "model")
        if rc != 1:
            raise AssertionError(f"`model` alone: exit {rc}, want 1")
        checks += 1
        rc, _out, _err = run(args.mcme, "model",
                             os.path.join(tmp, "no-such-pack"),
                             "block/base")
        if rc != 2:
            raise AssertionError(f"missing pack dir: exit {rc}, want 2")
        checks += 1
        rc, _out, _err = run(args.mcme, "model", pack, ":block/base")
        if rc != 1:
            raise AssertionError(f"empty namespace: exit {rc}, want 1")
        checks += 1
        rc, _out, _err = run(args.mcme, "model", pack, "minecraft:")
        if rc != 1:
            raise AssertionError(f"empty path: exit {rc}, want 1")
        checks += 1
        _log("ok — usage refusals: 1 / 2 / 1 / 1")

        _log(f"ok — {checks} checks")
        return 0
    finally:
        import shutil
        shutil.rmtree(tmp, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())
