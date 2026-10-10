# Decisions

One entry per architectural decision. Newest at the bottom of each section.
Each entry: **what we chose, why, what we rejected, and what would make us
reverse it.** If you change your mind, add a new entry that supersedes the old
one — do not edit the old entry into agreeing with you.

---

## D1 — C11, not C++ and not Rust

**Chosen.** Strict C11 (built as `gnu11` for `typeof` and statement
expressions).

**Why.** The goal is learning plus full control of the generated code. C is the
smallest toolchain and the easiest to read the assembly of. The mesher in the
original is already written as C structs under Cython, so the performance-
critical part maps across almost literally.

**Rejected.**
- *C++*: better STL, but templates and implicit lifetime make the port harder to
  reason about and reintroduce the abstraction creep simplexity forbids.
- *Rust*: excellent for exactly this workload, but ownership fights the
  "caller frees" model a C port wants, and the brief was strictly C.

**Reverses if:** we need RAII-style cleanup for a subsystem with 50+ live
handles, or GTK interop becomes painful. Not expected.

---

## D2 — GTK4 for the GUI

**Chosen.** GTK4, system-linked (LGPL, dynamic linking).

**Why.** It is the only mature GUI toolkit with a **C API** that ships the
widgets this app actually needs: dialogs, file choosers, tree views (for the
NBT editor), list widgets, notebook tabs (one per open world), menus, and
`GtkGLArea` for the live 3D viewport. Verified installed: **GTK4 4.24.1**.

The original is wxPython — a binding over wxWidgets, which is C++. There is no C
binding. So the realistic choice was GTK4 or hand-rolling every widget in SDL3.
Hand-rolling is roughly +30–50k lines for the dialogs alone.

**Rejected.**
- *SDL3 + custom UI*: pure C and permissive, but Amulet has ~15 non-trivial
  dialogs, a tree-based NBT editor, and dockable panels. Too much work for no
  benefit.
- *wxWidgets from C*: possible via its C++ core, but violates D1.
- *Dear ImGui*: C++.

**Cost accepted.** The UI will not be pixel-identical to Amulet. This is
**feature parity, not visual parity.**

**Reverses if:** GTK4 licensing or cross-platform packaging becomes a real
problem. At that point the honest move is C++ + wxWidgets, which breaks D1 — so
it would be a two-decision reversal.

---

## D3 — CLI is the primary interface, GUI is a client

**Chosen.** Every capability is reachable from `mcme <verb>` first. The GUI
calls the same code.

**Why.** Three reasons, in order of weight:
1. **Testability.** A command can be run by a script or an agent, asserted on
   exit code and `--json`. A widget cannot.
2. **Parity checking.** We can diff `mcme convert` output against the Python
   reference byte-for-byte without a display server.
3. **It is simpler.** If a feature needs the GUI to be exercised, there is
   pressure to special-case it in the GUI layer. Making it a command removes
   that pressure.

**Rejected.** *GUI-first with a thin CLI.* Rejected because it makes the domain
layer entangled with widgets, which is how the original ended up with world
logic importing `wx`.

**Reverses if:** never, for this project. This is load-bearing.

---

## D4 — JSON config instead of Python pickle

**Chosen.** User config is a plain JSON file.

**Why.** The original's `api/config.py` writes gzipped Python `pickle` to
`$CONFIG_DIR/<id>.config`. Pickle is a Python-only, arbitrary-code-execution
format — it cannot be read by C, and reading one from an untrusted source is a
security problem. JSON is boring and portable, and we already vendor cJSON.

**Rejected.** *Binary config with a custom header.* Faster, but config is
written once per session and read once at startup. Not worth the format to
maintain.

**Accepted cost.** Slightly larger file and no type enforcement beyond what we
validate on load.

---

## D5 — Vendor single-file libraries; link big ones from the system

**Chosen.** Small, permissive, single-file libraries get vendored into
`third_party/`. Large or frequently-patched ones get linked from the system.

Vendored: `nob.h` (build), `stb_ds.h` / `stb_image.h` / `stb_image_write.h` /
`stb_image_resize2.h`, `cJSON`, `miniz`, `lz4`.
System-linked: **GTK4**, **leveldb**.

**Why.** Vendoring makes builds reproducible and offline. Linking the two big
ones avoids carrying ~50k lines of C++ (leveldb) and a very large C API (GTK4)
in-tree where we would have to track upstream security fixes ourselves.

**leveldb is C++ but reached through its C API** (`leveldb/c.h`). Our code stays
C11; the C++ lives behind a system library and is isolated behind
`mc_leveldb.h` so it can be swapped later.

**Reverses if:** we need a pure-C leveldb for a target with no packaged leveldb.
Then we write a C implementation behind the same header.

---

## D6 — Threading model: workers produce data, only the main thread touches GL

**Chosen.** Mirrors the original's contract exactly, because the original got it
right. Worker threads do CPU work (mesh building, chunk loading) and push
results through a thread-safe queue. The GTK main loop drains that queue and is
the only thread that calls OpenGL.

**Why.** OpenGL contexts are single-threaded. The original enforces this with
`wx.CallAfter`/`wx.PostEvent` and a merge-on-draw queue. We replace those with
`mcme_eq_post` (any thread) / `mcme_eq_drain` (main loop via `g_idle_add`).

**Reverses if:** we move to a render-thread + GL context sharing model. Would
buy parallelism at the cost of substantial complexity. Not justified yet.

---

## D7 — No generic format-plugin framework

**Chosen.** Each world format is a normal C file with an explicit registration
table. There is no dynamic plugin loader.

**Why.** The original loads world formats and user "operations" via Python
`import`. Porting that literally means a `dlopen` ABI plus a scripting language.
Simplexity rule 6 forbids building that before a third caller exists. Today we
need: anvil, leveldb, and a handful of schematic formats. A static table with
five entries is obviously correct.

**Planned, not built.** A `dlopen` plugin ABI in a later phase, and optionally
embedded Lua for user-authored operations. Deliberately deferred.

---

## D8 — System zlib for gzip/zlib, not vendored miniz

**Chosen.** Link system `zlib` (`-lz`) for gzip and zlib streams.
`third_party/miniz` stays on disk but is **never compiled**.

**Why.** Phase 3 needs zlib for three real things: Anvil region chunk payloads
(raw zlib), Java `level.dat` (gzip), and Bedrock `level.dat` (gzip). miniz was
vendored for this in Phase 1 on the theory that it is a single-file C library.
That theory failed twice:

1. miniz does not ship `miniz_export.h`. It is CMake-generated, so using it
   means running CMake — exactly the build complexity `rebuild.sh` exists to
   avoid. This is already recorded in the Phase 1 note in `build.c`.
2. zlib is **already a hard dependency of GTK4** via libpng/gdk-pixbuf, and it
   is present on every target we care about. Vendoring miniz would not remove a
   dependency; it would add a second implementation of one we already have.

So miniz bought nothing and cost a build system.

**Rejected.**
- *miniz with a hand-written `miniz_export.h`*: works, but carries a second
  gzip implementation whose behaviour on malformed input we would have to test
  against zlib's anyway.
- *stb's gzip writer only*: `stb_image_write` has no inflate. We need to read.
- *A self-written DEFLATE decoder*: educational, but it is a well-known
  security-sensitive surface and zlib is already on the machine. Not a good
  trade.

**Cost accepted.** `mcme` is no longer buildable on a system without zlib.
That is acceptable: zlib is ubiquitous, and D2 already makes GTK4 a system
dependency.

**Reverses if:** we need a genuinely static, dependency-free binary for a target
with no zlib. Then vendor miniz and generate the header once, behind
`mc_compress.h` — which is why all compression goes through that header and
nothing else in the tree calls `zlib.h` directly.
