# NBT: the format contract

The NBT layer is validated **byte-for-byte** against the reference Python build
of `amulet_nbt` **2.1.8**. This page records what that reference actually does,
because several of its rules look like bugs and are not.

Read this before changing any formatting in `src/nbt/`.

- [Oracle and golden data](#oracle-and-golden-data)
- [Binary](#binary)
- [Modified UTF-8](#modified-utf-8)
- [SNBT output](#snbt-output)
- [Behaviours that look wrong but are not](#behaviours-that-look-wrong-but-are-not)
- [Behaviours pinned by fuzzing](#behaviours-pinned-by-fuzzing)
- [Intentional divergences](#intentional-divergences)
- [Testing](#testing)

## Oracle and golden data

The reference ships as a prebuilt extension with no source, so it is treated as
an oracle: we run it, never read it.

```sh
VENV=~/Downloads/Amulet-Map-Editor-0.10.64/venv
$VENV/bin/python tools/ref_dump.py        # regenerate tests/golden/nbt/
```

For each of 11 documents the generator writes four files:

| File | Contents |
|---|---|
| `<name>.be.bin` | big-endian (Java) binary |
| `<name>.le.bin` | little-endian (Bedrock) binary |
| `<name>.snbt` | SNBT text, no trailing newline |
| `<name>.root.txt` | the root tag's name (`Root`, or empty) |

Documents: `empty_compound`, `scalars`, `floats`, `arrays`, `strings`, `lists`,
`compound_list`, `nested`, `deep_strings`, `big`, `unnamed_root`.

The two binaries differ **only in byte order** — same keys, same order. That is
what makes the cross-endian tests meaningful: parsing `.be.bin` and writing LE
must produce `.le.bin` exactly, which cannot happen by accident.

## Binary

- Big-endian by default (Java); `--le` / `little_endian` for Bedrock. Both are
  supported on read and write, and they are structurally identical — every
  multi-byte integer and every length prefix simply flips.
- Layout per tag: `type:u8`, then for a *named* position a `u16` length-prefixed
  name, then the payload.
- **The root may be any real tag type, not only a compound.** `amulet_nbt`
  loads a root `TAG_Byte` or `TAG_List` without complaint. Only two things are
  refused: `TAG_End` (`0x00`), which is a terminator rather than a value, and
  type numbers above `12`. The reference reports these as *"Tag 0 does not
  exist"* / *"Tag 110 does not exist"*; we say *"tag type N does not exist"*.
  Minecraft files just always happen to use a compound, which makes this easy to
  over-constrain — see `test_non_compound_root`.
- Compound length is not a prefix; entries run until `TAG_End`. The parser
  stages them in a growable array and copies into the arena once, because an
  arena cannot `realloc` in place and a two-pass parse would duplicate all the
  validation.
- `MCME_NBT_MAX_DEPTH` is **512**. Deeper nesting is rejected rather than
  blowing the C stack; crafted nesting is a standard denial-of-service shape.

## Modified UTF-8

Binary string payloads use Java's *modified* UTF-8 (`DataOutputStream.writeUTF`),
which differs from standard UTF-8 in exactly two ways — and both are the cases a
naive port gets wrong:

| | Modified UTF-8 | Standard UTF-8 |
|---|---|---|
| `U+0000` | `C0 80` (never a bare `00`) | `00` |
| `U+10000` and above | **6 bytes** — UTF-16 surrogate pair, each half as 3 bytes | 4 bytes |

So a four-byte lead (`F0..F7`) is *invalid* modified UTF-8 — the quickest way to
tell the two encodings apart.

Inside the tree a string is held as **standard UTF-8** (the reference is Python,
so this matches its in-memory form) and converted only at the binary boundary:
`mcme_mutf8_decode()` on read, `mcme_mutf8_encode()` on write.

Golden data points worth memorising, all from the `strings` document:

| Value | Binary bytes |
|---|---|
| `"a" + U+0000 + "b"` | `61 c0 80 62` |
| `😀` | `ed a0 bd ed b8 80` (6 bytes) |
| flag `🇬🇧` | `ed a0 bd ed b8 80 ed b0 9f ed b0 ba` (12 bytes) |

**Lone surrogates are preserved** as their three-byte sequence (WTF-8) rather
than replaced with `U+FFFD`, and the encoder accepts them back. This is a
deliberate choice: a file we could read must be a file we can write, or an
editor silently loses data on save. `mcme_mutf8_cp_len()` stays strict, because
it answers "is this a code point", and a surrogate is not one.

## SNBT output

Exact rules, all verified against the reference:

- **Keys are sorted case-insensitively**, with lowercase before uppercase as the
  tiebreak (Python's `str.lower()` ordering). Binary keeps file/insertion order.
  The two orderings genuinely differ — see below.
- **Strings are always `"`-quoted** and only `"` and `\` are escaped. Control
  characters — including `NUL`, tab and newline — are written **raw**. Strings
  are standard UTF-8 here, even though the binary form is modified UTF-8.
- **Numbers use Python `repr`**: the shortest decimal that round-trips through
  `strtod`. So `3.4028234663852886e+38` and `9.999999350456404e-39`, and the
  CPython reshaping rule `use_exp = (decpt <= -4) || (decpt > precision)` with a
  `%+03d` exponent (`e+38`, `e-39`).
- **Suffixes**, in their exact cases:

  | Type | Emitted | Example |
  |---|---|---|
  | Byte | `b` | `1b` |
  | Short | `s` | `2s` |
  | Int | *(none)* | `3` |
  | Long | `L` | `4L` |
  | Float | `f` | `5.5f` |
  | Double | `d` | `6.25d` |

- Arrays: `[B;0B, 1B]`, `[I;0, 1]`, `[L;0L, 1L]` — note the semicolon, the
  uppercase element letters for byte arrays, and no suffix on int-array
  elements. An empty array is `[B;]`.
- Compound/list separators are `", "` (comma **and** space) in compact form;
  pretty form uses `,` plus a newline. Keys use `": "`.

## Behaviours that look wrong but are not

These are the four that `nbt.h` points at. All are confirmed behaviour, not
oversight.

1. **An empty list is written with element-type byte `0x01`, not `0x00`.** The
   parser preserves whatever byte it read, so a round trip is byte-exact.
   `TAG_End` as an element type would be meaningless — a list of End tags has no
   payload at all, so a 4-byte count would request elements that were never
   written.
2. **SNBT sorts keys; binary does not.** `{i: 3, j: 4, a: 1}` produces
   `{"a": 1, "i": 3, "j": 4}` as SNBT but `i, j, a` order in binary. Tests must
   therefore never compare SNBT against a binary file directly — parse the
   binary, write SNBT, compare to `.snbt`.
3. **SNBT escapes only `"` and `\`.** A raw newline inside a string is not
   written as `\n`. The reference does this, so `.snbt` files contain literal
   control bytes.
4. **`inff`, `-inff`, `nanf` are emitted but the reference cannot re-parse
   them** — it turns them back into strings, so its own output does not survive
   its own round trip. We accept them on input (plus bare `inf`/`nan`) so ours
   does. This is the one place we are deliberately *more* permissive than the
   oracle, and it is what makes the round-trip invariant hold.

## Behaviours pinned by fuzzing

The two libFuzzer targets assert **idempotence**, not merely survival:

```
parse(X) -> write -> Y
parse(Y) -> write -> Z          assert Y == Z
```

Note `Y == Z`, not `X == Y`. Input may legitimately not round-trip — a
hand-written file can use a non-canonical spelling that our writer normalises.
What must hold is that once we have written a file, re-reading and re-writing it
is stable. A violation means the reader and writer disagree about the format,
which in an editor is silent corruption.

Three real bugs were found this way and are now regression inputs under
`tests/fuzz/regressions/`:

| Input | Found |
|---|---|
| `bad-utf8-in-string.snbt` | The SNBT parser accepted a string containing malformed UTF-8 (`CA 20`). It parsed fine and only failed when writing binary — i.e. a world we could read but not save. Now every string enters the tree through `mcme_nbt_str_set()`, which validates with `mcme_utf8_valid()`; invalid text is rejected on read. |
| `bare-scalar-root.snbt` | A bare SNBT scalar (`02 00`) parses to a `String` root, is written as binary, then our own binary reader rejected it for not being a compound. The reader was the side that diverged — the reference accepts any real root type. |
| `heterogeneous-list.snbt` | See below. |

### Lists must be homogeneous

`amulet_nbt` refuses a list whose elements are not all the same type as the
first:

```
'[1, "two"]'  -> SNBTParseError: Expected type IntTag but got StringTag at 4
'[1b, 2]'     -> SNBTParseError: Expected type ByteTag but got IntTag at 5
```

This is not only a parity detail. Binary NBT stores **one** element type for the
whole list, so a heterogeneous list written under that type would serialize one
element at the wrong width and could never be read back — the exact corruption
the fuzzer caught (`reparse: truncated at byte 54, need 32758 more`). We reject
on read, reporting the byte where the offending element *begins*, matching the
reference's `at` position.

An empty list gets element-type `Byte`, matching what the reference writes.

### Compound keys are unique

A repeated key collapses to **first position, last value** — plain Python-dict
behaviour — on **both** the binary and the SNBT path. The reference reads a
20-byte file with a duplicated key as `{"a": 2}` and writes back 12 bytes, so
its own round trip is not byte-exact for that input. Neither is ours.

The collapse lives in `mcme_nbt_compound_seal()`, which both parsers funnel
through, so there is one rule instead of two. Grouping is done by sorting an
index array rather than scanning for each key: a compound's entry count is
attacker-controlled, and a linear search would make parsing quadratic — a
~600 KB file could hold 100k entries and stall the editor.

## Intentional divergences

| Where | We differ | Why |
|---|---|---|
| SNBT input | Accept `inff` / `nanf` / bare `inf` | Reference emits them but cannot re-read them. Round-trip stability. |
| SNBT input | Reject malformed UTF-8 in strings | Reference never sees raw bytes — it is handed a Python `str`. We must reject at the byte boundary or we cannot save what we read. |
| Lone surrogates | Preserved (WTF-8) both ways | Lossless round trip for odd files. |

## Testing

```sh
./rebuild.sh test                      # golden round trips + unit tests
./rebuild.sh fuzz                      # libFuzzer harnesses (needs clang)
./build/fuzz_nbt  build/corpus_nbt  -max_total_time=60
./build/fuzz_snbt build/corpus_snbt -max_total_time=60
./build/fuzz_snbt tests/fuzz/regressions/*   # replay fixed crashes
```

The golden suite asserts, for each document:

1. parse BE → write BE == `.be.bin`
2. parse BE → write LE == `.le.bin` (same key order, opposite byte order)
3. parse BE → write SNBT == `.snbt`
4. parse LE → write LE == `.le.bin`
5. parse LE → write BE == `.be.bin`
6. parse SNBT → write SNBT == `.snbt` (idempotent)
7. parse SNBT → write BE → parse → write SNBT == `.snbt`

(2) and (5) are the strong ones: producing the *other* endianness from a parsed
tree cannot be satisfied by accident.

Because a passing suite proves nothing on its own, the harness was checked by
corrupting one payload byte in `scalars.be.bin`: the cross-checks (BE→LE,
BE→SNBT, LE→BE) failed as they should, while the self-consistent BE→BE round
trip correctly still passed, since round-tripping a self-consistent file is
exactly what it is meant to do.

Truncation sweeps cut every strict prefix of `strings` and `nested` (binary and
SNBT) and assert nothing crashes; ASan is what actually enforces the
out-of-bounds half.

## Known gaps

- **Gzip is not handled.** `mcme nbt dump` reports a file starting with `1f 8b`
  as `MCME_EXIT_NOT_IMPLEMENTED`. Compression belongs to the Phase 3 region
  layer; miniz stays out of the build until then.
- **JSON output flattens types** and renders infinity/NaN as `null`, because
  JSON has no literal for them. Use SNBT when types matter.
- **No region/chunk NBT yet.** Everything here is a single document.
