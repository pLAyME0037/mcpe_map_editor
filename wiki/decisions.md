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

## D9 — Pure-C LevelDB, written here

**Chosen.** Implement the LevelDB on-disk format in C, behind
`include/mcme/leveldb.h`. No C++, no cmake, nothing to link.

**Why.** D5 assumed leveldb would be system-linked. It is not packaged on the
build machine, so that assumption failed, and the two ways out were: vendor
google/leveldb and build it with cmake, or write it.

Writing it wins on the constraint that actually governs this project. The brief
is **strictly C11/C17**, and "C++ behind a C API" was only ever acceptable as a
*system* dependency — something that lives outside our tree and outside our
build. Building ~40k lines of C++ in-tree, from a cmake sub-build, inside a
project whose entire premise is a hand-written C toolchain, is not that. It also
reintroduces the packaging weight simplexity argues against.

D5 already named this exact outcome as its own reversal condition, so this is
the documented path rather than a new invention.

**Scope.** Read first: log records, SSTable blocks, the index block, the footer
and the bloom filter, enough to enumerate and fetch every key. Write support
comes after, and the intended shape is appending to a log file — LevelDB
replays a log on open and compacts it later — rather than implementing
compaction ourselves.

**Rejected.**
- *Vendored google/leveldb + cmake*: battle-tested and much faster to land, but
  puts a C++ build step inside a strictly-C project. Right answer for a
  product; wrong answer for this brief.
- *Reusing `amulet-leveldb`'s extension module*: it is a CPython object, not a
  library with a C API we could call.
- *A LevelDB-compatible store of our own (write our own format)*: would not be
  readable by Minecraft, which is the entire point.

**Cost accepted.** We own the correctness of a storage format that can corrupt a
user's world if we get writing wrong. Mitigations, in order of weight: reads are
validated against the Python `leveldb` binding as an oracle; the reader refuses
anything it does not recognise rather than guessing; and writing lands only after
reads have been proven, so a bug in the writer is never also a bug in the reader
that would have to un-read it.

**Reverses if:** the reader proves too large to get right, or write support
turns out to need compaction after all. At that point vendoring upstream and
accepting the cmake step is the honest fallback, and `mc_leveldb.h` exists
precisely so that swap is local.

**What writing it found.** Implementing D9 turned up four things about the
format that no description of it led us to expect, each of which would have
made the reader silently wrong rather than loudly broken. They are recorded
here because they change what "read support" costs:

1. **Minecraft's fork is not stock LevelDB.** Table blocks are compressed with
   *bare DEFLATE* — no zlib header, no gzip header, no trailing checksum —
   and the block trailer labels it type 4. Stock LevelDB knows types 0 (none)
   and 1 (snappy). Every real Bedrock world writes type 4, so a reader built
   from the upstream format description reads none of them, and reports the
   failure as "unsupported compression", which reads like an optional feature
   being absent rather than the whole store being unreadable. This is now
   `MCME_COMPRESS_RAW_DEFLATE` in `compress.h`, and the block trailer is the
   only place that names it.
2. **The block trailer is not in the block.** A block handle's size counts the
   contents only; five bytes of `type + masked CRC32C` follow and are excluded.
   Reading the handle's size as the whole block puts the restart array at the
   wrong offset and mis-detects the block's end — and, because the trailer then
   lands on real data, it usually surfaces as a bogus compression type rather
   than as an off-by-five.
3. **The checksum covers the *stored* bytes.** The masked CRC32C is taken over
   the contents as they appear in the file, plus the type byte. For a
   compressed block that is the compressed form, so checksumming what the block
   decompresses to produces a block that decodes perfectly and still fails
   verification.
4. **Data blocks are about 100 KiB here.** Upstream targets 4 KiB. A world with
   a few thousand keys therefore has one data block and one index entry, which
   means a fixture built by driving Minecraft's own LevelDB never exercises the
   index walk's second iteration or resumption from a mid-block restart point.
   `tools/ref_leveldb_table.py` forges a small store with eight blocks and short
   restart runs specifically to cover that, and validates the forgery by reading
   it back with the reference library before any golden data is written.

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

---

## D10 — LevelDB writes are log-append only; no rotation, no compaction

**Chosen.** Writing appends WriteBatch records to the store's log, one record
per batch, and stops there. Sequence numbers come from `max(MANIFEST's
last_sequence, highest sequence in the data)`. A writable handle takes an
exclusive `flock` on `LOCK` — the lock Minecraft takes — for its lifetime.
Appending past 32 MiB refuses with an error instead of rotating the log.

**Why.** LevelDB replays a log on open, so a store built purely by appending
is correct for every reader without this code implementing compaction at all
(see D9's scope). The sequence max matters because either source alone can be
stale: the MANIFEST can be ahead of the data (batch written, MANIFEST updated,
crash before flush) and the data ahead of the MANIFEST (crash between the
two). Re-using a sequence number would make an old copy of a key sort newer
than the write that replaced it — a corruption that reads correctly.

Rotation is the load-bearing omission. LevelDB recovers only the logs numbered
at or above the MANIFEST's log number, because it assumes everything older was
flushed into a table first. A writer that starts a new log without that flush
makes every record in the old log invisible to every reader that trusts the
MANIFEST — including mcme's own reader. So there is none: one log for the life
of the handle, and a ceiling low enough (half the reader's 64 MiB intake limit)
that a session cannot grow a store only that session can read.

**Rejected.**
- *Rotating the log with a MANIFEST edit*: exactly the orphaning described
  above, unless paired with a table flush — which is compaction, the thing D9
  deferred.
- *Writing SSTables directly*: correct output, but a table writer is the
  single most corruption-prone piece of the format (index, restart array,
  filters, per-block checksums). Log-append is verifiable by reading it back
  with the reference library, which is what was done.
- *Mirroring upstream's memtable/write-buffer machinery*: a memtable only pays
  off if something eventually flushes it. Nothing here does.
- *Taking the lock on read-only opens too*: two readers are harmless, and
  `mcme info`/`mcme chunks` on a world the game has open is a normal use.

**Cost accepted.** A long editing session ends at 32 MiB of log with an error
telling it to reopen, and reopening does not help until compaction exists —
that is next. Reads also rebuild the merged view after every write, O(entries)
each time, which is blunt but reuses the open path rather than growing a
second merge implementation.

**Reverses if:** table writing lands, at which point rotation becomes safe and
the ceiling can go. The `mcme_leveldb.h` surface is where that swap belongs,
per D9.

---

## D11 — Compaction: full rewrite into one level-0 table

**Chosen.** Writing SSTables exists now, and compaction is what uses it: the
whole merged view is rewritten as one new level-0 table, the MANIFEST is
edited to point at it and delete the old tables, and only then are the old
tables and logs removed. `mcme_leveldb_compact()` is the verb;
`mcme_leveldb_write()` calls it itself once the log passes 4 MiB
(`MCME_LDB_LOG_COMPACT_AT`), mirroring upstream's default write buffer.
`mcme db compact <world>` is the CLI form. This supersedes D10's "not
supported" list and satisfies its reversal condition; D10 stays as written.

**The view does not change.** Every live entry goes into the new table under
its original internal key and sequence number, tombstones included. Dropping
tombstones is what upstream does at the bottommost level, but here it would
change `has()` and `get(deleted)` across a compaction — and a key emptied by
an edit must stay emptied across compaction and reopen. Preserving everything
also makes the property testable: compaction is correct exactly when the
merged view before and after is byte-identical, sequences included.

**Why one level-0 table and not upstream's leveled scheme.** The sizes at
stake are a single user's world and a store mcme already holds entirely in
memory to merge it. A leveled compaction needs per-level overlap tracking,
compaction pointers and picked-file logic — several hundred lines whose
correctness shows up as data loss in a rare path — to optimize rewrite cost
on a store that is rewritten whole in well under a second. One table is also
one file to reason about in the crash story below. The cost is real and
accepted: every compaction rewrites every byte, so a store edited in one
region repeatedly pays full rewrite each time. When that hurts, the level
machinery can be added behind the same verb.

**Index keys are the full last key of each block**, not upstream's shortest
separator. A separator is a size optimization; the full key is always a
valid separator, and no code here could get the shortening subtly wrong in a
way that only surfaces in someone else's index binary search. Blocks are
uncompressed (type 0), 4 KiB target, restart every 16 entries with the
restart array seeded at offset 0 — the layout the existing reader already
proves it can read.

**Crash-safety is ordering, not hope.** New table (fsync'd) → MANIFEST edit
appended (fsync'd) → old files deleted. A crash before the edit leaves an
unreferenced table and the old store intact; a crash after it leaves
unreferenced leftovers the MANIFEST no longer names. What never happens is a
MANIFEST naming bytes that might not have reached the disk. The fsyncs are
what make those windows real: without them the MANIFEST could land ahead of
the table it points at, and no later recovery can invent the missing bytes.

**The MANIFEST edit carries each old table's real level.** Recovery rejects
a `DELETED_FILE` whose level does not match where the file was added, so the
reader now tracks `(level, number)` pairs and applies deletions — which also
fixes reading any store that has ever been compacted by the game, since
`DELETED_FILE` records were previously walked past and their files demanded
from disk.

**Rejected.**
- *Leveled compaction as upstream does it*: see the cost argument above.
- *Dropping tombstones at the bottom level*: changes observable answers
  across compaction for bytes that fit in memory anyway.
- *Rotation without a flush*: D10's orphaning argument, unchanged — the log
  is retired by compaction, never renamed.
- *Writing the new MANIFEST rather than appending an edit*: a fresh MANIFEST
  must restate the whole version; an appended edit is smaller, is what the
  log-reader already handles, and cannot lose the old state by being torn
  mid-write.

**Cost accepted.** Full rewrites, and MANIFEST growth of one edit (tens of
bytes) per compaction. Both are invisible at these sizes.

**Verified against the reference, not against ourselves.** The binding that
generates the golden fixtures reads a store `mcme db compact` produced with a
byte-identical 3775-key dump, writes to that store afterwards (which fails
if the edit's file numbers collide with what the next writer mints), and
reads a store mcme compacted again on top of those writes. The C selftest
pins the view-identity property, tombstone survival, repeated compaction,
post-compaction writes, and the one-table-no-log shape, all through reopen.
