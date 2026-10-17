#!/usr/bin/env python3
"""
Cross-check: `mcme pack info` — a Java resource pack's manifest read
by mcme equals what the reference reader makes of the same directory
(decisions.md#D29).

The C selftest pins the library against fixtures it writes itself.
This tool proves the CLI against the reference the project translates
with: minecraft_model_reader's `JavaResourcePack`, the reader the
original Python used for packs, models and the atlas. Its public
answer for a directory is `valid_pack`, `pack_format`,
`pack_description` and whether `pack.png` sits beside the manifest.

What is compared:

  VALID PACKS — a crafted pack with format, description and pack.png;
                a pack with no description (the reference defaults it
                to the empty string, and so must mcme) and no icon;
                and the reader's own shipped java_vanilla_fix pack
                (pack_format 4, its hardcoded description). mcme's
                --json fields must equal the reference's attributes
                exactly, and both must call the directory a pack.

  REFUSALS    — a directory with no pack.mcmeta, a manifest that is
                not JSON, a manifest with no `pack` object, and one
                without pack.pack_format: the reference marks each
                not-a-pack (`valid_pack` False) and mcme must refuse
                each with exit 2 and print nothing on stdout.

  USAGE       — `pack` alone and an unknown pack subcommand exit 1.

One recorded difference: the reference marks a pack valid whenever
`pack_format` is PRESENT, whatever its type, and keeps the raw JSON
value; mcme requires the integer the game actually writes and refuses
any other type. Real packs are integers, so the pin's fixtures stay in
the shared domain and the strictness is logged in decisions.md D29.

Usage:
    venv/bin/python tools/ref_pack.py --mcme ./mcme
"""

from __future__ import annotations

import argparse
import json
import os
import subprocess
import sys
import tempfile


def _log(msg: str) -> None:
    print(f"ref_pack: {msg}", file=sys.stderr)


def run(mcme: str, *args: str) -> tuple[int, str, str]:
    """Run mcme, return (exit code, stdout, stderr)."""
    proc = subprocess.run(
        [mcme, *args], capture_output=True, text=True, timeout=120)
    return proc.returncode, proc.stdout, proc.stderr


def write_pack(root: str, manifest: str | None, icon: bool = False) -> str:
    """A directory that may hold pack.mcmeta and pack.png."""
    os.makedirs(root, exist_ok=True)
    if manifest is not None:
        with open(os.path.join(root, "pack.mcmeta"), "w",
                  encoding="utf-8") as fh:
            fh.write(manifest)
    if icon:
        with open(os.path.join(root, "pack.png"), "wb") as fh:
            fh.write(b"\x89PNG\r\n\x1a\n")
    return root


def ref_pack(path: str):
    """The reference's own reading of the directory."""
    from minecraft_model_reader.api.resource_pack.java import JavaResourcePack
    return JavaResourcePack(path)


def mcme_json(mcme: str, path: str) -> dict:
    rc, out, err = run(mcme, "pack", "info", path, "--json")
    if rc != 0:
        raise AssertionError(
            f"pack info {path}: exit {rc}, stderr {err.strip()!r}")
    try:
        return json.loads(out)
    except json.JSONDecodeError:
        raise AssertionError(f"pack info: stdout is not JSON: {out!r}")


def check_valid(mcme: str, root: str, checks: int) -> int:
    """One valid pack: mcme --json equals the reference's attributes."""
    got = mcme_json(mcme, root)
    ref = ref_pack(root)
    if not ref.valid_pack:
        raise AssertionError(f"{root}: the reference says not a pack")
    icon_isfile = os.path.isfile(os.path.join(root, "pack.png"))
    if got["format"] != ref.pack_format:
        raise AssertionError(
            f"{root}: format mcme {got['format']} != ref {ref.pack_format}")
    if got["description"] != ref.pack_description:
        raise AssertionError(
            f"{root}: description mcme {got['description']!r} != "
            f"ref {ref.pack_description!r}")
    if got["has_icon"] != icon_isfile:
        raise AssertionError(
            f"{root}: has_icon mcme {got['has_icon']} != {icon_isfile}")
    if got["dir"] != root:
        raise AssertionError(f"{root}: echoed dir {got['dir']!r}")
    return checks + 1


def check_refusal(mcme: str, root: str, checks: int, why: str) -> int:
    """One not-a-pack: the reference says invalid, mcme exits 2 silent."""
    ref = ref_pack(root)
    if ref.valid_pack:
        raise AssertionError(f"{root}: the reference says it IS a pack "
                             f"({why}); the fixture is wrong")
    rc, out, err = run(mcme, "pack", "info", root, "--json")
    if rc != 2:
        raise AssertionError(
            f"{root}: {why} — exit {rc}, want 2; stderr {err.strip()!r}")
    if out != "":
        raise AssertionError(f"{root}: refused but printed {out!r}")
    return checks + 1


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--mcme", required=True)
    args = ap.parse_args()

    import minecraft_model_reader
    vanilla_fix = os.path.join(
        os.path.dirname(minecraft_model_reader.__file__),
        "api", "resource_pack", "java", "java_vanilla_fix")

    checks = 0
    tmp = tempfile.mkdtemp(prefix="mcme_ref_pack_")
    try:
        # A pack with everything: format, description, icon.
        good = write_pack(
            os.path.join(tmp, "good"),
            json.dumps({"pack": {"pack_format": 9,
                                 "description": "A study pack"}}),
            icon=True)
        checks = check_valid(args.mcme, good, checks)
        _log("ok — format, description and pack.png equal the reference")

        # No description: the reference defaults to ""; no icon file.
        nodesc = write_pack(
            os.path.join(tmp, "nodesc"),
            json.dumps({"pack": {"pack_format": 4}}))
        checks = check_valid(args.mcme, nodesc, checks)
        _log("ok — a manifest without a description answers the empty "
             "string, as the reference does")

        # The reader's own shipped pack, real pack.mcmeta and pack.png.
        if not os.path.isdir(vanilla_fix):
            raise AssertionError(f"missing reference pack {vanilla_fix}")
        checks = check_valid(args.mcme, vanilla_fix, checks)
        _log("ok — java_vanilla_fix: format and description equal the "
             "reference's own reading")

        # The four not-a-packs the reader refuses.
        nomanifest = os.path.join(tmp, "nomanifest")
        os.makedirs(nomanifest, exist_ok=True)
        checks = check_refusal(args.mcme, nomanifest, checks,
                               "no pack.mcmeta")

        notjson = write_pack(os.path.join(tmp, "notjson"), "no braces {")
        checks = check_refusal(args.mcme, notjson, checks,
                               "manifest is not JSON")

        nopack = write_pack(os.path.join(tmp, "nopack"),
                            json.dumps({"nope": True}))
        checks = check_refusal(args.mcme, nopack, checks,
                               "no pack object")

        noformat = write_pack(
            os.path.join(tmp, "noformat"),
            json.dumps({"pack": {"description": "format-less"}}))
        checks = check_refusal(args.mcme, noformat, checks,
                               "no pack_format")
        _log("ok — the four refusals the reference makes, exit 2 and "
             "silent")

        # Usage: `pack` alone, and an unknown subcommand, exit 1.
        rc, _out, _err = run(args.mcme, "pack")
        if rc != 1:
            raise AssertionError(f"`pack` alone: exit {rc}, want 1")
        checks += 1
        rc, _out, _err = run(args.mcme, "pack", "junk", "x")
        if rc != 1:
            raise AssertionError(f"`pack junk`: exit {rc}, want 1")
        checks += 1
        _log("ok — usage refusals exit 1")

        _log(f"ok — {checks} checks")
        return 0
    finally:
        import shutil
        shutil.rmtree(tmp, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())
