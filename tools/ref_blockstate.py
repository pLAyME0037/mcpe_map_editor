#!/usr/bin/env python3
"""
Cross-check: `mcme blockstate` — which model(s) a pack's blockstates
select for one block state, answered by mcme and by the reference
reader deciding the same question with its own code
(decisions.md#D30).

How the reference is observed. Its `_get_model` answers with a MESH:
it loads models, merges them, rotates them. The selection itself —
the thing mcme reports — passes through exactly one function on its
way out, `_load_blockstate_model`, once per winning variant or
matching multipart case, handed the raw apply value from the JSON.
The pin wraps that function: every value the reference selects is
recorded in order, and the wrap returns the missing mesh so no model
file is ever opened. The reference's answer for a probe is therefore
the recorded applies — list first element, model, x, y, uvlock —
plus whether nothing was recorded and `_get_model` returned the
missing mesh, which is mcme's `missing` flag. Both sides answer the
same two fields, so the comparison is the reference's own decision
code against mcme's mirror.

What is compared:

  CRAFTED PACK — variants first-match in file order; the
    missing-property default that lets a states-less block select
    the first key; the `|` second value the key regex never sees;
    a key it cannot pair matching everything; multipart OR, AND,
    pipe-splits, JSON booleans and the empty `when`, all under the
    opposite absent-property rule (absent never matches); an apply
    list contributing only its first element; a model-less apply
    answering a null model; an unparseable blockstate file both
    readers skip; and the three missing shapes (no file, no key,
    no matching variant) kept apart from multipart's empty
    selection.

  VANILLA FIX — every blockstate file in the reader's own shipped
    java_vanilla_fix pack: one probe per variant key plus an empty
    probe, and for multipart files the cartesian of the `when`
    values (capped at a per-file probe budget), against real game
    data.

  USAGE — missing arguments exit 1, a pack directory that does not
    exist exit 2, a non-string state exit 1.

Usage:
    venv/bin/python tools/ref_blockstate.py --mcme ./mcme
"""

from __future__ import annotations

import argparse
import itertools
import json
import os
import subprocess
import sys
import tempfile

# Probe budget per vanilla multipart file: the cartesian of its
# when-values, or this many at most (single-prop probes beyond it).
FILE_PROBE_CAP = 36
# Total probes across the vanilla pack.
TOTAL_PROBE_CAP = 500


def _log(msg: str) -> None:
    print(f"ref_blockstate: {msg}", file=sys.stderr)


def run(mcme: str, *args: str) -> tuple[int, str, str]:
    """Run mcme, return (exit code, stdout, stderr)."""
    proc = subprocess.run(
        [mcme, *args], capture_output=True, text=True, timeout=120)
    return proc.returncode, proc.stdout, proc.stderr


# ---------------------------------------------------------------------------
# The reference side
# ---------------------------------------------------------------------------

class RefSpy:
    """The reference's own selection, recorded.

    Wraps `_load_blockstate_model` on the manager instance so
    `_get_model` runs unmodified: each apply value it selects lands
    in `recorded`, in selection order, and no model file opens.
    """

    def __init__(self, mgr, missing):
        self.mgr = mgr
        self.missing = missing
        self.recorded: list = []
        mgr._load_blockstate_model = self._wrap

    def _wrap(self, value, **kwargs):
        self.recorded.append(value)
        return self.missing

    def answer(self, name: str, props: dict[str, str]) -> tuple[bool, list]:
        from minecraft_model_reader.api import Block
        import amulet_nbt

        self.recorded.clear()
        block = Block(
            "minecraft", name,
            {k: amulet_nbt.TAG_String(v) for k, v in props.items()})
        result = self.mgr._get_model(block)

        models = []
        for value in self.recorded:
            eff = value[0] if isinstance(value, list) else value
            models.append({
                "model": eff.get("model"),
                "x": int(eff.get("x", 0)),
                "y": int(eff.get("y", 0)),
                "uvlock": bool(eff.get("uvlock", False)),
            })
        # missing: nothing selected AND the missing mesh came back.
        # A multipart block whose cases all fail answers merge([]),
        # a new mesh — empty selection, not missing.
        missing = (result is self.missing) and not self.recorded
        return missing, models


def make_manager(packdir: str):
    """The reference manager over one pack, load and all."""
    os.environ.setdefault("CACHE_DIR", tempfile.mkdtemp(prefix="mcme_cache_"))
    from minecraft_model_reader.api.resource_pack.java import (
        JavaResourcePack, JavaResourcePackManager)

    mgr = JavaResourcePackManager([JavaResourcePack(packdir)], load=True)
    return RefSpy(mgr, mgr.missing_block)


# ---------------------------------------------------------------------------
# The mcme side
# ---------------------------------------------------------------------------

def mcme_answer(mcme: str, packdir: str, name: str,
                props: dict[str, str]) -> tuple[bool, list]:
    spec = f"minecraft:{name}"
    if props:
        inner = ",".join(f'{k}:"{v}"' for k, v in props.items())
        spec += "{" + inner + "}"
    rc, out, err = run(mcme, "blockstate", packdir, spec, "--json")
    if rc != 0:
        raise AssertionError(
            f"blockstate {spec}: exit {rc}, stderr {err.strip()!r}")
    try:
        got = json.loads(out)
    except json.JSONDecodeError:
        raise AssertionError(f"blockstate: stdout is not JSON: {out!r}")
    return got["missing"], got["models"]


def check_probe(mcme: str, packdir: str, spy: RefSpy, name: str,
                props: dict[str, str], checks: int, why: str) -> int:
    want_missing, want_models = spy.answer(name, props)
    got_missing, got_models = mcme_answer(mcme, packdir, name, props)
    probe = f"{name}{props}"

    if got_missing != want_missing:
        raise AssertionError(
            f"{probe} ({why}): missing mcme {got_missing} != "
            f"ref {want_missing}")
    if len(got_models) != len(want_models):
        raise AssertionError(
            f"{probe} ({why}): {len(got_models)} models, reference "
            f"selected {len(want_models)}: "
            f"{[m['model'] for m in want_models]}")
    for i, (got, want) in enumerate(zip(got_models, want_models)):
        for field in ("model", "x", "y", "uvlock"):
            if got[field] != want[field]:
                raise AssertionError(
                    f"{probe} ({why}): model {i} {field} "
                    f"mcme {got[field]!r} != ref {want[field]!r}")
    return checks + 1


# ---------------------------------------------------------------------------
# Fixtures and probes
# ---------------------------------------------------------------------------

def write_pack(root: str, files: dict[str, dict | str]) -> str:
    """A pack directory: pack.mcmeta plus named blockstate files.
    A string value is written raw (for the unparseable file)."""
    os.makedirs(os.path.join(root, "assets", "minecraft", "blockstates"),
                exist_ok=True)
    with open(os.path.join(root, "pack.mcmeta"), "w",
              encoding="utf-8") as fh:
        json.dump({"pack": {"pack_format": 9,
                            "description": "ref_blockstate fixture"}},
                  fh)
    for name, content in files.items():
        path = os.path.join(root, "assets", "minecraft", "blockstates",
                            name + ".json")
        with open(path, "w", encoding="utf-8") as fh:
            if isinstance(content, str):
                fh.write(content)
            else:
                json.dump(content, fh)
    return root


def crafted_files() -> dict[str, dict | str]:
    return {
        "stairs": {"variants": {
            "facing=east,half=bottom": {"model": "m:stairs_eb", "y": 90},
            "facing=north": {"model": "m:stairs_n", "uvlock": True},
            "": {"model": "m:stairs_default"}}},
        "quirk": {"variants": {
            "a=1,b=2": {"model": "m:1"}, "a=1": {"model": "m:2"}}},
        "pipe": {"variants": {
            "x=1|2": {"model": "m:p1"}, "x=2": {"model": "m:p2"}}},
        "junkkey": {"variants": {
            "X=1": {"model": "m:j1"}, "": {"model": "m:j2"}}},
        "nomatch": {"variants": {"a=1": {"model": "m:n1"}}},
        "empty": {},
        "bad": "{not json at all",
        "nomodel": {"variants": {"": {"x": 90}}},
        "listapply": {"variants": {"": [{"model": "m:L0"},
                                        {"model": "m:L1"}]}},
        "fence": {"multipart": [
            {"apply": {"model": "m:post"}},
            {"when": {"north": "true"}, "apply": {"model": "m:ns"}},
            {"when": {"OR": [{"east": "true"}, {"west": "true"}]},
             "apply": {"model": "m:ew"}},
            {"when": {"AND": [{"a": "1"}, {"b": "2"}]},
             "apply": {"model": "m:ab"}},
            {"when": {"x": "1|2"}, "apply": {"model": "m:pipe"}},
            {"when": {"lit": True}, "apply": {"model": "m:lit"}},
            {"when": {"gone": "v"}, "apply": {"model": "m:gone"}},
            {"when": {}, "apply": {"model": "m:emptywhen"}},
            {"when": {"abort": 3}, "apply": {"model": "m:abort"}},
            {"when": {"x": "1"}, "apply": {"model": "m:x1"}}]},
        "zero": {"multipart": [
            {"when": {"p": "1"}, "apply": {"model": "m:z1"}}]},
        "list": {"multipart": [
            {"when": {"q": "1"}, "apply": [{"model": "m:L0"},
                                            {"model": "m:L1"}]},
            {"apply": [{"model": "m:M0"}]}]},
    }


def crafted_probes() -> list[tuple[str, dict[str, str]]]:
    return [
        ("stairs", {"facing": "east", "half": "bottom"}),
        ("stairs", {"facing": "north"}),
        ("stairs", {"facing": "south"}),
        ("stairs", {}),
        ("quirk", {"a": "1"}),
        ("quirk", {"a": "1", "b": "3"}),
        ("quirk", {"a": "2"}),
        ("quirk", {}),
        ("pipe", {"x": "2"}),
        ("pipe", {"x": "1"}),
        ("junkkey", {"x": "0"}),
        ("nomatch", {"a": "1"}),
        ("nomatch", {"a": "9"}),
        ("empty", {}),
        ("bad", {}),
        ("nomodel", {}),
        ("listapply", {}),
        ("fence", {"north": "true", "x": "2", "lit": "true"}),
        ("fence", {}),
        ("fence", {"north": "false"}),
        ("fence", {"east": "true", "a": "1", "b": "2"}),
        ("fence", {"west": "true"}),
        ("zero", {"p": "1"}),
        ("zero", {"p": "0"}),
        ("list", {"q": "1"}),
        ("list", {}),
        ("absent_block", {}),
    ]


def vanilla_probes(packdir: str) -> list[tuple[str, dict[str, str]]]:
    """One probe per variant key plus the empty probe; for multipart,
    the cartesian of when-values under a per-file budget."""
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
                        # Budget blown: one prop at a time instead.
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
    tmp = tempfile.mkdtemp(prefix="mcme_ref_blockstate_")
    try:
        # --- crafted pack: every quirk, decided by both readers ---
        pack = write_pack(os.path.join(tmp, "crafted"), crafted_files())
        spy = make_manager(pack)
        for name, props in crafted_probes():
            checks = check_probe(args.mcme, pack, spy, name, props,
                                 checks, "crafted")
        _log(f"ok — crafted pack: {len(crafted_probes())} probes, "
             "variants and multipart quirks alike on both sides")

        # --- the reader's own shipped pack, real game data ---
        if not os.path.isdir(vanilla_fix):
            raise AssertionError(f"missing reference pack {vanilla_fix}")
        spy = make_manager(vanilla_fix)
        probes = vanilla_probes(vanilla_fix)
        if len(probes) > TOTAL_PROBE_CAP:
            step = len(probes) / TOTAL_PROBE_CAP
            probes = [probes[int(i * step)] for i in range(TOTAL_PROBE_CAP)]
        for name, props in probes:
            checks = check_probe(args.mcme, vanilla_fix, spy, name, props,
                                 checks, "vanilla_fix")
        _log(f"ok — java_vanilla_fix: {len(probes)} probes across its "
             "blockstates, the reference's selections answered by mcme")

        # --- usage refusals ---
        rc, _out, _err = run(args.mcme, "blockstate")
        if rc != 1:
            raise AssertionError(f"`blockstate` alone: exit {rc}, want 1")
        checks += 1
        rc, _out, _err = run(args.mcme, "blockstate",
                             os.path.join(tmp, "no-such-pack"),
                             "minecraft:stone")
        if rc != 2:
            raise AssertionError(
                f"missing pack dir: exit {rc}, want 2")
        checks += 1
        rc, _out, _err = run(args.mcme, "blockstate", pack,
                             "minecraft:stairs{x:1}")
        if rc != 1:
            raise AssertionError(
                f"non-string state: exit {rc}, want 1")
        checks += 1
        _log("ok — usage refusals: 1 / 2 / 1")

        _log(f"ok — {checks} checks")
        return 0
    finally:
        import shutil
        shutil.rmtree(tmp, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())
