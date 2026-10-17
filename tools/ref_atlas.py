#!/usr/bin/env python3
"""
Cross-check: `mcme atlas` — every texture a pack ships on one
square page, answered by mcme and by the editor's own atlas module
deciding the same question (decisions.md#D34).

How the reference is observed. The atlas is not in the model
reader — it is the editor's OpenGL layer's textureatlas module,
whose `create_atlas` takes a tuple of texture paths, loads every
frame whole, sorts them by perimeter (2w + 2h) non-increasing and
STABLY, packs them into a square page with a binary-split region
tree (the first free region that holds a frame takes it at its
top-left and splits into the strip below and the strip right),
doubling the page until everything fits, and answers the page
image plus each texture's bounds — (x/size, y/size, (x+w)/size,
(y + min(h, w))/size), the top edge using the smaller of the
frame's sides. That module's package chain imports wx (needs a
display we do not have), so the pin loads the file itself by path
and runs the reference's own code verbatim.

Both sides are handed the SAME input: the manager's own shape —
its missing_no sentinel seeded FIRST (the file lives inside the
reader's package, not the pack; mcme packs its own copy of the
same image under the word "missing_no", D35), then the pack's
texture paths sorted. The sentinel's place is fixed by the seed;
the rest is sorted because the layout rides on the input order
wherever the perimeter sort ties (a pack of 16x16 textures is one
long tie) and the filesystem's directory order must not decide
the answer. The pin canonicalises the reference's sentinel path
to the word "missing_no"; every other key compares exactly —
same paths, both sides.

What is compared:

  CRAFTED PACKS — six packs, each one behaviour: three 2x2s (the
    perimeter tie keeps the sorted input; one of them is an RGB
    image, which must expand to the same opaque RGBA as Pillow's
    paste); a 4x2 among 2x2s (the perimeter sort beats the input
    order); a lone 2x4 (the bounds' min(h, w) top edge); 2x2,
    2x2, 3x2 (the first page estimate fails and doubles); all six
    images together (the tall/wide tie and a mixed layout); and
    the empty pack (the missing image alone: the app's page is
    never empty, the manager seeding its table, so the empty pack
    answers the sentinel's own 16x16 rectangle).

  VANILLA FIX — the reader's own shipped pack as it ships: 22
    real images — 64x64 banners tying on perimeter, 16x16 blocks,
    and one PALETTE image whose expansion Pillow and stb must
    agree on pixel for pixel.

  BROKEN IMAGE — a pack whose only texture is not a PNG: the
    reference never even builds the atlas — its manager's load
    opens every texture image for the transparency flags and
    Pillow refuses to identify the file (UnidentifiedImageError,
    propagated), while mcme's index keeps the file opaque with a
    warning (D32's record) and the atlas itself refuses by name,
    exit 6. Both sides answer nothing; each is pinned to its own
    refusal.

  HUMAN AND USAGE — the summary line a person reads; missing
    arguments exit 1, a pack directory that does not exist exit
    2, --out without a path exit 1.

Usage:
    venv/bin/python tools/ref_atlas.py --mcme ./mcme
"""

from __future__ import annotations

import argparse
import importlib.util
import json
import math
import os
import shutil
import subprocess
import sys
import tempfile


def _log(msg: str) -> None:
    print(f"ref_atlas: {msg}", file=sys.stderr)


def run(mcme: str, *args: str) -> tuple[int, str, str]:
    """Run mcme, return (exit code, stdout, stderr)."""
    proc = subprocess.run(
        [mcme, *args], capture_output=True, text=True, timeout=120)
    return proc.returncode, proc.stdout, proc.stderr


# ---------------------------------------------------------------------------
# The reference side
# ---------------------------------------------------------------------------

def load_textureatlas():
    """The editor's textureatlas module, loaded by file path: the
    package chain imports wx (which needs a display we do not
    have), the file itself needs only Pillow — the reference's own
    code, verbatim."""
    spec = importlib.util.find_spec("amulet_map_editor")
    if spec is None or not spec.submodule_search_locations:
        raise AssertionError("amulet_map_editor is not installed")
    path = os.path.join(spec.submodule_search_locations[0],
                        "api", "opengl", "textureatlas.py")
    if not os.path.isfile(path):
        raise AssertionError(f"no textureatlas module at {path}")
    mod_spec = importlib.util.spec_from_file_location(
        "mcme_ref_textureatlas", path)
    mod = importlib.util.module_from_spec(mod_spec)
    mod_spec.loader.exec_module(mod)
    return mod


def make_manager(packdir: str):
    """The reference manager over one pack, load and all."""
    os.environ.setdefault("CACHE_DIR", tempfile.mkdtemp(prefix="mcme_cache_"))
    from minecraft_model_reader.api.resource_pack.java import (
        JavaResourcePack, JavaResourcePackManager)

    return JavaResourcePackManager([JavaResourcePack(packdir)], load=True)


def ref_input(mgr) -> tuple[str, ...]:
    """The input both sides are handed: the manager's own table
    shape — its missing_no sentinel seeded FIRST (asserted, so the
    seed can not silently stop working), then the pack's textures
    sorted. D34 stripped the sentinel while mcme had no image of
    its own; D35 gives it one and the strip is superseded."""
    from minecraft_model_reader.api.image import missing_no_path

    if missing_no_path not in mgr.textures:
        raise AssertionError("the manager's table has no missing_no sentinel")
    paths = [p for p in mgr.textures if p != missing_no_path]
    return (missing_no_path, *sorted(paths))


def norm_name(path: str) -> str:
    """The reference's sentinel path as the word both mcme and the
    pins use for it (D32's canonicalisation)."""
    from minecraft_model_reader.api.image import missing_no_path

    return "missing_no" if path == missing_no_path else path


def first_pixel_diff(a: bytes, b: bytes, size: int) -> str:
    for i in range(0, min(len(a), len(b)), 4):
        if a[i:i + 4] != b[i:i + 4]:
            px = i // 4
            return (f"pixel {px} at ({px % size},{px // size}): reference "
                    f"{tuple(a[i:i + 4])}, mcme {tuple(b[i:i + 4])}")
    return f"pixel counts differ: {len(a) // 4} reference, {len(b) // 4} mcme"


def check_pack(mcme: str, ref, packdir: str, tag: str, outdir: str,
               checks: int) -> int:
    """Build the reference atlas over the pack's sorted texture
    paths and mcme's over the same pack, and compare: the name
    order (both sides' input order), the page size, every bound's
    value, and every pixel of the page."""
    mgr = make_manager(packdir)
    paths = ref_input(mgr)
    ref_img, ref_bounds = ref.create_atlas(paths)

    out_png = os.path.join(outdir, f"atlas_{tag}.png")
    rc, out, err = run(mcme, "atlas", packdir, "--json", "--out", out_png)
    if rc != 0:
        raise AssertionError(f"{tag}: mcme exit {rc}: {err.strip()}")
    doc = json.loads(out)

    # The names: mcme's JSON order and the reference's dict order
    # are both the input order — the tie-break both sides keep.
    # The reference's sentinel key canonicalises to the word
    # mcme names its own copy with.
    want = [norm_name(p) for p in paths]
    if list(doc["textures"]) != want:
        raise AssertionError(
            f"{tag}: mcme names {list(doc['textures'])}, want {want}")
    if [norm_name(p) for p in ref_bounds] != want:
        raise AssertionError(
            f"{tag}: reference dict order {list(ref_bounds)}, want {want}")
    checks += 1

    # The page: square, and the same side.
    if ref_img.size[0] != ref_img.size[1]:
        raise AssertionError(f"{tag}: reference page {ref_img.size} "
                             "is not square")
    if doc["size"] != ref_img.size[0]:
        raise AssertionError(f"{tag}: mcme page {doc['size']}, "
                             f"reference {ref_img.size[0]}")
    checks += 1

    # The bounds: the keys were checked above as one order; 1e-9
    # on the values.
    for raw, name in zip(paths, want):
        got = doc["textures"][name]
        ref_b = list(ref_bounds[raw])
        for i in range(4):
            if not math.isclose(got[i], ref_b[i], rel_tol=1e-9,
                                abs_tol=1e-9):
                raise AssertionError(f"{tag}: {name} bounds {got}, "
                                     f"want {ref_b}")
    checks += len(want)

    # The page's pixels, decoded: stb's PNG and Pillow's own
    # answer must carry the same RGBA bytes.
    from PIL import Image
    with Image.open(out_png) as im:
        if im.mode != "RGBA":
            raise AssertionError(f"{tag}: mcme page mode {im.mode}")
        if im.size != ref_img.size:
            raise AssertionError(f"{tag}: mcme page {im.size}, "
                                 f"reference {ref_img.size}")
        page = im.tobytes()
    ref_page = ref_img.tobytes()
    if page != ref_page:
        raise AssertionError(f"{tag}: page differs — "
                             f"{first_pixel_diff(ref_page, page, doc['size'])}")
    checks += 1
    return checks


# ---------------------------------------------------------------------------
# The crafted packs
# ---------------------------------------------------------------------------

RED = (255, 0, 0, 255)
GREEN = (0, 255, 0, 255)
YELLOW = (255, 255, 0, 255)
MAGENTA = (255, 0, 255, 255)
CYAN = (0, 255, 255, 255)

CRAFTED = {
    # Three 2x2s: one perimeter tie decided by the sorted input,
    # and one RGB image whose expansion must match Pillow's.
    "tri": {
        "a.png": ("RGBA", (2, 2), RED),
        "b.png": ("RGBA", (2, 2), GREEN),
        "c.png": ("RGB", (2, 2), (0, 0, 255)),
    },
    # The 4x2's perimeter (12) beats the two 2x2s (8) regardless
    # of where the input put it.
    "mix": {
        "a.png": ("RGBA", (2, 2), RED),
        "wide.png": ("RGBA", (4, 2), YELLOW),
        "b.png": ("RGBA", (2, 2), GREEN),
    },
    # A lone 2x4: the bounds' top edge is min(h, w), not h.
    "tall": {
        "tall.png": ("RGBA", (2, 4), MAGENTA),
    },
    # The area estimate does not fit these three at size 4; the
    # page doubles to 8 and takes them.
    "double": {
        "a.png": ("RGBA", (2, 2), RED),
        "b.png": ("RGBA", (2, 2), GREEN),
        "s3.png": ("RGBA", (3, 2), CYAN),
    },
    # Everything together: tall and wide tie at 12 and the tie
    # keeps the sorted input (tall.png sorts first), then s3,
    # then the three 8s.
    "multi": {
        "a.png": ("RGBA", (2, 2), RED),
        "b.png": ("RGBA", (2, 2), GREEN),
        "c.png": ("RGB", (2, 2), (0, 0, 255)),
        "s3.png": ("RGBA", (3, 2), CYAN),
        "tall.png": ("RGBA", (2, 4), MAGENTA),
        "wide.png": ("RGBA", (4, 2), YELLOW),
    },
    # The manifest loads assets, the textures directory is empty:
    # both sides answer the missing image's own page.
    "empty": {},
}


def write_pack(root: str, name: str, images: dict) -> str:
    """A pack with a valid manifest — both sides gate asset
    loading on it — and these images under textures/block/."""
    from PIL import Image

    pack = os.path.join(root, name)
    texdir = os.path.join(pack, "assets", "minecraft", "textures", "block")
    os.makedirs(texdir, exist_ok=True)
    with open(os.path.join(pack, "pack.mcmeta"), "w") as f:
        json.dump({"pack": {"pack_format": 4,
                            "description": "atlas probes"}}, f)
    for fname, (mode, size, color) in images.items():
        Image.new(mode, size, color).save(os.path.join(texdir, fname))
    return pack


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--mcme", required=True)
    args = ap.parse_args()

    ref = load_textureatlas()
    import minecraft_model_reader

    checks = 0
    tmp = tempfile.mkdtemp(prefix="mcme_ref_atlas_")
    try:
        # --- crafted packs, one behaviour each ---
        for tag, images in CRAFTED.items():
            pack = write_pack(tmp, f"pack_{tag}", images)
            checks = check_pack(args.mcme, ref, pack, tag, tmp, checks)
        _log(f"ok — crafted packs: {len(CRAFTED)} layouts answered "
             "alike — the perimeter sort and its stable ties, the "
             "min(h, w) top edge, the doubling page, the mixed pack "
             "and the empty one's missing-image page")

        # --- the reader's own shipped pack, real game data ---
        vanilla_fix = os.path.join(
            os.path.dirname(minecraft_model_reader.__file__),
            "api", "resource_pack", "java", "java_vanilla_fix")
        if not os.path.isdir(vanilla_fix):
            raise AssertionError(f"missing reference pack {vanilla_fix}")
        checks = check_pack(args.mcme, ref, vanilla_fix, "vanilla", tmp,
                            checks)
        _log("ok — java_vanilla_fix: 22 real images atlased alike — "
             "the 64x64 banner ties on perimeter, the 16x16 blocks, "
             "and the one palette image expanding the same in Pillow "
             "and stb")

        # --- an image neither side can read: the reference raises
        # before answering, mcme exits 6 naming the file ---
        broken = write_pack(tmp, "pack_broken", {})
        with open(os.path.join(broken, "assets", "minecraft",
                               "textures", "block", "broken.png"),
                  "w") as f:
            f.write("this is not a png")
        ref_died = False
        try:
            mgr = make_manager(broken)
            ref.create_atlas(ref_input(mgr))
        except Exception:
            ref_died = True
        if not ref_died:
            raise AssertionError(
                "broken pack: the reference ANSWERED — its manager's "
                "transparency read was expected to raise on the file")
        checks += 1
        rc, _out, err = run(args.mcme, "atlas", broken)
        if rc != 6:
            raise AssertionError(f"broken pack: mcme exit {rc}, want 6")
        if "broken.png" not in err:
            raise AssertionError(
                f"broken pack: mcme's refusal does not name the file: "
                f"{err.strip()!r}")
        checks += 2
        _log("ok — broken image: the reference dies at manager load "
             "reading the file (UnidentifiedImageError), mcme's index "
             "keeps it and the atlas exits 6 naming it")

        # --- the human summary ---
        rc, out, err = run(args.mcme, "atlas",
                           os.path.join(tmp, "pack_tri"))
        if rc != 0:
            raise AssertionError(f"human mode: exit {rc}: {err.strip()}")
        if "4 textures on a 32x32 page" not in out:
            raise AssertionError(f"human output {out!r}")
        checks += 1
        _log("ok — human output: '4 textures on a 32x32 page' — the "
             "three pack textures and the missing image on the page "
             "they grew")

        # --- usage refusals ---
        rc, _out, _err = run(args.mcme, "atlas")
        if rc != 1:
            raise AssertionError(f"`atlas` alone: exit {rc}, want 1")
        checks += 1
        rc, _out, _err = run(args.mcme, "atlas",
                             os.path.join(tmp, "no-such-pack"))
        if rc != 2:
            raise AssertionError(f"missing pack dir: exit {rc}, want 2")
        checks += 1
        rc, _out, _err = run(args.mcme, "atlas",
                             os.path.join(tmp, "pack_tri"), "--out")
        if rc != 1:
            raise AssertionError(f"`--out` without a path: exit {rc}, want 1")
        checks += 1
        rc, _out, _err = run(args.mcme, "atlas",
                             os.path.join(tmp, "pack_tri"), "extra")
        if rc != 1:
            raise AssertionError(f"extra argument: exit {rc}, want 1")
        checks += 1
        _log("ok — usage refusals: 1 / 2 / 1 / 1")

        _log(f"ok — {checks} checks")
        return 0
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())
