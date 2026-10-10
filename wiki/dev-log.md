# Dev log

Newest entry at the top. Append; do not rewrite history.

Status vocabulary:
**scaffold** = files exist, not functional · **wip** = being written ·
**works** = runs, may lack tests · **done** = runs and covered by tests ·
**blocked** = needs something first

---

## 2026-10-10 — Phase 2: NBT library, golden parity, fuzzing

**Status: done**

Binary NBT, SNBT and modified UTF-8, validated byte-for-byte against the
reference Python `amulet_nbt` 2.1.8, with libFuzzer harnesses for both parsers.

### Built

- **`include/mcme/nbt.h`, `src/nbt/`** — one tree, two encodings. Big-endian
  (Java) and little-endian (Bedrock) binary read/write, SNBT read/write, JSON
  output, navigation (`mcme_nbt_path` dotted lookup through compounds and
  lists), and builders.
- **`include/mcme/mutf8.h`, `src/nbt/mutf8.c`** — full modified-UTF-8 codec.
  `U+0000` → `C0 80`, supplementary characters → 6-byte surrogate pair, lone
  surrogates preserved as WTF-8 rather than replaced with `U+FFFD`.
- **`McmeBuffer`** (`src/core/buffer.c`) — growable byte buffer, the output
  target for every serializer. Floats go through bit reinterpretation so NaN and
  `-0.0` survive.
- **`tools/ref_dump.py`** — drives the oracle; writes 44 golden files for 11
  documents into `tests/golden/nbt/`.
- **`mcme nbt dump <file>`** — SNBT by default, `--json`, `--le`, `--path`,
  `--indent`. Documented in [cli.md](cli.md).
- **`./rebuild.sh fuzz`** — builds `fuzz_nbt` and `fuzz_snbt` with clang
  (`-fsanitize=fuzzer,address,undefined`). Kept out of the default build so a
  normal `./rebuild.sh` stays a plain cc build.
- **Full format contract written up in [nbt.md](nbt.md).** That page is the
  one to read before touching any formatting; `nbt.h` points at it.

### The oracle

`amulet_nbt` ships as a prebuilt extension with no source, so it is used as an
oracle rather than read. Two of its quirks are worth repeating here because they
inverted what I expected:

- `from_snbt("42")` returns `IntTag(42)` — **SNBT may have a bare scalar root.**
- `load(<root TAG_Byte>)` succeeds — **binary NBT may have a non-compound
  root.**

I had both wrong. My parser demanded a compound root, which the reference does
not; the fuzzer caught it when a bare SNBT scalar was written as binary and then
refused by my own reader. The reader was the side that diverged, not the
SNBT parser.

### Bugs found

**By the fuzzer (3) — all now regression inputs in `tests/fuzz/regressions/`:**

| Input | Symptom | Fix |
|---|---|---|
| `bad-utf8-in-string.snbt` | SNBT with malformed UTF-8 (`CA 20`) parsed fine, then failed on `write_binary`. A world we could **read but not save** — silent data loss. | Every string now enters the tree via `mcme_nbt_str_set()`, which validates with the new `mcme_utf8_valid()`. That one choke point covers binary read, quoted and bare SNBT, keys, and the public setter. |
| `bare-scalar-root.snbt` | SNBT bare scalar → binary → our own reader rejects it for not being a compound. | Reader now accepts any real tag type (1–12), refusing only `TAG_End` and undefined numbers, matching *"Tag N does not exist"*. |
| `heterogeneous-list.snbt` | `[IB, -128B]` parsed as a list declared `String` holding a `Byte`. One element written at the wrong width → `reparse: truncated at byte 54, need 32758 more`. | Lists must be homogeneous (reference: *"Expected type IntTag but got StringTag"*). Rejected on read, reporting the offset where the offending element **begins**. |

**By inspection, before running anything:**

- SNBT compound and list separators were `","`; the reference emits `", "`.
  A missing space, caught by reading `cat -A` on the golden files.
- Array prefixes were written `[B` with no semicolon — `[B;` is the format.

**Confirmed against the oracle:** compound keys are **unique**, collapsing to
first-position/last-value on *both* read paths (Python-dict semantics). Amulet
reads a 20-byte file with a duplicated key and writes back 12, so its round trip
is not byte-exact there either. Implemented once in
`mcme_nbt_compound_seal()`, which both parsers already funnel through, using a
sort rather than a per-key scan so a compound's attacker-controlled entry count
cannot make parsing quadratic.

### Testing

- **Golden suite**: 11 documents × 7 assertions, including two cross-endian
  checks (parse BE → write LE must equal `.le.bin`) that cannot be satisfied by
  accident.
- **The harness was itself verified.** Corrupting one payload byte in
  `scalars.be.bin` made the cross-checks fail as expected while the
  self-consistent BE→BE round trip correctly still passed. A suite that cannot
  fail proves nothing.
- **Fuzzing, final runs**: `fuzz_snbt` 1,319,998 execs, `fuzz_nbt` 922,773
  execs, both exit 0. The invariant is **idempotence** (`Y == Z`), not merely
  "did not crash".
- Truncation sweeps over every strict prefix of `strings` and `nested`, in both
  encodings; ASan enforces the out-of-bounds half.

### Known gaps / next up

- **Gzip still not handled.** `mcme nbt dump` on a `1f 8b` file exits 5 with a
  clear message. Phase 3, together with miniz.
- **No region/chunk layer yet.** Everything here is a single document.
- **JSON output flattens types** and renders infinity/NaN as `null`.
- **`.lang` list still hardcoded in `main.c`.**
- **leveldb not linked yet** — Phase 3, with Bedrock.
- **`wiki/porting/` does not exist**, though [README.md](README.md) and
  [cli.md](cli.md) both link to it. Those links are currently broken. The
  parity checklist they promise is the natural companion to Phase 3, since that
  is when world formats start landing.

### Next

Phase 3 — world formats: the region/chunkIO layer, gzip, `mcme info`, and the
leveldb link. NBT is ready to be its payload format.

---

## 2026-10-10 — Phase 0/1: skeleton, core primitives, CLI contract

**Status: done**

Set up the project and the first working slice. Deliberately CLI-first so every
later phase can be tested by a script with no GUI involved.

### Built

- **Toolchain confirmed.** gcc 16.2.1, clang 23.1.1, git 2.56, GTK4 **4.24.1**
  available via pkg-config. So the GTK4 decision in
  [decisions.md](decisions.md) is safe — it is already installed.
- **Repo + tree.** `git init` at `~/mythings/studying_code/mcpe_map_edit`.
  Directory layout mirrors subsystem boundaries (see README).
- **Vendored deps** (all permissive, single-file where possible): `nob.h` v3.10,
  `stb_ds.h` v0.67, `stb_image.h` v2.30, `stb_image_write.h` v1.16,
  `stb_image_resize2.h` v2.18, `cJSON`, `miniz` 3.1.2, `lz4`. leveldb and GTK4
  are system-linked, not vendored.
- **Build system.** `rebuild.sh` bootstraps `nob`, `build.c` walks `src/`
  recursively and links `mcme`. Debug builds default to
  `-fsanitize=address,undefined`. `./nob test` builds and runs unit tests.
- **Core primitives** (`src/core/`): logging, error string, file IO, arena
  allocator, event queue, thread pool, `.lang` parser, JSON config.
- **CLI** (`src/app/main.c`): argument parsing, `--help`, `--version`,
  `--json`, `--quiet`, `-v`. Subcommands `lang`, `config`, `selftest`.
  Stable exit codes in `include/mcme/cli.h`.
- **Tests** (`tests/unit/test_core.c`): arena, lang parser, event queue,
  thread pool, config round-trip. Green under ASan/UBSan.

### Decisions made this session

- **GUI toolkit: GTK4.** Verified installed. Rationale in
  [decisions.md](decisions.md#gtk4-for-the-gui).
- **Config is JSON, not pickle.** The original used gzipped Python `pickle`,
  which is unportable and unsafe to load. Ours is plain JSON.
- **CLI is the primary interface.** Written up as a contract in [cli.md](cli.md)
  so agents can rely on exit codes and `--json` shapes.

### Bugs found on day one — all caught before any feature existed

This is the whole reason the toolchain was wired with `-Werror` and
ASan/UBSan *before* writing a single subsystem. Every one of these is a
mistake I would have shipped into the world parser otherwise.

| Bug | Where | Why it mattered |
|---|---|---|
| **Use-after-free on JSON error path** | `config.c` | `cJSON_GetErrorPtr()` returns a pointer *into* the buffer passed to `cJSON_Parse`. I freed that buffer, then read through the pointer. |
| **`realloc`-grown string pool** | `lang.c` | Interning strings into one growing buffer **moves** it, dangling every key/value pointer already handed out. Passed the tiny unit test (one 4 KB block, no realloc); crashed on the real 22 KB `en.lang`. Fixed with a chunked chain that never moves. |
| **Arena aligned the size, not the pointer** | `arena.c` | The block header is 24 bytes, so `data[]` starts 8-aligned. `sizeof(float) * N` SIMD loads would have faulted. Now pads each allocation up to the next 16-byte boundary. |
| **`cJSON_AddItemToObject` appends** | `config.c` | Setting the same key twice wrote a duplicate JSON member. |
| **Double-printed errors** | `main.c` | Subcommands printed the error *and* `main()` printed it again. Now only `main()` prints. |
| **`--json` only parsed before the command** | `main.c` | `mcme selftest --json` silently emitted prose. The wiki documented the flag anywhere, so the parser was wrong, not the doc. |
| **Static buffer reused across a loop** | `build.c` | `obj_path_for()` returned one `static char[]`, but `nob_cmd_append` stores *pointers*. Every object argument in the link loop aliased the last path written. |

**The pattern worth keeping:** six of the seven are lifetime or ownership
mistakes, and five are invisible to a test that uses small inputs. That is
exactly the failure mode of a world parser, which is why the sanitizers are
on by default and not something you opt into.

### Verified working

```
./rebuild.sh test            # builds ./mcme + tests, all green
./mcme --version             # mcme 0.1.0
./mcme selftest --json       # {"suites":[{"name":"core","failed":0}],"failed":0}
./mcme lang list             # 18 languages from assets/lang/
./mcme lang get program_3d_edit.tab_name --lang ja   # マップの編集
./mcme config set options.fov 90                     # overwrites, no dup key
./mcme info x ; echo $?      # exit=5 (reserved, not implemented)
```

Clean under ASan/UBSan across repeated runs. `-Wall -Wextra -Werror` on our
code; third-party is compiled separately without `-Werror`.

### Known gaps / next up

- **No world formats yet.** `mcme info <world>` returns
  `MCME_EXIT_NOT_IMPLEMENTED`. This is the Phase 3 entry point.
- **No NBT yet.** `mcme nbt` is reserved but unimplemented. Phase 2.
- **miniz is vendored but not compiled.** Upstream master expects a
  CMake-generated `miniz_export.h` it does not ship. We need gzip for the
  Anvil region format in Phase 3; at that point either generate the header or
  define the export macro. Kept out of the build until then — an unused
  dependency is an unused way to break the build.
- **lz4 is compiled but unused.** It comes in with Bedrock chunks. Kept only
  because it costs nothing and is a single file; revisit if the build time
  ever matters.
- **leveldb not linked yet.** Header check passed; wiring comes with Bedrock
  support.
- **Fuzz targets not written yet.** Planned for NBT and SNBT in Phase 2 — the
  parsers touch untrusted world data, so they go first.
- **`selftest` covers core only.** It grows one suite per subsystem.
- **`.lang` list is hardcoded in `main.c`**, not read from the directory.
  Works, but adding a language file needs a code change. Fix when touching
  `lang` next.

### Next

Phase 2 — the NBT library. Binary read/write, SNBT, modified-UTF-8, plus golden
tests that diff our output against the reference Python `amulet_nbt` build, and
libFuzzer harnesses for both parsers.
