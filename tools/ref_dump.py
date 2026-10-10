#!/usr/bin/env python3
"""
Golden NBT reference-data generator (amulet_nbt 2.1.8).

Must run under the Amulet venv python so `import amulet_nbt` resolves:
    /home/playme/Downloads/Amulet-Map-Editor-0.10.64/venv/bin/python tools/ref_dump.py
If run with another interpreter, the script re-execs itself under that venv python.

For each test document <name> it writes into tests/golden/nbt/:

    <name>.be.bin    raw (uncompressed) big-endian NBT, incl. root tag name
    <name>.le.bin    raw (uncompressed) little-endian NBT, incl. root tag name
    <name>.snbt      SNBT text of the root tag (tag.to_snbt(), indent=None),
                     UTF-8 encoded, written in binary mode (these documents
                     contain raw NUL / newline / tab / CR bytes by design)
    <name>.root.txt  the root tag name as plain UTF-8 text (may be 0 bytes)

API notes (verified against the installed package, see report):
  * NBTFile is a deprecated alias for NamedTag  -> NamedTag(root_tag, "Root")
  * byte order is a `little_endian: bool` kwarg on load()/to_nbt()/save_to(),
    there is NO BigEndian/LittleEndian/ByteOrder type.
  * SNBT parse: amulet_nbt.from_snbt(text) -> bare tag (no name)
    SNBT dump : tag.to_snbt(indent=None, indent_chr=None) / NamedTag.to_snbt()
  * Python values are NOT auto-wrapped: CompoundTag["k"] = StringTag("v").
  * Binary output preserves compound insertion order; SNBT output re-orders
    keys with a case-insensitive comparator (see report).  Both are dumped
    verbatim here, so nothing depends on that ordering in this script.
"""

import io
import math
import os
import struct
import sys

OUT_DIR = os.path.normpath(os.path.join(
    os.path.dirname(os.path.abspath(__file__)),
    "..", "tests", "golden", "nbt"))

VENV_PY = "/home/playme/Downloads/Amulet-Map-Editor-0.10.64/venv/bin/python"


def _ensure_amulet():
    global an
    try:
        import amulet_nbt as an
        return
    except ImportError:
        pass
    if os.path.exists(VENV_PY) and os.path.realpath(sys.executable) != os.path.realpath(VENV_PY):
        os.execv(VENV_PY, [VENV_PY] + sys.argv)
    sys.stderr.write("amulet_nbt not importable and venv python missing: %s\n" % VENV_PY)
    raise SystemExit(1)


_ensure_amulet()

# Silence the NBTFile deprecation machinery (we use NamedTag directly).
import warnings
warnings.simplefilter("ignore")


# ---------------------------------------------------------------- helpers ---

def cps(s):
    """Space separated code points of a python str."""
    return " ".join("U+%04X" % ord(c) for c in s)


def hexdump(data, limit=None):
    """Classic `hexdump -C` style dump of bytes."""
    if limit is not None:
        data = data[:limit]
    lines = []
    for off in range(0, len(data), 16):
        chunk = data[off:off + 16]
        hexpart = " ".join("%02x" % b for b in chunk)
        # pad to 39 chars (16 * 2 + 1 group space)
        hexpart = "%-39s" % hexpart
        hexpart = hexpart[:23] + " " + hexpart[23:]
        ascii_ = "".join(chr(b) if 0x20 <= b < 0x7F else "." for b in chunk)
        lines.append("%08x  %s  |%s|" % (off, hexpart, ascii_))
    if limit is not None and limit < len(data):
        lines.append("%08x  ... (%d more bytes)" % (limit, len(data) - limit))
    return "\n".join(lines)


def f32_bits(x):
    """Raw bits of the float32 that FloatTag(x) stores."""
    return struct.unpack("<I", struct.pack("<f", x))[0]


def deep_mismatches(a, b, path="$", out=None):
    """Structural comparison of two tags, insensitive to compound key order.
    Returns a list of mismatch descriptions (empty == equivalent)."""
    if out is None:
        out = []
    if type(a) is not type(b):
        out.append("%s: type %s vs %s" % (path, type(a).__name__, type(b).__name__))
        return out
    if isinstance(a, an.CompoundTag):
        ka, kb = sorted(a.keys()), sorted(b.keys())
        if ka != kb:
            out.append("%s: keys %r vs %r" % (path, ka, kb))
            return out
        for k in ka:
            deep_mismatches(a[k], b[k], "%s.%s" % (path, k), out)
        return out
    if isinstance(a, an.ListTag):
        if a.list_data_type != b.list_data_type:
            out.append("%s: list_data_type %d vs %d" % (path, a.list_data_type, b.list_data_type))
        if len(a) != len(b):
            out.append("%s: length %d vs %d" % (path, len(a), len(b)))
            return out
        for i, (x, y) in enumerate(zip(a, b)):
            deep_mismatches(x, y, "%s[%d]" % (path, i), out)
        return out
    if isinstance(a, an.StringTag):
        if a.value != b.value:
            out.append("%s: string %r vs %r" % (path, a.value, b.value))
        return out
    if isinstance(a, (an.FloatTag, an.DoubleTag)):
        fmt = "<f" if isinstance(a, an.FloatTag) else "<d"
        if struct.pack(fmt, a.value) != struct.pack(fmt, b.value):
            out.append("%s: float %r vs %r (bit difference)"
                       % (path, a.value, b.value))
        return out
    if isinstance(a, (an.ByteTag, an.ShortTag, an.IntTag, an.LongTag)):
        if a.value != b.value:
            out.append("%s: int %r vs %r" % (path, a.value, b.value))
        return out
    # arrays
    if a.value.tolist() != b.value.tolist():
        out.append("%s: array %r vs %r" % (path, a.value.tolist(), b.value.tolist()))
    return out


# amulet_nbt's own SNBT serialiser emits 'inff'/'-inff'/'nanf' for non-finite
# floats, but its parser does NOT recognise those tokens: from_snbt('inff')
# yields StringTag('inff').  Those keys can never round-trip through SNBT.
KNOWN_SNBTPARSE_LIMITATION_KEYS = frozenset({"pos_inf", "neg_inf", "nan"})


def amulet_sort_key(k):
    """The ordering amulet_nbt uses for compound keys in to_snbt():
    character-wise case-insensitive; at the first position where the folded
    characters are equal but the actual characters differ, lowercase < uppercase."""
    return tuple((ch.lower(), ch.isupper()) for ch in k)


def reorder_by_template(tag, template):
    """Rebuild `tag` using `template`'s compound key order (recursively).
    Both must contain the same keys/lengths (guaranteed by deep_mismatches)."""
    if isinstance(tag, an.CompoundTag) and isinstance(template, an.CompoundTag):
        c = an.CompoundTag()
        for k in template.keys():
            c[k] = reorder_by_template(tag[k], template[k])
        return c
    if isinstance(tag, an.ListTag) and isinstance(template, an.ListTag):
        l = an.ListTag()
        for x, y in zip(tag, template):
            l.append(reorder_by_template(x, y))
        return l
    return tag


def _read_snbt_string(s, i):
    """i indexes the opening quote; returns (content, index_after_closing_quote)."""
    n = len(s)
    j = i + 1
    buf = []
    while j < n:
        c = s[j]
        if c == "\\" and j + 1 < n:
            buf.append(s[j + 1])
            j += 2
            continue
        if c == '"':
            break
        buf.append(c)
        j += 1
    return "".join(buf), j + 1


def _skip_snbt_value(s, i):
    """i indexes the start of a value; returns index just past it."""
    n = len(s)
    if i >= n:
        return i
    if s[i] == '"':
        return _read_snbt_string(s, i)[1]
    if s[i] in "{[":
        depth = 0
        while i < n:
            c = s[i]
            if c == '"':
                i = _read_snbt_string(s, i)[1]
                continue
            if c in "{[":
                depth += 1
            elif c in "}]":
                depth -= 1
                if depth == 0:
                    return i + 1
            i += 1
        return i
    while i < n and s[i] not in ",}":
        i += 1
    return i


def snbt_top_keys(snbt):
    """Key order emitted by a compact (indent=None) CompoundTag.to_snbt()."""
    keys = []
    if not snbt.startswith("{"):
        return keys
    i, n = 1, len(snbt)
    while i < n:
        while i < n and snbt[i] in " \t\r\n,":
            i += 1
        if i >= n or snbt[i] == "}":
            break
        if snbt[i] == '"':
            key, i = _read_snbt_string(snbt, i)
        else:
            j = i
            while j < n and snbt[j] != ":":
                j += 1
            key = snbt[i:j].strip()
            i = j
        if i < n and snbt[i] == ":":
            i += 1
        while i < n and snbt[i] == " ":
            i += 1
        keys.append(key)
        i = _skip_snbt_value(snbt, i)
    return keys


# ------------------------------------------------------- test documents ----
# Each entry: name -> (NamedTag, note)

DOCS = {}


def add(name, tag, root_name="Root", note=""):
    DOCS[name] = (an.NamedTag(tag, root_name), note)


# 1. empty compound ----------------------------------------------------------
add("empty_compound", an.CompoundTag())

# 2. scalars -----------------------------------------------------------------
c = an.CompoundTag()
c["b"] = an.ByteTag(1)
c["s"] = an.ShortTag(2)
c["i"] = an.IntTag(3)
c["l"] = an.LongTag(4)
c["f"] = an.FloatTag(5.5)
c["d"] = an.DoubleTag(6.25)
c["bmin"] = an.ByteTag(-128)
c["bmax"] = an.ByteTag(127)
c["smin"] = an.ShortTag(-32768)
c["smax"] = an.ShortTag(32767)
c["imin"] = an.IntTag(-2147483648)
c["imax"] = an.IntTag(2147483647)
c["lmin"] = an.LongTag(-9223372036854775808)
c["lmax"] = an.LongTag(9223372036854775807)
add("scalars", c)

# 3. floats ------------------------------------------------------------------
FLOAT_CASES = [
    ("pos_inf", math.inf),
    ("neg_inf", -math.inf),
    ("nan", math.nan),
    ("zero", 0.0),
    ("negzero", -0.0),
    ("tiny", 1e-38),
    ("big", 3.4028235e38),
]
c = an.CompoundTag()
for k, v in FLOAT_CASES:
    c[k] = an.FloatTag(v)
add("floats", c)

# 4. arrays ------------------------------------------------------------------
c = an.CompoundTag()
c["ba"] = an.ByteArrayTag([0, 1, -1, 127, -128])
c["ia"] = an.IntArrayTag([0, 1, -1, 2147483647, -2147483648])
c["la"] = an.LongArrayTag([0, 1, -1, 9223372036854775807, -9223372036854775808])
c["empty_ba"] = an.ByteArrayTag([])
c["empty_ia"] = an.IntArrayTag([])
c["empty_la"] = an.LongArrayTag([])
add("arrays", c)

# 5. strings -----------------------------------------------------------------
STRING_CASES = [
    ("ascii", "hello"),
    ("empty", ""),
    ("spaces", "  leading and trailing  "),
    ("quote", 'say "hi"'),
    ("backslash", "a\\b"),
    ("newline", "line1\nline2\ttabbed"),   # real U+000A and real U+0009
    ("bmp", "café 東京"),
    ("emoji", "\U0001F600\U0001F1EC\U0001F1E7"),  # U+1F600 U+1F1EC U+1F1E7
    ("nul", "a\x00b"),                     # U+0000 in the middle
    ("mixed", "aé\U0001F600b"),
]
c = an.CompoundTag()
for k, v in STRING_CASES:
    c[k] = an.StringTag(v)
add("strings", c)

# 6. lists -------------------------------------------------------------------
c = an.CompoundTag()
c["intlist"] = an.ListTag([an.IntTag(1), an.IntTag(2), an.IntTag(3)])
c["emptylist"] = an.ListTag()
c["strlist"] = an.ListTag([an.StringTag("a"), an.StringTag("b")])
c["nestedlist"] = an.ListTag([
    an.ListTag([an.IntTag(1), an.IntTag(2)]),
    an.ListTag([an.IntTag(3)]),
])
add("lists", c,
    note="emptylist is ListTag(): element type byte written is 0x01 (TAG_Byte), "
         "NOT the vanilla-canonical 0x00 (TAG_End); a 0x00 read back from disk "
         "is preserved as 0x00 and re-serialized as 0x00")

# 7. compound list -----------------------------------------------------------
c = an.CompoundTag()
lst = an.ListTag()
for cid, v in (("a", 1), ("b", 2)):
    e = an.CompoundTag()
    e["id"] = an.StringTag(cid)
    e["v"] = an.IntTag(v)
    lst.append(e)
c["list"] = lst
c["empty_list_of_compounds"] = an.ListTag()
add("compound_list", c,
    note="empty_list_of_compounds is ListTag() -> list_data_type == 1 (TAG_Byte, "
         "serialized as element type byte 0x01); amulet_nbt has no way to "
         "pre-declare the element type of an empty list")

# 8. nested ------------------------------------------------------------------
# a:{b:{c:{d:{e:1}}}}
lvl_d = an.CompoundTag(); lvl_d["e"] = an.IntTag(1)
lvl_c = an.CompoundTag(); lvl_c["d"] = lvl_d
lvl_b = an.CompoundTag(); lvl_b["c"] = lvl_c
lvl_a = an.CompoundTag(); lvl_a["b"] = lvl_b
c = an.CompoundTag()
c["a"] = lvl_a
# l:[{x:[{y:2}]}]
y_comp = an.CompoundTag(); y_comp["y"] = an.IntTag(2)
x_comp = an.CompoundTag(); x_comp["x"] = an.ListTag([y_comp])
c["l"] = an.ListTag([x_comp])
add("nested", c)

# 9. deep strings ------------------------------------------------------------
_LONG = ("The quick brown fox jumps over the lazy dog. " * 200)[:5000]
assert len(_LONG) == 5000
c = an.CompoundTag()
c["text"] = an.StringTag(_LONG)
add("deep_strings", c,
    note="value is exactly 5000 ASCII chars (5000 bytes of modified UTF-8)")

# 10. big --------------------------------------------------------------------
c = an.CompoundTag()
for i in range(50):
    c["key_%d" % i] = an.IntTag(i)
add("big", c)

# 11. extra: empty root name -------------------------------------------------
add("unnamed_root", an.CompoundTag({"v": an.IntTag(7)}), root_name="",
    note="ADDITION beyond the requested list: same as a tiny compound but the "
         "root tag name is the empty string (.root.txt is 0 bytes)")


# --------------------------------------------------------------- dumping ----

def dump_one(name):
    nt, note = DOCS[name]
    be = nt.to_nbt(compressed=False, little_endian=False)
    le = nt.to_nbt(compressed=False, little_endian=True)
    snbt = nt.to_snbt()                     # indent=None (compact)
    root = nt.name

    paths = {
        "be": os.path.join(OUT_DIR, name + ".be.bin"),
        "le": os.path.join(OUT_DIR, name + ".le.bin"),
        "snbt": os.path.join(OUT_DIR, name + ".snbt"),
        "root": os.path.join(OUT_DIR, name + ".root.txt"),
    }
    with open(paths["be"], "wb") as f:
        f.write(be)
    with open(paths["le"], "wb") as f:
        f.write(le)
    with open(paths["snbt"], "wb") as f:
        f.write(snbt.encode("utf-8"))
    with open(paths["root"], "wb") as f:
        f.write(root.encode("utf-8"))

    # ---- verification -------------------------------------------------
    # problems  = real failures (files not byte-stable / data not equivalent)
    # quirks    = documented amulet_nbt behaviours that are NOT failures
    problems = []
    quirks = []

    orig_tag = back_be = an.load(io.BytesIO(be), compressed=False, little_endian=False)
    if back_be.name != root:
        problems.append("BE root name %r != %r" % (back_be.name, root))
    if back_be.to_nbt(compressed=False, little_endian=False) != be:
        problems.append("BE re-serialize != original")
    if back_be.to_snbt() != snbt:
        problems.append("BE reload SNBT mismatch")

    back_le = an.load(io.BytesIO(le), compressed=False, little_endian=True)
    if back_le.name != root:
        problems.append("LE root name %r != %r" % (back_le.name, root))
    if back_le.to_nbt(compressed=False, little_endian=True) != le:
        problems.append("LE re-serialize != original")

    with open(paths["snbt"], "rb") as f:
        snbt_bytes = f.read()
    if snbt_bytes != snbt.encode("utf-8"):
        problems.append("on-disk SNBT != in-memory SNBT")

    reparsed = None
    try:
        reparsed = an.from_snbt(snbt_bytes.decode("utf-8"))
    except Exception as exc:
        problems.append("from_snbt failed: %r" % (exc,))

    if reparsed is not None:
        # semantic equivalence, insensitive to compound key order
        mm = deep_mismatches(orig_tag.tag, reparsed)
        known_mm = [m for m in mm
                    if m.split(":", 1)[0] in
                    {"$.%s" % k for k in KNOWN_SNBTPARSE_LIMITATION_KEYS}]
        other_mm = [m for m in mm if m not in known_mm]
        for m in other_mm:
            problems.append("SNBT round-trip semantic mismatch: " + m)
        if known_mm:
            quirks.append(
                "from_snbt cannot parse the emitted non-finite float tokens "
                "(inff/-inff/nanf -> StringTag): " + "; ".join(known_mm))

        re_snbt = reparsed.to_snbt()
        if re_snbt != snbt:
            if known_mm:
                quirks.append("to_snbt(from_snbt(...)) differs from the stored "
                              "SNBT only in the inff/-inff/nanf keys "
                              "(they come back quoted as strings)")
            else:
                problems.append("SNBT re-serialize mismatch:\n  first : %r\n  second: %r"
                                % (snbt, re_snbt))

        # byte level: does SNBT -> tag -> NBT reproduce the original bytes?
        re_be = an.NamedTag(reparsed, root).to_nbt(compressed=False, little_endian=False)
        if re_be != be and not known_mm:
            # Prove the difference is purely compound key order: rebuild the
            # original tag following the key order that the SNBT text used
            # (recursively) and re-serialise.
            ordered = reorder_by_template(orig_tag.tag, reparsed)
            ordered_be = an.NamedTag(ordered, root).to_nbt(compressed=False,
                                                           little_endian=False)
            if ordered_be == re_be:
                quirks.append(
                    "binary key order = insertion order, SNBT key order = sorted "
                    "-> a SNBT round trip reorders compound entries in the binary "
                    "(all values otherwise identical)")
            else:
                problems.append("SNBT->tag->NBT bytes differ by more than key order")
        elif re_be != be and known_mm:
            quirks.append("SNBT->tag->NBT bytes differ (inf/nan keys become "
                          "strings; see limitation above)")

    with open(paths["root"], "rb") as f:
        if f.read() != root.encode("utf-8"):
            problems.append("root.txt mismatch")

    # key order reporting
    bin_keys = list(orig_tag.tag.keys())
    s_keys = snbt_top_keys(snbt)
    expect = sorted(bin_keys, key=amulet_sort_key)
    order_info = "snbt key order %s binary insertion order" % (
        "==" if s_keys == bin_keys else "!=")
    order_info += "; snbt order %s amulet's case-insensitive key sort" % (
        "==" if s_keys == expect else "!=")
    if s_keys != bin_keys:
        order_info += " (a SNBT round trip therefore changes binary byte order)"

    sizes = {k: os.path.getsize(v) for k, v in paths.items()}
    return sizes, problems, quirks, order_info, be, snbt


def main():
    os.makedirs(OUT_DIR, exist_ok=True)
    print("amulet_nbt version:", an.__version__)
    print("output dir:", OUT_DIR)
    print()

    all_problems = []
    all_quirks = []
    results = {}
    for name in DOCS:
        sizes, problems, quirks, order_info, be, snbt = dump_one(name)
        results[name] = (sizes, be, snbt)
        flag = "OK  " if not problems else "FAIL"
        note = DOCS[name][1]
        print("[%s] %-16s be=%-6d le=%-6d snbt=%-6d root.txt=%d"
              % (flag, name, sizes["be"], sizes["le"], sizes["snbt"], sizes["root"]))
        print("       " + order_info)
        if note:
            print("       note: " + note)
        for q in quirks:
            print("       KNOWN amulet_nbt behaviour: " + q)
            all_quirks.append((name, q))
        for p in problems:
            print("       PROBLEM: " + p)
            all_problems.append((name, p))
    print()

    # ---- float details -------------------------------------------------
    print("=" * 72)
    print("FLOATS: exact values used for document 'floats'")
    print("=" * 72)
    for k, v in FLOAT_CASES:
        tag = an.FloatTag(v)
        print("  %-8s python=%-24r float32_bits=0x%08X  snbt=%r"
              % (k, v, f32_bits(v), tag.to_snbt()))
    print("  (FloatTag stores numpy float32; to_snbt prints the exact")
    print("   shortest repr of the stored value, so 1e-38 prints as")
    print("   9.999999350456404e-39f and 3.4028235e38 as 3.4028234663852886e+38f)")
    print()

    # ---- string code points -------------------------------------------
    print("=" * 72)
    print("STRINGS: exact code points")
    print("=" * 72)
    for k, v in STRING_CASES:
        print("  %-10s len=%d  %s" % (k, len(v), cps(v) if v else "(empty)"))
    print()

    # ---- verbatim SNBT -------------------------------------------------
    print("=" * 72)
    print("SNBT VERBATIM: scalars")
    print("=" * 72)
    print(results["scalars"][2])
    print("repr(scalars snbt):")
    print(repr(results["scalars"][2]))
    print()
    print("=" * 72)
    print("SNBT VERBATIM: strings  (contains raw NUL/NEWLINE/TAB bytes)")
    print("=" * 72)
    print(results["strings"][2])
    print()
    print("repr(strings snbt):")
    print(repr(results["strings"][2]))
    print()

    # ---- hex dumps -----------------------------------------------------
    dumps = [
        ("empty_compound", None),
        ("scalars", None),
        ("arrays", None),
        ("strings", None),
        ("big", 400),
        ("deep_strings", 400),
    ]
    for name, limit in dumps:
        be = results[name][1]
        print("=" * 72)
        print("HEX DUMP %s.be.bin (%d bytes%s)"
              % (name, len(be), "" if limit is None else ", showing first %d" % limit))
        print("=" * 72)
        print(hexdump(be, limit))
        print()

    if all_problems:
        print("VERIFICATION FAILED: %d problem(s)" % len(all_problems))
        for name, p in all_problems:
            print("  %s: %s" % (name, p))
        return 1
    print("VERIFICATION: all documents round-trip OK "
          "(BE bytes, LE bytes, SNBT text on disk, root name)")
    print("KNOWN amulet_nbt behaviours observed: %d (documented above, "
          "not failures)" % len(all_quirks))
    for name, q in all_quirks:
        print("  %s: %s" % (name, q))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
