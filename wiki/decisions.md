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

## D12 — Convert drops what it cannot carry, and says so; it never half-writes an output world

`mcme convert` (Phase 4, bedrock→java/1.20 first) faces a world that holds
far more than blocks: entity lists, block entities, tick lists, waterlogging
layers, subchunks in payload versions this build does not decode, other
dimensions, and a keyspace of bookkeeping. Three policies, decided once:

**Report, do not fail.** Anything the conversion cannot carry is dropped,
counted in the report, and the run succeeds. A world full of chests converts;
the summary line says how many were left behind. Refusing instead would make
convert useless on every real world until every subsystem exists, and the
house has never shipped a converter that lies by silently dropping — the
report is the honesty, and each count is a named next entry.

**The block map follows the reference's own policy for unknowns.** Bedrock
states the map does not know (a furnace's `facing_direction`, a torch's
`torch_facing_direction`, farmland's `moisture`) take the Java block's default
state — exactly what PyMCTranslate does with the same inputs, so `convert` and
the Python reference agree block for block rather than mcme inventing a
mapping the reference does not have. States both editions share pass through
byte-for-byte; the rename cases (`pillar_axis`→`axis`, red sand) are in the
map, and the map grows per world, pinned by the reference tool each time.

**The output world is whole or absent.** `<out>` must not exist, so convert
never mixes into a directory someone else made, and a failure part-way
removes whatever it created before the error stands. A half-written world
directory is worse than no directory: the next `mcme info` on it would
report a world that lost chunks silently. (There is still no transaction
across region files *during* the run — but the run's failure is never
invisible, because nothing survives it.)

**Rejected.**
- *Opt-in `--drop` flag with strict default*: makes the common case
  (convert my world) refuse on the first chest, for a safety that the
  report already provides — the loss is named and counted, never silent.
- *Carrying block entities in this entry*: each entity type's Bedrock→Java
  NBT mapping is its own project; the report counts them until then.
- *Failing on unreadable subchunk payloads*: the pre-palette payloads are
  a known gap, not corruption; refusing would refuse whole era-spanning
  worlds for one missing decoder.

**Cost accepted.** Converted worlds lose entities, block entities, ticks,
waterlogging and non-overworld dimensions until later entries; every loss
is a count in the convert report and a line in the dev-log's next-up.

---

## D13 — Prune deletes whole chunks by column; reclaim stays with the compact verbs

`mcme chunk prune` deletes chunks — the unit the game saves — selected by
fill's box. Four policies, decided once:

**The box selects columns; Y is parsed and ignored.** Corners in any order,
X/Z floored to chunk coordinates by the rule every chunk verb speaks. A
chunk is all of its Y: "delete most of a chunk" is not a state any reader
keeps, so a box's Y only ever names columns, and the contract says so out
loud rather than pretending the box is three-dimensional.

**Bedrock: every key the chunk owns, batches of one log record, under the LOCK.** A chunk's
keys are the ones `mcme_bedrock_key_parse` names — subchunks in both
spellings, the version record, biome/heightmap, block entities, pending
ticks, every fixed-shape record addressed by the chunk's x/z and dimension —
tombstoned in batches of one log record each, sliced so a world-scale
prune stays under the log's ceiling (D10): every batch is whole or not
at all, and a failure part-way leaves whole batches landed — the same
honesty every multi-record write keeps. Keys that are not fixed-shape
chunk records — level metadata, actor
records, portallists — are never touched: mcme deletes what it can name,
and the tag set it will not guess at stays unguessed. The write takes the
exclusive flock the game takes.

**Java: the slot is zeroed; the bytes stay.** A zero location entry is the
format's own advertisement that nothing is stored here — the same bytes a
chunk-less slot already has — so the punch writes the slot's entry and
timestamp in place: no re-encode, no payload read. The payload bytes it
pointed at stay in the file until `region compact` rewrites the file
around the survivors, exactly as Bedrock's tombstones stay in the log
until `db compact`. Every region file the box touches is checked first,
like fill, so a file that will not read refuses the whole prune with
nothing punched.

**Reclaim is a separate verb.** Neither half shrinks anything by itself —
`db compact` and `region compact` already exist, are tested, and are what
the write verbs already point at. One verb, one truth.

**Rejected.**
- *Pruning by subchunk*: partial chunks are not a world state; the game
  reads and saves the chunk whole.
- *Radius or centre selection*: the box reuses fill's parser and
  `McmeBox`; a radius is sugar a caller can compute.
- *Deleting actor records with the chunk*: the actor tag is deliberately
  not in the guessed-at set (`bedrock_keys.h`); actor keys are the entity
  feature's to name, and a prune that guessed a tag number would be the
  first mcme write that guessed.
- *Compacting inside prune*: one verb, one truth — and a compaction
  doubles the write of a delete-heavy run that may fail part-way.

**Cost accepted.** Pruned space is not reclaimable until the paired
compact verb runs; actor records of pruned chunks remain until the entity
work; a Bedrock prune scans the store's whole keyspace (O(entries)) rather
than synthesising keys, because only the scan sees every companion record.

---

## D14 — Compacted tables carry bare-DEFLATE blocks when that helps

`db compact` writes the whole store as one table (D11). The blocks it
writes now go out compressed — bare DEFLATE, type 4, the codec Minecraft's
own fork writes — whenever deflate makes a block smaller than storing it
raw, and as type 0 when it does not. This supersedes D11's "Blocks are
uncompressed (type 0)" sentence; D11 stays as written.

**The trigger was measured, not theorised.** The same world, both ways: the
game holding its own compaction at ~130 files / 259 MB, mcme writing the
identical merged view as one 2.27 GB table — every block raw. An 8.7× churn
paid on each compact against a byte-level diff the goldens never asked for:
they diff merged views through the Python binding, never block bytes.

**One codec, chosen per block, announced by the trailer.** The reader has
always decoded type 4 — the fixtures were full of it before anything here
wrote it — and Amulet and the game read it natively, so the reader and the
reference battery needed no change at all. The CRC32C still covers the
stored bytes plus the type byte (D9's on-disk rule): compressing changed
what sits under the checksum, not what the checksum promises. Raw remains
the honest fallback for blocks deflate cannot shrink — the empty
metaindex, a restart-only block, incompressible payloads.

**Rejected.**
- *Raw blocks for stock google/leveldb compatibility*: no stock LevelDB
  reads Bedrock worlds, and the three readers this store has — mcme, the
  game, Amulet — all decode type 4. Compatibility with a reader that will
  never open the file is not compatibility.
- *zlib-wrapped or LZ4 blocks*: the game's blocks are bare DEFLATE; using
  its codec means every reader here decodes with no sniffing and no guess.
- *Raising the block target to the game's ~100 KiB at the same time*: the
  game's bigger blocks hold ~1.8× tighter (259 MB vs 460 MB on the same
  world), but block size is its own dial — write granularity, read
  amplification, restart density — and D14 changes the codec, not the
  shape of the table.

**Cost accepted.** A compacted store can no longer be described
byte-for-byte as raw block bytes, and stock LevelDB tooling refuses what it
cannot inflate — a cost paid only by tools that could never read these
worlds anyway. Compacting the 2.2 GB world went 36s → 1m35s: deflate over
every payload, once per compact.

**Reverses if:** a real workflow needs mcme's compacted output opened by
stock google/leveldb, or the per-block deflate cost shows up in editing
loops rather than the rare compact.

---

## D15 — Convert carries waterlogging and the ten container types, as two writers spell them

Before D15, `mcme convert` moved blocks and left behind everything that
lives in them: layer-one water was counted and dropped, and every
block-entity document was counted and dropped — the pre-D15 run over one
real world counted 23,102 documents and carried none, so the Java world
arrived with no containers at all. D15 carries the two loudest drops (the
Next D14 pointed at), under rules read off the world rather than the wiki.

**Waterlogging.** A block with `minecraft:water` on layer 1 over a block
on layer 0 becomes Java's `waterlogged: "true"`, and only where the Java
name actually has that property — the translate map answers, nothing is
guessed. Layer-one water that is not waterlogging anything counts as
`water_positions` and never fails (1,416,949 positions in one real
world; the fixture pins 3,280).

**Block entities.** A document carries iff three things hold: the id is
one of ten container types (chest, trapped chest, ender chest, barrel,
hopper, furnace, blast furnace, smoker, dispenser, dropper), x/y/z are
integers, and the world has already filled it. A `LootTable` is a
container whose contents are a path mcme does not read (4,006 of 7,749
container-id documents in one real world): carrying it would write an
empty chest where the world would grant items — the half-answer the
mapping refuses. What carries: Java's id, the block's coordinates, the
stacks under the rule below, a nonempty `CustomName`, `Lock`, the
smelter timers as ints. What stays behind: `keepPacked`, the pair
coordinates double chests record, and anything else the game adds. The
report counts whole documents and whole stacks, never a field.

**Three spellings in, one out.** The game writes its savegame ids short
(`Chest`, `BlastFurnace` — a shape shared by all 43 distinct document
ids measured in one real world) while the frozen golden fixture was
hand-authored `minecraft:chest`; neither spelling alone survives both
inputs, so both carry and the answer is always Java's namespaced id.
Stacks are the same bargain: `Name`/`Count`/`Damage` as the game writes
them and `id`/`count`/`damage` as the fixture holds them are the same
stack out under Java's `Slot`/`Count`.

**The stack rule.** A stack needs Slot plus Name-or-id plus Count-or-
count; nonzero Damage becomes Java's `tag{Damage:int}`. What the game
writes *beside* a stack's identity is bookkeeping that drops without
dropping the stack: `WasPickedUp`'s byte, the `Block` compound a block
item repeats about itself (measured: all 5,455 in one real world are
exactly name/states/version), a `tag` holding exactly `{Damage: 0}` —
the undamaged marker. Any other field beside a stack is item data mcme
has no map for (enchants, display names, brew state, nested `Items`):
the stack drops out of the list entirely and `skipped.items` counts it
(711 stacks in one real world). No field travels on hope.

**The record's strings decode both editions' UTF-8.** Finding the
spellings above in a real world turned up a third one, in the decoder
underneath the record walk: it accepted only modified UTF-8, so one
emoji in one item name refused the whole record — measured, three
records, 621 documents behind `display.Name = "🦴"`, 480 of them
carryable containers. Bedrock's C++ writer emits standard UTF-8
four-byte sequences above the BMP; Java's writer emits modified UTF-8
surrogate pairs. `mcme_mutf8_decode` now accepts each encoding's own
spellings and rejects only what neither can write: overlong forms,
continuation bytes standing as leads, code points past U+10FFFF.

**Rejected.**
- *Carrying every id mcme can name* — signs, frames, spawners,
  shulkers, lecterns, the rest of the 43: Java spells each type
  differently and a partial map writes wrong worlds. The ten are
  the ones whose fields map today; the rest count, never fail.
- *Keeping the unknown stack fields*: carrying them verbatim would
  hand the reference's translation job back to a table mcme does
  not have, and dropping them silently would lie about what
  arrived. The stack drops whole and is counted.
- *Carrying loot chests empty*: see above — the count is honest,
  the empty chest would not be.
- *Tolerant strings (U+FFFD) for the binary blobs 16 records hold
  in a string field*: a decoder that invents text is worse than
  one that says it cannot read. Those records refuse and count.

**Cost accepted.** One seam in the count unit: an unreadable record
counts once, not per document — 16 records holding 102 documents in
one real world, 45 of them containers, invisible behind the unit.
Documents in chunks mcme writes no Java chunk for count rather than
carry (231 in one world). And D15 is the read path: `put` still
requires a namespaced id while the game writes short ones — whether
the game loads mcme's spelling is untested.

**Reverses if:** a world's id census outgrows the ten (measure the
ids again, extend the table), or the dropped-stack count grows past
the point where its fields deserve mappings one at a time (measure
what those stacks hold first).

## D16 — `put` takes an id in either spelling the game has, and stores it as given

The write side had one rule where the read side had two. D15's
closing note kept it deliberately: `put` required a colon in the
`id` because the premise then standing was that the reference's
decoder — and the game — drop a document whose id names no
namespace. The measurements say the premise is backwards. One
real store holds 23,806 documents across 43 distinct ids, every
one of them bare: `Chest`, `TrappedChest`, `BrushableBlock`, no
colon anywhere. The wiki defines the field as "the savegame ID of
the block entity". The game binary's own strings hold the
registry literals bare (`Chest`, `Barrel`, `Hopper`,
`BlastFurnace`…) and no bare `minecraft:chest`. The namespaced
spelling is one mcme itself wrote; the refuser was refusing the
game's own format.

`mcme_bedrock_put_block_entity` now refuses an id in *neither*
spelling instead of an id without a colon: shape, not list — no
id, an empty side of the colon, a second colon, or a character
an id in neither form holds (letters, digits, `_`, `-`, `.`) is
USAGE before any byte is written. What arrives is what is
stored: `Chest` in, `Chest` out; `minecraft:chest` in,
`minecraft:chest` out — the same rule as every other field mcme
writes, and D15's read path takes either back.

**Rejected.**
- *Normalizing the input to the game's spelling*: the mapping is
  registry, not prefix — strip `minecraft:` and `chest` is still
  not `Chest` — so normalization would guess, and the write path
  must not lie about what it was given.
- *Checking the id against the 43-id census*: one world's ids
  are a floor, not the registry; refusing a document because a
  measured subset lacks its id is D15's half-answer wearing a
  different hat.
- *Keeping the namespaced-only rule*: it refuses what every
  document in a real store is.

**Cost accepted.** mcme can refuse a malformed id but not a
wrong one: `{id: "chest"}` now stores, and whether the game's
lookup keeps it — or mcme's own namespaced writes, for that
matter — is the same untested question D15 named: which
spellings the game holds on load can only be measured by
loading a world that holds both.

**Reverses if:** that load test proves a spelling dropped — then
the rule becomes write the spelling the game keeps, the census
question returns with a real registry behind it, and mcme's
earlier namespaced writes become a repair job.

**Measured, same day.** The load test ran and nothing dropped.
Three full chests — a control (`Chest`), one written
`minecraft:chest`, one `chest` — loaded in the game (1.26.52.3),
about five minutes played, store read back: all three documents
survived as working containers with their items, and all three
ids came back `Chest`. The game accepts either spelling on load
and rewrites the id to its own spelling at the next save; its
re-serialization added `BlockEntityVersion: 0` and dropped
`isMovable` on every chest, the control included, so that touch-up
is the game's format, not the id's fate. The reversal clause does
not fire: the rule stands, and mcme's earlier namespaced writes
need no repair — the game canonicalises them itself.

## D17 — The block map learns the names real worlds hold, from the reference that knows them

Before D17 `mcme_translate_block` passed block names through unchanged, and
the pass-through was wrong: real worlds hold Bedrock names Java 1.20 does
not have. The full census of one real store — every subchunk payload in
all three dimensions (392,032 subchunks, 0 decode failures) plus every
tick record — counted 828 distinct names and 4,011 distinct (name,
states) combinations, and among them `flowing_water`, `magma`,
`snow_layer`, `double_plant`, `log`/`log2`, `bed`, coral families: names
the map handed to Java verbatim, so convert wrote Java worlds the Java
chunk parser cannot read.

**The table.** The name half is now one table between the GENERATED NAME
MAP markers in translate.c — 309 rows in three shapes: 132 wholesale
renames (a name changes under any states), 177 value rules (one state
value decides: `log`+spruce, `sand`+red, `torch`+west), and 33
two-condition rows where no single value decides (a dead coral's colour
times `dead_bit`, a cauldron's liquid times a nonzero `fill_level`).
Lookup: binary search to the name, rows most-specific first, the first
row whose conditions all hold wins, else the name's default row, else
the name unchanged — the reference's own answer for a block it does not
rename.

**Generated, then checked, then pinned.** Rows are enumerated against
PyMCTranslate at the versions ref_convert pins (Bedrock 1.20.80 → Java
1.20.0): every state product up to 64 combinations for names the spec
table knows, and the world's own combinations for the 163 names the spec
table has never heard of but the translation answers anyway (the two
tables are separate — `get_specification` raising says nothing about
whether the reference can translate). The learner takes the smallest set
of one- and two-condition rows that reproduces every answer; before the
table was written, all 4,011 real combinations and 3,753 product
combinations were checked against the reference: 0 mismatches, 0
combinations no flat rule could carry. The pinning stays: ref_convert
reads the markers back out of translate.c on every battery run and
re-answers every row against the reference, and refuses a table not
sorted in the byte order the lookup's binary search walks.

**Why the reference decides.** The map is not a wiki transcription:
Amulet converts with PyMCTranslate, so mcme's answers must match the
answers the reference gives for the same entry, including where the
reference keeps a name mcme's judgement would change (parity, D12). Two
reference facts the enumeration proved: it answers tag-type-sensitively —
`Byte` 10 and `Int` 10 for `huge_mushroom_bits` are different answers,
and the pin must ask with the spec's own spelling — and for a name it
does not know at all it keeps the name, which is what the table does
too: unknown names carry no rows and the residue is names, not rules.

**Cost accepted — names only.** The state-width gap does not shrink:
mcme still drops every state except the kept handful, so a renamed water
block arrives as `minecraft:water` with no `level` — the flooding risk
D15 named, now with the *name* right. The reference's own
`liquid_depth` → `level` keeps is a map decision for when the state map
grows; D17 fixes spellings, not states.

**Rejected.**
- *Hand-curating the renames the census showed*: the census names a
  floor — the next world holds the next name — and a hand table rots
  against a reference that is the actual acceptance test.
- *Renaming everything at a newer Bedrock bank*: the pin is
  (1.20.80 → 1.20.0) because that is the pair translate.h documents and
  ref_convert compares against; moving the bank moves every answer in
  the battery at once and belongs to a version-pair decision of its own.
- *Inventing Java-side renames the reference does not answer*: the table
  records the reference's answers, not Java's ideal.

**Reverses if:** the battery's position-by-position comparison starts
diverging from the reference on fixture blocks — then the enumeration,
not the reference, is wrong, and ref_convert's first failing row names
it.

## D18 — Convert carries the tick records, split the way Java spells its lists

Before D18 every pending-tick record was a `chunk_data` skip: the key was
counted, the entries vanished. The census of one real store counted 2,235
tick records across three dimensions — 1,623 of tag 0x33 and 377 of 0x3a
in the overworld alone — and with them every scheduled update the world
set: water and lava with their times (including entries over 31 million
ticks overdue, `t` far negative), fire spread scheduled seconds ahead. A
converted world that loses those is a different world at the next tick.

**Carried, through the same map.** A tick's `blockState` is shaped
exactly like a subchunk palette entry — name, states, version — so
D17's table answers it and mcme carries what it would have written as a
block, with no second set of rules. Both tags are read for the chunk
(0x33 first, then 0x3a — the scan's sort ties on the tag, so list order
is the records' order) and merged into the chunk's two Java lists.

**Two lists by translated content.** Java spells fluid ticks apart:
after the map answers, `minecraft:water` and `minecraft:lava` land in
`fluid_ticks`, every other block in `block_ticks`. The split reads the
*translated* name — the map is the single answer to "what block is
this", and a source-side test (`flowing_water` contains "water") would
disagree with it the moment the table renames.

**The entry Java spells.** `{i, p, t, x, y, z}`: `i` is the map's name
(Java's tick entry carries no properties — the block at x/y/z holds
them), `p` is 0 because Bedrock's record carries no field mcme may map
to a priority and inventing one would reorder simultaneous ticks,
`t = time − currentTick` as an Int, and the coordinates verbatim. A
negative `t` is Java's own spelling of overdue, which the measured
world needs: entries exist at −31,149,718.

**Both keys stand in every chunk.** `fluid_ticks` and `block_ticks` are
written always, empty where no entry lands — a shape fact, like
`block_entities`, never a conditional one.

**Counts, never failures (D12).** `ticks_carried` at the top level;
`skipped.ticks` for what does not arrive: an entry missing x/y/z/time,
a `time − currentTick` that leaves int32, a blockState the map refuses
(each counted once, whole or not at all), a record that will not
decode (counted once — half a record is not a count), and a record
whose chunk holds no subchunks, whose entries count instead of carry —
the D15 orphan rule, extended. Tick keys of other dimensions stay
`other`: one rule for every tag, D15's dimension rule untouched.

**Pinned where the reference is silent.** Amulet reads no tick
records — PyMCTranslate is never asked one — so there is no reference
answer to mirror; the silence is a gap in the tool, not an answer to
match. ref_convert therefore states the spec itself: it fabricates
four records on the fixture copy (a scheduled record whose entries
must split through the same pinned reference, an entry with no z, an
orphan record, a payload that is not NBT), recomputes both lists from
them, and pins all seven counts in its exact report check. The C
selftest fabricates the same four shapes on its own copy.

**Rejected.**
- *Counting them like every other uncarried key*: nothing in the
  entries needs more than D17 already provides, and D12's counts are
  for what mcme cannot carry — this is what it can.
- *Splitting on the source name*: two places deciding "is this a
  fluid" is two places to disagree, and the map is the one that has
  to be right anyway.
- *Carrying only 0x33, the canonical pending-tick tag*: measured, the
  0x3a records hold fire entries with explicit future times —
  scheduled updates under another tag — so dropping 0x3a drops every
  fire spread in the world.
- *A priority ordering in `p`*: Bedrock records none, and a made-up
  order changes when blocks tick relative to each other — a lie
  about time, the one thing a tick list is for.

**Cost accepted.** The 92 names D17 leaves untranslated reach the tick
lists with the same spelling (parity, D17's cost, now scheduled);
coordinates are carried verbatim, so an entry naming a block outside
its own chunk arrives where the record said it, and mcme does not
audit chunk-locality.

**Reverses if:** a load test of a converted world shows Java refusing
a tick list mcme wrote — a field shape or a `t` the game rejects —
then the spec narrows to what the game takes and ref_convert's mirror
narrows with it.

## D19 — Convert carries the 3D biome cells, each name the reference's own answer

Before D19 every Data3D record — tag `0x2b`, 512 heightmap bytes
followed by the per-stratum biome arrays, 36,848 of them in the
measured store — was a `chunk_data` skip: the key counted once and
every cell behind it vanished. 21,799,232 cells, 54 distinct ids, and
a converted world every biome reader would answer plains across
terrain that never said so.

**Read once, in the game's own words.** What follows the heightmap
is a run of arrays, one per stratum from the world floor (−4 in the
overworld — measured, the arrays run floor to top stored stratum
without a gap). Each array's first byte is a signed header: shifted
right one it is the word width (the low bit is amulet's palette
flag — ignored exactly as amulet ignores it), and a byte at or over
0x80 (observed 0xFF) is padding, walked off a byte at a time, which
is the reference's skip too. Width 0 is one u32 id for all 4,096
positions; widths 1..16 are Tomcc's packed words — 4,096 u32
little-endian values, LSB-first — then a u32 palette length and i32
ids. Cells are read the way both decoders read them: every fourth
block, storage index `(x*4)<<8 | (z*4)<<4 | (y*4)`, handed back in
Java's container order `y*16 + z*4 + x`. Widths over 16, 24 arrays
or more, an empty palette, an index past the palette, a truncated
payload — the whole record refuses and counts once into `biomes`;
half a record is not a count. Measured widths: 0..3.

**The map answers an id the way the reference answers it.** 86 rows
in `translate.c` between the generated-map markers, sorted by id
(the lookup binary searches), each row's Java name produced by the
reference's own two legs — bedrock `unpack`, `to_universal`,
`from_universal` — the same pipeline D17 named blocks by. An id the
table does not hold (measured: 193, 194, 195 — 75,424 cells) lands
on `minecraft:plains`, the reference's own fallback and its own
warning, and the cell counts: `biomes_carried` and `biomes` are
per-cell and mutually exclusive.

**The section speaks only where the store spoke.** An array at the
stratum puts a `biomes` container on that section: palette the
distinct resolved names sorted by name, `data` only when there is
more than one, bits = ceil(log2(palette)) at least 1, floor(64/bits)
values to a long, none spanning two. No array at the stratum puts
no key at all: the game reads absent as plains — the reference's
own answer — so D15's placeholder container is deleted; it said the
same thing in more bytes. The section set stays mcme's pinned
non-air one, which costs the cells at pure-air strata (measured:
27,264, 0.13%): counted, nothing written.

**Counts, never failures (D12).** `biomes_carried` at the top level
for cells the map named; `skipped.biomes` for what does not arrive —
fallback cells, cells with no section, one per refused record, an
orphan record's cells (a record whose chunk holds no subchunks
counts instead of carries — D15's orphan rule extended; LevelDB
keys are unique, so a chunk's record is one-to-one with its key).
The drops line grows `biome cells`; `chunk_data` loses the `0x2b`
records wholesale (measured −36,848), the 0x2d-era payload records
still counted there.

**Pinned on the reference itself.** The reference reads this record
— amulet's `_decode_height_3d_biomes` and `_decode_packed_array` —
so ref_convert mirrors nothing: it decodes the fabricated records
with the reference's own decoder, runs each id through the live
translation pipeline, and lays the cells out in Java's container
order. All 256 fabricated cells are compared positionally against
mcme's containers (content is the reference's; palette order is
mcme's), the fabricated mixed array marking one cell on each axis
so a sampling slip in either decoder shows as a wrong cell. The
table re-pins every run — markers present, ids sorted, the id set
exactly the ones Bedrock's tables name, each row's answer the live
reference's — and both reports stay exact (255/258 on the fixture;
the entities fixture 0/0).

**Rejected.**
- *Refusing cells the map does not know*: the reference carries
  them as plains, so refusing would drop what the reference itself
  converts — the count is mcme's half of the same admission.
- *A sampling other than every fourth block*: the game's storage
  and the reference's decode both step stride 4; any other sample
  is a different biome than either shows.
- *Extending the packed form past 16 bits, or a second packing
  form*: both decoders cap at the signed header's width and the
  measured world tops out at 3 — a wider spec is unmeasured
  invention.
- *Keeping the placeholder so every section carries a key*: two
  spellings of one fact (plains), and the absent key is the one
  the game already treats as the default.

**Cost accepted.** 75,424 cells reach plains — the reference's own
fallback for three ids this bank has never heard of; 27,264 cells
count instead of carry, standing where mcme writes no section; the
heightmap half rides decoded-past and uncounted — it is not a skip,
it is the heightmap drop's cargo; and the palette's internal order
is mcme's own, name-sorted, because the mirror pins content and a
sorted palette is the one order nothing downstream can argue with.

**Reverses if:** a load test of a converted world shows Java
refusing a biome container mcme wrote — a bits count, a palette
placement, or a key where the game expects none — then the shape
narrows to what the game takes and ref_convert's mirror narrows
with it.

## D20 — Convert carries the heightmap: the record's own columns, under `MOTION_BLOCKING`

**What crosses.** The chunk's Data3D record opens with its other half:
512 bytes, 256 little-endian int16 column heights. Java wants the
same columns under a `Heightmaps` compound — 9 bits to a value,
7 values to a long, none spanning two (37 longs, the 1.16+ shape),
the value counting blocks above the world's bottom. The measured
store answers that spec with no arithmetic at all: its columns read
10..309, inside the 0..384 the overworld's height spells, and each
value is the first air above the floor — mcme's own `chunk height`
verb and the record agree column for column (y112 moss under value
177; a poppy at y63 over the y62 ground that value 127 counts).
Only the packing changes; the order is the record's own (x + 16z =
Java's index), so nothing moves.

**Which name.** One name: `MOTION_BLOCKING`, and the semantics were
measured, not assumed — 2,560 columns across ten chunks of one real
store, blocks read through the reference, record values beside them.
2,188 columns agree with top + 65 exactly; the 372 that differ all
have something that does not block motion on top (short grass, vines,
ferns, torches, flowers, snow layers — 23 names) or top one of 29
columns the game's own bookkeeping still counts below (crimson
stairs, trapdoors, a fence, a chain, a path), and every differing
value points at a real block below — none at nothing. Water, ice and
leaves tops never differ: fluids and leaves count. That is
`MOTION_BLOCKING` and none of its fellows — `WORLD_SURFACE` would
count the grass (measured out), `MOTION_BLOCKING_NO_LEAVES` would
drop the canopy (measured in), `OCEAN_FLOOR` would read the sea
floor where the store's dominant value, 127 on 4,475,483 columns,
sits at y62 — sea level — and the `_WG` pair is worldgen-era. The
29 exotic columns carry the store's own bookkeeping unchanged: the
same stance ticks and biomes take, the store's words.

**Counts (D12).** Root `heightmaps_carried`, one per map that
crosses; `skipped.heightmaps` one per map that does not: a converted
chunk whose store holds no record, a record that will not decode (its
map is lost with the record, counted beside its cells), a column
outside the world's height (that half refused, the cells beside it
still cross), an orphan record's map (counted where its cells
count). A tombstone counts neither half. The real store: 36,848 + 1
= 36,849, its converted chunks exactly.

**Absent when the store is.** No map crossed → no `Heightmaps` key —
D19's "no array → no key" rule applied to the record's other half,
the store's own shape, and absent is the game's answer to compute.

**Pinned on the reference itself.** The reference writes nothing to
pin against: amulet's Bedrock reader parks the columns in
`misc["height"]`, its Java writer packs `misc["height_mapC"]`, and
nothing bridges the two — the golden java fixture's ten chunks each
hold an empty `Heightmaps` compound. So, as in D18, mcme pins its
own spec: ref_convert packs the fabricated columns through its own
implementation and compares all 37 longs against the output's
`MOTION_BLOCKING`; the selftest pins word 0's seven values nine bits
apart, word 1's wrap, word 36's four, and the formula over every
column. The fabricated columns spell their own coordinates (column i
reads 100 + i), so an order slip in either packing shows as a wrong
number. Both reports stay exact: 1 carried / 3 counted on the
fixture (the bad map, the orphan, the junk record) and 0 / 2 on the
entities fixture (both chunks record-less).

**Rejected.**
- *Computing the map from the payloads mcme already reads*: it would
  mean inventing Java's motion-blocking set — which the game's own
  columns show is not quite any documented one — where the store
  already answers.
- *Writing all six names with the one value*: `OCEAN_FLOOR` over the
  store's sea-surface columns would be a lie the record itself
  contradicts.
- *The empty compound, beside the reference's shape*: two spellings
  of nothing, and absent is the one the store speaks.
- *Clamping a column outside the world, or widening the value's
  range*: a column the world's height cannot spell is a record that
  does not answer the world it claims; that half counts, the cells
  beside it still cross.

**Cost accepted.** mcme's editing verbs never write a heightmap —
`chunk height` derives its answer from payloads for the same reason —
so a store mcme has edited carries the map the game last wrote for
those chunks: stale by design, like every other record convert
faithfully translates. 29 columns ride the game's own bookkeeping
unexamined; one map counts on the real store, the chunk whose record
never existed.

**Reverses if:** a load test shows Java wanting a second map mcme
can honestly spell, or refusing `MOTION_BLOCKING` standing alone —
the compound narrows or widens to what the game takes, and the pin
follows.

## D21 — Convert carries the nether and the end: keys, floors and directories the game's own

**What crosses.** The game's three dimensions keep every chunk key
the same way: x, z, the dimension's own number, the tag, the section
index — the dimension spoken at offset 8 when it is not overworld
and omitted when it is (len 13/14 against 9/10, the two spellings
`mcme_bedrock_key_parse` has always taken). Each dimension's chunks
are written where Java reads them: `region/`, `DIM-1/region/`,
`DIM1/region/` — the layout the editing verbs' `--dimension`
documents name, through the same function
(`mcme_java_dimension_dir`), so the tool's two halves cannot drift
on where a dimension lives. Nothing inside the chunks changes: the
payloads, records, documents, ticks and maps convert through
D15–D20's rules untouched; what they gain is a floor and a height
of their own. The section list stands on the dimension's floor —
overworld −64 → yPos −4, nether and end 0 → yPos 0 — the biome
strata count from that floor, and a heightmap's columns must sit
inside that dimension's own height: 384 overworld, 256 nether and
end (D20's `heightmap_fits`, asked per dimension — the fixture's
nether record spells columns 0..255 and crosses; one column at 300
sits inside the overworld's 384 and past the nether's 256, so that
map counts while its cells beside it still cross). No map, no key;
no record, no containers — D19's shape rules, unchanged.

**Dimensions held apart.** The sort's first key is the dimension:
(dim, cx, cz, cy). The store's own coordinates collide happily —
the fixture's end chunk sits at the overworld's own (1,−2), and
both fixtures hold a nether (0,0) beside an overworld (0,0) — and a
group walk that did not read the dimension would merge three chunks
into one and write whichever payload sorted first. Pairing keeps
the same law: a block-entity, tick or biome record belongs to its
own dimension's chunk — the nether record at (0,0) never reaches
the overworld chunk at the same coordinates, the overworld's lists
stay empty beside the nether's full ones. The output directory is
made only for a dimension that has chunks: a store with no end
chunks leaves no `DIM1` at all, the absent-is-the-answer rule the
containers keep, extended to a whole dimension.

**Counts (D12).** No new report fields: `chunks`, `subchunks`,
`entities` and `chunk_data` stop being overworld-only, and
`skipped.other` keeps exactly the keys with nowhere to go — the
unparseable lengths, and now the dimension numbers beyond the
game's three (any dword outside 0/1/2), counted like level
metadata instead of written to a path Java would never open. The
real store, release, rc=0: `other` 885,435 → 780,267 (780,266
unparseable + 1 dimension beyond the three); `chunks` 36,849 →
46,291; `subchunks` 340,599 → 392,032 (+31,938 nether, +19,495
end); `blocks` 1,395,093,504 → 1,605,763,072; `entities` 10,745 →
15,501; `chunk_data` 180,969 → 219,867. The records answer their
own halves: `heightmaps_carried` 36,848 → 46,290 — one per new
chunk, every nether and end record whose chunk converts has its
map inside its own height, none refused — and the 236 records whose
chunks hold no subchunks count their maps beside the one that
already did (`skipped.heightmaps` 1 → 237); `ticks_carried` 74,361
→ 107,855, every entry of the 235 nether and end records arriving
(`skipped.ticks` stays 0); `biomes_carried` 21,696,544 →
24,977,632 with `skipped.biomes` 102,688 → 628,064 — the same
rules, the store's own strata and orphans deciding each cell;
`block_entities_carried` 4,023 → 4,194 and `skipped.block_entities`
20,723 → 21,214 — the 168 records hold 662 documents, D15's rules
sending 171 across and 491 to the count (`items` 842 → 896).
`water_positions` (1,455,655) and `legacy_subchunks` (0) do not
move: the other dimensions' payloads hold no second layer and no
pre-palette versions.

**Pinned.** The fixture supplies one nether key; the selftest
fabricates the rest on its copy, byte for byte what ref_convert
puts on its own: a nether record whose columns spell their own
coordinates with two strata from the nether's floor, subchunk
copies at nether (1,−2) and end (1,−2) — three dimensions one sort
away from merging — the column-300 record (refused under 256,
crossing under 384 — the pin on the per-dimension height), an
orphan, tick records on a converting and an orphan nether chunk,
two chest documents through mcme's own write API (one carried, one
counted), and the three keys with nowhere to go. The report is
exact: 5 chunks, 10 subchunks, 36,864 blocks, 1 `other`,
block entities 1 carried / 1 counted, 1 entity list, 1 chunk
version, ticks 4/5, biomes 383/386, heightmaps 2/6, legacy 1,
water 3,280 — `DIM-1/region/r.0.0.mca`, `DIM-1/region/r.0.-1.mca`
and `DIM1/region/r.0.-1.mca` present with the parity cells position
for position, overworld yPos −4 beside nether and end yPos 0, the
nether's tick and document in the nether's lists and the
overworld's empty at the same coordinates, and the carry test's
store — no nether, no end — leaving no `DIM-1` and no `DIM1`.
ref_convert counts the same world through its own mirrors, now
asking each dimension for its records from its own floor: 12
checks (was 11), the twelfth the nether document read back in
DIM-1 against the fixture's record.

**Rejected.**
- *Re-zeroing the other dimensions' y onto the overworld's floor*:
  the store's coordinates are the game's own; the floor belongs to
  the dimension's section list, not to a translation of every cell
  — and a heightmap re-zeroed stops answering the world it claims.
- *Keeping the old rule — len 9/10 only, dims 1 and 2 as `other`*:
  105,168 keys the game has words for, a nether that converts as a
  hole, and `other` naming data the report can no longer honestly
  call unreachable.
- *A directory for every dimension number a key mentions*: a
  garbage dword would write a path Java never opens; the number
  beyond the three counts, like every other key with nowhere to go.
- *Probing which dimensions have chunks to decide the layout*: the
  mapping is the game's fixed three — probing would let a stray key
  invent a directory the game has no name for.

**Cost accepted.** The report stays one set of totals across
dimensions: a question asked of the nether alone is answered by
rerunning the store the dimension names. Three dimensions are the
game's own set; a fourth arrives as `other` until a decision names
its directory, floor and height together.

**Reverses if:** a load test shows Java reading these dimensions
somewhere else, or the game grows a fourth whose layout is measured —
the fixed three become one more row in the table, and the pin
follows the directory, the floor and the height together.

## D22 — Convert carries the block entities D15 left: every family the reference answers for

**What crosses.** D15 stopped at the containers: ten ids whose Java
spelling mcme knew, whose documents were inventory plus three field
names. Everything else dropped and counted by default — not because
the mapping said no, but because mcme had not asked. D22 asks, family
by family, each answer measured through the reference on the real
store: Java's identifier from PyMCTranslate's own specification
(`nbt_identifier`), the fields from the reference's bedrock→java
translation of every document the store holds. The table now has
twenty-one ids, and what travels beside id and x/y/z is each family's
own Java spelling:

- *the shulker box* joins the containers — D15's fields unchanged;
- *the brewing stand*: Items, CustomName, Lock, and its progress
  under Java's `BrewTime` — the game spells the same clock
  `CookTime`, the furnace's name for a different question — with
  `FuelAmount` as `Fuel`; `FuelTotal` is the game's bookkeeping of
  what a fill started with and stays;
- *the mob spawner*: the seven ranges under names both editions
  spell alike (`Delay`, `MaxNearbyEntities`, `MaxSpawnDelay`,
  `MinSpawnDelay`, `RequiredPlayerRange`, `SpawnCount`,
  `SpawnRange` — identical on every spawner the store holds), and
  one `SpawnData{id}` whose id is the document's
  `EntityIdentifier`; `SpawnPotentials` is Java's own default and
  the display fields describe a rendering Java computes itself;
- *signs, plain and hanging*: `FrontText`/`BackText` → `front_text`
  and `back_text` — the text split into Java's four messages, each
  its JSON string, the trailing newline ending the last line rather
  than opening an empty one, fewer than four lines padded the way
  Java's own translator pads a missing line (`\"\"`), `color: black`
  and `has_glowing_text: 0` beside them, and `is_waxed`. The black
  is measured, not chosen: the int the game stores for the color
  takes no path through the universal format the Java mapping
  reads, so every sign's answer — the reference's and now mcme's —
  is black. A document from before the two-sided format (a
  top-level `Text`) carries with both sides empty: that text has no
  measured Java spelling, and an unmapped field drops like any
  other;
- *the comparator's* `OutputSignal`; *the beacon's* `primary` and
  `secondary` as `Primary`/`Secondary` — the pair where both names
  are visibly the same word — with Java's own values filling the
  rest;
- *the end gateway*: `Age`, and `ExitPortal`'s three coordinates as
  Java's `X`/`Y`/`Z` compound — the position check the game has
  already made (`EndGatewayBadPosChecked`) is not the portal;
- *the campfire*: `ItemTime1..4` → `CookingTimes`, Java's int array
  of four (every campfire in the store spells all four); the cook
  totals and the items have no counterpart and Java's defaults say
  what the absent fields said;
- *the jukebox's* `RecordItem` through the stack rule without a
  `Slot` — a record stands alone, and it is the one stack in the
  store written that way;
- *the chiseled bookshelf*: `Items`, and `LastInteractedSlot` as
  `last_interacted_slot`;
- *the empty tier* — sculk sensor, catalyst and shrieker, lectern,
  skull, bell, enchanting table, daylight detector, end portal —
  id and coordinates alone: measured per type, this world's
  documents hold nothing Java's block entity spells (the skull's
  rotation and type, the bell's ring state, the listener's
  selector all stay behind, as do the books and souls the store
  never held).

D15's two gates stand unchanged: x/y/z must name a block, and a
document the world has not filled — one carrying a `LootTable` —
drops entirely.

**The count keeps the rest, by name.** The store's census over
every remaining document: `Bed` 914 and `Banner` 247 — the pair
whose color lives in the document while the name lives in the
block, so carrying either alone would write a colorless bed or a
one-color banner; the block-side drop that pairs with them is a
decision of its own. `PistonArm` 2,440, `SporeBlossom` 2,028,
`Cauldron` 567, `FlowerPot` 256, `Music` 20, `Lodestone` 1 — Java
has no block entity for these; the mapping is the block's.
`ItemFrame` 43 and `Beehive` 129 — documents of entities, D23's
half of the store. `BrushableBlock` 748 and `DecoratedPot` 1,074
with a LootTable, 24 more decorated pots with no measured Java
spelling for their fields. `Vault` 466, `TrialSpawner` 408,
`Crafter` 5, `CopperGolemStatue` 2, `PotentSulfurBlock` 57,
`CreakingHeart` 67 — blocks Java 1.20 does not know.

**Counts (D12).** No new report fields; three counts move. The real
store, release, rc=0, 1m57s twice with byte-identical output trees:
`block_entities_carried` 4,194 → 11,261 (+7,067 — 4,376 empty-tier,
1,711 spawners, 263 campfires, 262 signs, 231 comparators, 102
shulker boxes, 98 brewing stands, 16 gateways, 4 jukeboxes, 3
bookshelves, 1 beacon), `skipped.block_entities` 21,214 → 14,147,
`skipped.items` 896 → 964 (the 68 new drops are stacks inside
newly carried families — stored content and enchantments the stack
rule will not half-write). Every other field is unmoved — chunks
46,291, subchunks 392,032, blocks 1,605,763,072, ticks 107,855,
biomes 24,977,632, heightmaps 46,290 — because D22 changes no key
and no chunk. The prediction was made before the run and matched
all three numbers exactly, through a model that first reproduces
D21's four pins to the document: 25,408 counted (4,194 + 21,214),
items 896 — including the store's 17 records mcme's whole-record
read refuses, their 169 documents counted as 17 (never half-read),
and the documents in chunks that never convert, which count as
dropped whatever their id says: no Java chunk means no Java list.

**Pinned.** The selftest fabricates fourteen family documents
through the game's own spelling — shulker box, brewing stand,
spawner, a modern two-sided sign, comparator, beacon, gateway,
campfire, jukebox, bookshelf, lectern, skull, sculk sensor,
hanging sign — beside four with no Java counterpart (bed, banner,
piston arm, vault): 20 carried, 6 counted, list length 20, `items`
still 1. The modern sign's front text holds a real newline that
SNBT's two escapes cannot spell, so the string is set after the
parse; its pin is the split — two lines, no third, three pads on
the hanging sign's one. The old-format sign sits at list index 2
now, both sides four escaped-empty messages, no `is_waxed`.
ref_convert grows its own mirror to match: CARRY_IDS 10 → 21 ids
with kinds, `want_java_doc` spelling each family's fields, `plain`
learning that an int array compares as the list it holds (the
campfire's `CookingTimes`); the store check fabricates a sign,
spawner, campfire, gateway, lectern, jukebox and a counted bed
beside the container (report 1 → 7 carried, 1 → 2 counted), the
entities fixture's old-format sign carries (4 carried, 1 counted),
12 checks — the count unchanged, the questions doubled.

**Rejected.**
- *Carrying bed and banner as documents*: the color is in the
  document, the name is in the block — carrying either alone
  writes a colorless bed or a banner of one color, and Java's
  block entity holds neither. The pair waits for the block-side
  drop that carries them together; until then both count, with
  their tables measured and waiting.
- *Writing `SpawnPotentials`*: the reference answers this store's
  every spawner with `SpawnData` alone; a potentials list would be
  a default mcme invented where Java already has one.
- *Translating the sign's color int into a hue*: no path through
  the universal format exists — the measured answer is black for
  every sign, and a hue table would be a guess the reference does
  not make.
- *Feeding the old top-level `Text` into a side*: no measured
  Java spelling exists for it; the text drops as an unmapped field
  and the document still carries its position.
- *Relaxing the LootTable refusal for the new families*: the rule
  is the mapping's, not the family's — brushable blocks and
  decorated pots are the holders the store proves, and a half-
  filled shulker would be the same unmapped items.

**Cost accepted.** Every carried sign writes four messages and two
sides even where the world held one line, and the color a player
set does not cross — black is the honest answer until a universal
path exists. 4,376 empty-tier documents now write entries Java
reads as data-less markers: position over nothing, which is what
this world's documents hold.

**Reverses if:** a load test shows Java reading any family's
fields differently, the game gives the empty-tier types fields
both editions spell, or the reference grows a path for the sign's
color — the table changes shape, and the pins follow the fields.

## D23 — Convert carries no entity and counts every one: the reference answers nothing, so nothing half-crosses

**What crosses: nothing, measured.** The question this entry existed to
ask — what does the reference do with bedrock entities on the way to
Java — has now been measured from both ends, and the answer is that it
does nothing, because there is nothing there to do. PyMCTranslate
ships no entity database anywhere: the installed 1.2.51 carries
`block.json.gz` and `meta.json.gz` and nothing else; the upstream
`Amulet-Team/PyMCTranslate` trees for `bedrock_1_20_80`,
`bedrock_1_16_20`, `java_1_20_0` and `java_1_16_0` each contain a
`block` directory and no `entity` directory; the `EntityTranslator` is
a deepcopy pass-through; `amulet.entity_support` is `False`; and
Amulet's Anvil writer (`anvil_2681._encode_entities`) pops bedrock's
`EntitiesDataVersion` and writes only the entity sets Java natively
spells — every other document is dropped on the floor. The reference
bedrock→java output therefore carries no entity documents at all, and
the `entities/*.mca` files the golden fixtures hold are amulet's empty
`{DataVersion}` layer stubs, not conversion output. mcme carries
nothing too: a pass-through document would be `identifier` plus a
float `Pos` in bedrock's spellings — an unloadable Java entity, which
is D12's half-write by another name. No `entities/` directory is
written, no CLI verb is added, and prune still never touches an actor
record (wiki/cli.md's prune section stayed true through this entry).

**The count tells both spellings exactly.** The store writes entities
twice over, and the report now counts each the way it stands:

- `skipped.entities` counts *documents* of the per-chunk entity
  lists — the 0x32 records, read through a new reader that is the
  block-entity reader with the list's own key (`mcme_bedrock_chunk_
  entities`). The D9 whole-record rule holds: every document decodes
  or the record counts once as unread; an empty record counts zero
  (empty is not one); a record the store does not hold counts
  nothing. Every record is walked after the chunk loop, together:
  nothing crosses for entities, so no record pairs with a chunk and
  there is no carry-versus-count branch to respect.
- `skipped.actors` counts *records* of the modern spelling — one per
  `actorprefix` key the store holds, whether the chunk's `digp`
  index still names it or the chunk behind it was pruned.
- the `digp` index keys count **nowhere**. An index key is not a
  document; its documents are the actor records, counted where they
  stand. Counting both would count one entity twice, counting only
  the pointer would count one the store does not hold — never
  twice, never zero — so the index is routed out of `skipped.other`
  and counts nothing itself.

Item stacks inside dropped entity documents do not touch
`skipped.items`: the whole document drops under its own count, D15's
rule, and the item count answers for records read as inventories.
D22's `ItemFrame` (43) and `Beehive` (129) documents stay where they
always were — in `block_entities`, counted as the 0x31 documents they
are; no count moves for them here.

**The keys named — D13's deferral, half-paid.** `bedrock_keys` now
spells both entity keys with predicates and their inverse builders,
shapes measured on the real store rather than guessed: `actorprefix`
plus the 8-byte id (19 bytes), and `digp` plus a chunk key's own
bytes — 12 with the overworld's dimension omitted, 16 with one, the
same omission rule every chunk key speaks. This is the naming D13
said the entity work would own: prune still holds no tag number for
them and never writes either key; deleting the actor records of
pruned chunks is a separate entry's question and stays deferred
(734,406 such records stand in the store as this entry ships).

**Census (current store, measured twice — mcme's own rules and an
independent walk, agreeing key for key).** 1,451,166 keys, no
tombstones. Entity lists: 15,540 records — 10,784 overworld (9-byte
keys) and 4,756 in the nether/end (13-byte, dimensions 1 and 2 only;
no key spells a zero dimension explicitly, so the coordinate-rebuild
the block entities read with has no second spelling to miss) — of
which 15,207 payloads are empty and 333 hold content, decoding to 777
documents in total, 0 unreadable. Actor records: 740,591.
Index records: 38,752 — 33,615 overworld, 5,137 with a dimension —
naming 6,381 ids of which 6,185 exist (196 name a record the store no
longer holds) and 734,406 actor records are named by no index at all:
the pruned-chunk set D13 left standing.

**The store moved under the record — stated, not papered over.** The
real-run numbers in D17–D22 describe an earlier snapshot of this same
world: those entries ran on a `/tmp` copy taken before the game's
own save of Oct 15 14:58, the machine's later reboot wiped that copy,
and today's copy — verified identical to the source store, 1,451,166
keys both — contains the save. The nether and end still match D21's
record exactly (31,938 and 19,495 subchunk keys); the overworld lost
6,745 subchunks (340,599 → 333,854), `chunk_data` 219,867 → 215,490,
unparseable keys −409, entity lists +39. So no cross-entry delta is
claimed for any field D23 does not touch: "nothing else moved" is
proven the three ways this repo proves such things — the fixture's
report compares equal on every other field, exact and byte-for-byte
(`ref_convert`), the code paths that compute them are untouched by
this entry's diff, and the doubled run below is byte-identical.

**Counts (D12), on the current store.** Before, in the old routing —
which counts a key, not a document: `entities` 15,540,
`other` 779,858 (514 unparseable, 740,591 actor, 38,752 index, 1
dimension beyond the three), no `actors` field. After: `entities`
**777**, `actors` **740,591**, `other` **515**, `chunk_data`
**215,490** unmoved. A prediction probe — the shipped key rules and
the shipped reader, run over the store before convert was — named all
four numbers beforehand, and both release runs printed them exactly:
rc=0 twice, `281` output files each, every file's sha256 equal, JSON
reports byte-identical. The fixture pins the same shape at the small
scale: `entities` 1 → 3 (the unread record once, two documents twice,
the empty record zero), `actors` 1 with `other` held at 1 — the
fabricated actor and index may not leak into `other` — and the
entity fixture's `actors` 0.

**Cost accepted.** The report's `entities` no longer answers "how
many keys" but "how many documents", a field whose number shrinks by
twentyfold on this store because the old number counted records the
game mostly left empty. Anyone diffing reports across entries will
see `entities` fall and `other` collapse; both are the truth getting
sharper, not the world shrinking — and the `actors` field, new
beside them, is where the 740,591 modern records went.

**Reverses if:** Amulet or PyMCTranslate grows real entity
translation (a `entity` database or `entity_support = True`), a Java
load test shows a document form mcme could carry whole for some
entity id, or a reader-facing use for the `digp` index appears — the
count becomes a carry, and the pins follow the documents.

## D24 — Convert answers the pre-1.9 `{name, val}` palette: the loader's own rule, 2,330 rows and five defaults

**What the reference does, measured from the loader down.** Amulet's
block-storage loader branches on the entry's shape
(`base_leveldb_interface.py`): `if "states" in block or "val" not in
block` — a modern entry, states present or absent-empty — else the
entry becomes `{block_data: IntTag(val)}` and translates through
`min(world_version, (1,12,999))`. mcme's declared input is bedrock
1.20.80, so the cap is (1,12,999), which PyMCTranslate resolves to
bedrock (1,12,0), then universal, then java (1,20,0). `version` is
read by nothing on either side and never gates anything. The
signedness agrees both ways (`ByteTag(200).py_int` is −56; mcme's
byte tag is `int8_t`), and the reference is deterministic end to
end: an internal translate error returns the input block, and the
grass puzzle that looked like nondeterminism was two runs' val 0
versus val 1 confusion — grass's only answered value is 0
(`grass_block{snowy:false}`), every other val passes through.

**The table, enumerated whole.** Every one of the 328 names the 1.12
to_universal tables hold × the byte domain −128..255 — the domain a
byte or short tag spells — answers through that exact pipeline, and
**2,330 rows across 279 names** differ from the plain fallback
(name, no properties). Five names decode by formula rather than
table — kelp's age, sapling's type and stage, tnt's `unstable`,
noteblock's constant answer, lava_cauldron's — and each carries one
default row answering every val its rows do not claim; the defaults
hold across the int32 range (a full-range sweep: the five answer
the default at every sampled val outside their rows, the other 323
names answer name-alone everywhere outside the byte domain, so the
byte domain is complete). Row properties are all strings — Java
blockstate properties are `TAG_String` — sorted alphabetically, at
most seven per row.

**The rule in mcme.** `mcme_translate_block` replaces its version
gate with the loader's own branch: `states` present or neither
field is modern (walked by the rules below, `version` ignored); no
`states` and a `val` is legacy — a numeric val answers an explicit
row, else the name's default row, else the name alone and no
properties (the reference's own pass-through with `block_data`
dropped by the unknown-block policy); a val no int32 reaches never
truncates — defaults only; a `val` that is not a number is refused
and counted once, positions air (D12). Rows sort by name (byte
order) then val, the lookup binary searches, then the defaults by
name. The tick path answers its `blockState` through the same
function, so a legacy tick state carries the same way.

**Fixture truth.** The v1 payload the golden chunks fixture holds —
chunk (0,0), cy 2, four entries [stone/0, dirt/0, log/4, grass/1],
all 4096 cells non-air — now writes its section where air stood:
`blocks` 36,864 → 40,960, `legacy_subchunks` 1 → 0, and (0,34,0)
reads `minecraft:oak_log{axis: "x"}` while stone, grass and dirt
read name-alone. The report field keeps its name and its CLI label
("subchunk mcme cannot read") and now counts what is still unread:
payloads whose version this build does not decode, and palettes
holding an entry with no string name or a non-numeric val.

**Cost accepted.** ~255 KB of generated rows in translate.c; the
generator is a one-off in /tmp like D17's (the pin re-asks the
reference for every row it emitted, so the generator needs no repo
home), and the new `pin_legacy_map` re-asks the reference for all
2,330 rows, each default at every unclaimed byte val plus the
extremes, and each plain name's unclaimed val — name-alone — on
every battery run. The real store holds no v1 subchunks (legacy 0),
so both release runs are byte-identical to D23's, file for file and
report for report, and no cross-entry delta is claimed.

**Reverses if:** amulet's loader branch changes shape,
PyMCTranslate's bedrock 1.12 tables change their answers, or the
game grows a new val-bearing entry shape — the pin re-measures all
three every battery run, and the table regenerates from the same
enumeration.

## D25 — Convert carries the pre-palette payloads: two planes, two eras, 128 delta rows

**What the reference does, measured from the flat branch down.**
A pre-palette payload (versions 0, 2-7) is the version byte, 4096
block ids, then 2048 data nibbles — low nibble first, cell i's
nibble in byte i/2 — and amulet's loader reads exactly those bytes
as `combined = id << 4 | data`, one palette entry per distinct
pair behind cells offset past its air seed. mcme reads the same
bytes the same way into one synthesized layer whose entries are
the cells' own blocks spelled the way the loader spells them:
`name` from the bedrock 1.12 numerical map (six ids — 210, 211,
212, 217, 230, 242 — spell `minecraft:numerical`, the
placeholder the reference passes through), the data as `val`, and
the payload's era packed as `version` (`major << 24 | minor << 16`:
16842752 for payloads 0-5, 16908288 for 6-7). The era is amulet's
own `min(chunk_version_to_max[V][1], world)` capped at
`(1,12,999)` — V 0-5 resolves to bedrock 1.1.0, 6-7 to 1.2.0 —
and the V that matters is the payload byte, which for flat-era
worlds is also the first byte of the chunk-version record the
game keeps under a 0x2c key. The pin re-derives all eight stamps
from amulet's table every battery run and maps them to eras
explicitly: an int is a DataVersion on any other path, so the
stamp is never looked up through `get_version(int)`. The
reference's rule for the entries the loader itself spells: a
numeric `version` on a `{name, val}` entry anchors there and beats
the cap branch; a val without one is D24's cap chain exactly.

**The deltas, enumerated whole.** Two eras × the 251 distinct
names the numerical map spells × data 0-15 — 8,032 cells asked of
the reference — yield **128 delta rows** (91 at era 1.1, 37 at
1.2) where the era answer differs from the cap chain: sandstone
val 3 stays `minecraft:sandstone` where the cap smooths it,
jukebox val 0 keeps no record at era 1.1 while era 1.2 says
`has_record: false` — the chain's own answer, hence not a delta
there — and the rest. Rows are delta-only by construction: a cell
the tables do not carry is answered by D24's chain, which is the
reference's answer too, proven cell by cell in the pin. The
generator's first cut emitted 336 rows; 208 were canonicalization
artifacts — the chain replica compared the raw row property `"0"`
against the era answer's canonical `"false"` — and the fix was in
the replica, not the data.

**The rule in mcme.** The payload byte picks the decode branch:
versions 0, 2-7 read the two planes (bounds-checked by plane
name, bytes past them the same tail refusal the palettized path
makes) into one synthesized layer — first-seen (id, data) pairs
as palette entries in cell order, `bits` the width that holds the
slots, no index byte, one layer — and versions 1, 8, 9 are
unchanged. The legacy translation branch gains one step: a
numeric `version` on a val entry asks `flat_lookup(era, name,
val)` first — the 128 rows sorted (era, name byte order, val),
binary searched — and a miss falls to D24's chain exactly as the
reference falls through; a version that is not a number, or any
other version value, never reaches the lookup (the cap chain
answers — the documented boundary against the real val-entry
domain, where `version` is the entry's own anchor). The encoder's
gate stands untouched: a flat payload decodes for reading and
refuses to encode, loudly and before a byte is appended (D12) —
the id-to-name composition is lossy for the six unmapped ids, and
a loud refusal is how that gets noticed, never a half-write. A
payload that fails the plane bounds or carries a version nothing
decodes is refused and counted once: `legacy_subchunks` keeps its
name and its CLI label and counts those; flat payloads that
decode count as ordinary subchunks.

**Fixture truth.** The copy gains four keys under the pre-1.18
0x2c prefix, in the overworld's (0,0) at heights where no section
and no biome array stand: payload 5 (era 1.1) carrying sandstone
val 3, jukebox, an unmapped id and a log; payload 7 (era 1.2) the
era-pair cells; plus two that must refuse — a payload cut inside
its planes and a version nothing decodes. Report: `subchunks`
10 → 12, `blocks` 40,960 → 49,152, `legacy_subchunks` 0 → 2,
everything else unchanged; every position of both flat payloads
compares clean, and the era answers differ from the chain exactly
where the 128 rows say they do.

**Cost accepted.** 128 generated rows in translate.c, 256 id
names and 8 era stamps in bedrock_chunk.c; `pin_flat_map` re-asks
the reference for all 8,032 cells, the 256 ids against
`ints_to_block`, and the eight stamps against amulet's own table
every battery run. The real store holds only v9 payloads, so the
flat tables answer nothing on the census store by construction
and the release runs stay byte-identical to D23's. A flat payload
under a 0x2f key is a documented boundary, not a handled case:
the game cannot write it, and there the reference would answer
from the chunk's version record while mcme answers from the
payload byte — the fixture fabricates only the real shape, 0x2c.

**Reverses if:** amulet's flat branch changes shape,
PyMCTranslate's 1.1 and 1.2 tables change their answers, or the
game grows a flat-era payload the two measured eras do not cover
— the pin re-measures all three every battery run, and the tables
regenerate from the same enumeration.

## D26 — Convert carries the older biome record: the 768-byte table's cells and map

**What the reference does, measured from the loader's elif.** A
chunk whose data holds no Data3D record (`b"+"`) but holds the
older spelling (`b"\x2d"`) is read as `height =
frombuffer(d2d[:512], "<i2").reshape(16, 16)` and `biomes =
frombuffer(d2d[512:], uint8).reshape(16, 16).T` — the same
512-byte int16 heightmap half the Data3D record starts with, then
256 raw biome ids, one per column, no palette; when both records
stand, the 3D arrays win (the elif), and the older table is not
read at all. On the way to Java, `convert_to_3d` broadcasts the
table: `repeat(self._2d[::4, ::4].reshape(4,1,4), 64, axis=1)`
fills sections **0..15 absolute** — measured from Biomes3D's own
defaults, `(0, 16)` sections above the origin, not from any
dimension's floor — and every section's cell (x, y, z) reads
table byte `64z + 4x`, the same value down the column's four
cells. The java container order is the Data3D path's own:
`transpose((1, 2, 0)).ravel()` = `y*16 + z*4 + x`. Ids the
reference's biome table does not hold revert to `minecraft:plains`
with a warning — its own fallback, the one mcme's map has made
since D19 — and the table is version-stable where it matters:
bedrock (1, 2, 0)'s int→str table is 69 entries, (1, 20, 80)'s is
86, and every shared id spells the same name — ids were only ever
added, so one map answers both eras' records.

**The rule in mcme.** Both spellings are biome records now: the
scan collects a 0x2d key exactly like a 0x2b one, and
`chunk_data` loses the 0x2d — it counts the versions, markers and
unknown chunk-data tags it counted before, nothing the report's
biome counters can also count. The sort's tiebreak puts the Data3D
record first when a chunk holds both — not a guess which the game
means but the loader's own precedence, and the older record counts
beside the answering one: 16 broadcast arrays of 64 cells and a
map, never carried. `mcme_bedrock_biome_2d_decode` materializes
the broadcast the reference's `convert_to_3d` materializes: the
heightmap half decoded exactly as the Data3D record's, then 16
arrays at absolute cy 0..15 — whatever the floor, because the
reference's broadcast is absolute — cells in Java's container
order, `bytes[512 + 64z + 4x]` at `y*16 + z*4 + x`. Anything
other than exactly 768 bytes is refused whole: the reference
slices exactly a map and a table, and its reshape answers for
nothing else — counted once, cells and map lost with it (D12).
From there every verdict is D19's and D20's unchanged: a written
section carries its 64 cells through the biome map (unknown ids
the plains fallback, counted), an array with no section to stand
in counts 64, the map crosses when every column spells inside the
dimension's own height.

**Fixture truth.** The copy gains three keys in the older
spelling. The end chunk (1,-2) dim 2 gets a real one: columns
spelling their own coordinates inside the end's 256, and a table
whose 16 sampled positions hold id `x + 4z` — every one a biome
the map knows — except column (x=1, z=2), which holds 194: an id
neither table knows, so its four cells per section fall back to
plains and count, the Data3D fixture's own unknown id made
familiar. The overworld (0,0) gets a second spelling beside its
answering record — all zero, the shape is the pin — and the
orphan at (10,10) is 767 bytes, one short: not a record, counted
once. Report: `biomes_carried` 383 → 443 (the end section's 60
known cells), `heightmaps_carried` 2 → 3 (the end's map crosses
now), `skipped.biomes` 386 → 2375 (the end's 15 arrays with no
section, the unknown column's 4 cells, the ignored second
spelling's 1024, the orphan's 1), `skipped.heightmaps` 6 → 7 (the
end's drop became a carry, the ignored map and the orphan's lost
one stand beside it), `chunk_data` unchanged at 1 — the older
records are not there to count anymore. The mirror reads the
table the loader reads it, and the parity check counts 448 cells
where it counted 384: the end's one section beside the
overworld's four heights and the nether's two floors.

**Cost accepted.** No new tables: the biome map has answered all
256 ids since D19, and the measured id stability across versions
is why one map serves both spellings. The pin re-asks the
reference for every cell the fabricated records reach, the
unknown-id fallback included, and the heightmap comparison pins
the third map under MOTION_BLOCKING. The census snapshot that
pinned earlier drops' real-store behavior is gone — stale /tmp
cleanup, no source world left on disk — so this drop is
fixture-verified by the user's own call, and the dev-log says so.

**Reverses if:** amulet's `convert_to_3d` broadcast changes its
range or sampling, the biome id table grows a rename instead of
an append, or the game grows a third biome spelling — the pin
re-asks the reference's cells and fallback every battery run, and
the mirror decodes both records from the loader's own lines.

## D27 — The state map grows: the keeps, the axis suffixes and the wire branch retire into one generated delta table

**What the reference does, measured from its own answers.** Every
(block, states) cell's Java answer runs through `ref.answer` — the
two-leg translation with the unknown-block strip — and its third
element is the reference's own empty-cell answer, the value
props_match judges an absent property by. Measuring every block
the 1.20.80 spec registers against that default splits the states
three ways: values the answer carries BEYOND the default (the
wire's power at each signal, a door's hinge at its bits, snow's
layers at its height, water's level at its liquid_depth — 2,207
(block, state, value) cells); values that EQUAL the default (a
furnace's lit, grass's snowy at both values, the wire's four
sides, axis "y" — absence answers them the same); and
canon-`'false'` values props_match's false-escape also lets stand
absent. Booleans answer constant `'false'` whatever tag type and
value they carry — the pass-through kKeep shipped wrote
`snowy:"true"` where the reference answers `'false'`, divergent
at every truthy value the fixture never held. The wire's branch
hardcoded `power "0"` at every signal while the reference answers
the signal itself, and hardcoded the four sides the reference's
own empty answer already stands for. The row conditions are
tag-type strict the reference's own way — the value must carry
the spec's tag type and the row's value, no other spelling (the
name map's numeric path is looser; D17 shipped it so, and this
does not inherit it).

**The rule in mcme.** kKeep, kAxisSuffixes/kMushroom and the wire
branch are gone. In their place the generated tables between the
GENERATED STATE MAP and STATE RESCUE markers: one row per
(block, state, value) whose Java answer differs from the default
an absent property stands for, carrying the reference's own RAW
value — `power` carries `"1"` at signal 1, never the canon
`"true"` — and rows sharing a key compose, all matching rows
applying in table order, later puts overwriting earlier ones.
A two-condition rescue row answers the cells whose composition
alone cannot (hanging signs, doors, stems, walls): fired after
every single row, the first match overwrites and stops. The walk
binary-searches the block's group and scans it linearly, the name
map's own lookup shape. A state no row fires on is dropped: the
block takes the default an absent property stands for — the
reference's own empty-cell answer, which is what props_match
judges absence by, and what translate.h's rewritten rules 3 and
4 now say. (Where that answer differs from the Java spec's own
default — the wire's sides answer `'side'` against a spec
`'none'`, a furnace's empty cell answers `'south'` against
`'north'` — the absence still answers the reference's way: the
doctrine props_match has shipped since D17, unchanged here and
recorded as measured.)

**Fixture truth.** The generator measured 1,087 blocks — the name
map's own plus everything the 1.20.80 spec registers — through
base cells, every single state value, every base-minus-one-state
cell and every pair: 12,252 probe cells, 0 fails under
props_match, the pin's own acceptance function. The tables
(2,752 single rows, 422 rescue rows) were spliced into
translate.c; the selftest's translate assertions move to the
delta model, with new cases pinning what the fixture never held
(grass snowy true, furnace lit 1b → name only; the wire at
signal 7 → `power "7"`). Eight cells fabricated on a /tmp copy
of the fixture store — wire signal 7, furnace cardinal east,
snow height 4, water depth 3, mushroom bits 10, both door
halves, grass snowy true — convert to exactly the reference's
answers, cross-checked through props_match against ref.answer.
The 63 name-skip blocks (6,077 probe cells) are D17's name-map
gaps, counted not fixed: the wood hanging/standing signs where
the map only has bamboo, the →air families (border_block,
chemistry_table, glow_frame, copper_bulb, colored_torch,
underwater_torch), copper doors/trapdoors → oak, tuff →
cobblestone/oak/stone, double_slab → slab, the legacy spellings
(tripWire, light_block, pistonArmCollision, lit_blast_furnace,
end_brick_stairs, darkoak_wall_sign). The fixture never holds
any of them — the battery is green on that ground today.

**Cost accepted.** ~3,200 lines of generated table replace ~90
lines of hand-kept rules; the walk is one binary search plus a
linear scan per palette entry, and the pin checks the group
contiguity and byte order the binary search needs. mushroom_stem
keeps its own no-rows answer — the 1.20.80 spec does not
register the name, its pillar_axis is an unknown state like any
other — while the stem's Java properties arrive through
brown_mushroom_block's huge_mushroom_bits, as the reference
answers them. No census: the drop is fixture-verified by the
user's own call, like D26.

**Reverses if:** PyMCTranslate's answers move (the pin re-asks
every row at its own cell, raw spellings compared raw, every
battery run), the tag-type strictness proves too strict for a
real-world palette the fixture never held, or the rescue families
outgrow the two-condition rows — the probe's pair enumeration is
the measure, and it is re-runnable from the generator.

## D28 — The name map grows to the whole spec: 244 groups, the blocks no census reached

**What the reference does, measured from its own answers.** D17's
name table was the cases a real world was known to hold — the
census of one store, 828 distinct names, and the blocks it never
contained were left passing their Bedrock spelling straight to
Java, where the chunk parser cannot read it. The census is gone;
the spec is not. Every block the Bedrock 1.20.80 spec registers
(1,087 of them) was measured against PyMCTranslate's own answers
over the same cells the state map walks — base, every single state
value, every one-state partial, every pair — and 63 of them
answered a Java name mcme did not carry: the ten wood hanging
signs, the standing signs, the copper doors and trapdoors and
bulbs, the tuff and double-slab families, the legacy spellings
(tripWire, pistonArmCollision), the blocks Java 1.20 does not have
(border_block, the chemistry tables, the coloured torches).
6,077 probe cells went unanswered because of those 63 names.

**The table, grown the way D27 grew its own.** A generator
(`/tmp/opencode/d28_gen4.py`) measured every block's cells through
the reference, and where the answer moved it synthesized the cover:
a wholesale row where the name moves everywhere alike, and where
it varies, value rows whose condition the reference answers
uniformly — every measured cell satisfying the condition names the
same Java block — emitted in first-match order with the empty
cell's answer as the default row last. The 309 rows D17 wrote stand
byte-for-byte; the 264 new rows join as 244 new groups in the byte
order the lookup walks. The acceptance is the same walk the state
map's probe runs: every measured cell of every block resolves
through the finished table to the reference's answer, and
`pin_name_map` re-asks every row at its own conditions against the
reference on every battery run.

**The conditional family.** The wood hanging signs are the one
block whose Java name hangs off the states: `attached_bit` at 1 or
`hanging` at 1 keeps the floor variant's name, both at 0 is the
wall variant. That is the shape bamboo_hanging_sign already had
from D17 (the census held bamboo); the other ten wood types get
the same three rows. Every other new block is a wholesale rename,
several of them to `minecraft:air` for blocks Java 1.20 does not
have — the answer is the reference's own, quirks included.

**What the state map inherits.** Once the names answer, the state
map grows rows for the blocks it had skipped: 2,207 → 2,496 single
rows, 324 → 366 rescues. lit_blast_furnace now carries
`facing`/`lit`, light_block carries `level` at its
`block_light_level`, tuff_stairs carries oak_stairs' `facing`,
piston heads carry their `facing`/`short`/`type`. The name skips go
to zero: the probe's full 12,252 cells answer, none skipped, none
failed.

**Reverses if:** PyMCTranslate's answers move (the pin re-asks
every row, and the probe re-measures every cell), the uniformity
rule proves too weak for a name that needs three conditions to
cover (the two-condition rows are the table's ceiling), or the
spec gains blocks the reference has not learned (they would be
measured, and their cells counted the way D17 counted its gaps).

## D29 — Phase 5 opens on the manifest: a directory names itself a pack

**What the reference does.** The reader the original Python used for
the whole resource half — `minecraft_model_reader`'s
`JavaResourcePack` — decides a directory is a resource pack by one
test: `pack.mcmeta` is a file. From it the reader keeps three
things: `pack_format` as whatever JSON type it found, `description`
coerced with `str()`, and whether `pack.png` sits beside the
manifest. Its validity flag turns on exactly one condition:
`pack.pack_format` is *present* — no type check, no description
required. Everything else Phase 5 needs (blockstates, models, the
atlas) hangs off this identity layer.

**What mcme does.** `mcme_pack_open` (src/resource/pack.c) reads the
same manifest through cJSON and fills an `McmePack` the caller frees
with one `mcme_pack_free`: an int32 `format`, a heap `description`,
a `has_icon` flag, the `root` it came from. Three refusals, all of
them conditions the reference also calls not-a-pack: no readable
`pack.mcmeta`, a manifest that is not the game's `{pack:{...}}` shape
(having no `pack` object), and no `pack.pack_format`. A missing
description is the empty string, exactly the reference's default —
not a refusal. The verb is `mcme pack info <dir>` with
`--json {dir,format,description,has_icon}` (D3: the CLI is the
product; the render layer later calls the same function).

**The recorded difference.** The reference accepts `pack_format` at
any JSON type and keeps the raw value; mcme requires an integer and
refuses any other type, because its field is int32 and the game
writes integers. The reference `str()`-coerces any description type
(a translation-key object becomes a Python repr); mcme reads a
string description and defaults to empty otherwise. Real packs carry
both as the game writes them, so the shared domain is total — but a
manifest out of that domain is where the two readers part, and it is
recorded here rather than papered over: mcme is the stricter of the
two, and it refuses instead of reporting a value it cannot trust.
`tools/ref_pack.py` pins the shared domain against the reader itself,
including the reader's own shipped `java_vanilla_fix` pack.

**Why the manifest first.** Simplexity 9 — test the parser before
the pretty part. The manifest is the one file every later layer
opens the pack through, it answers with data the reference answers
with, and the whole slice runs with no pixels, no GL and no atlas in
sight. Blockstate and model resolution (the `variants`/`multipart`
matching and parent merge in the reference's manager) is the next
drop's subject, measured the same way.

**Reverses if:** the game's manifest grows a field the model layer
cannot wait for, multi-pack stacking forces the merge question
earlier than the reader's list-of-packs model, or the atlas lands
before blockstates need it (then the identity layer moves up a drop,
and this one still answers `pack info`).

## D30 — The blockstates answer: variants first-match, multipart collects

**What the reference does.** `_get_model` in the reader's manager
answers one block state with a mesh, but the *selection* — which
JSON apply values it feeds the mesher — is where the rules live,
and the two branches of a blockstate file follow deliberately
different laws:

- `variants` is consulted in **file order** and the first key that
  matches wins outright. A key is comma-separated `name=value`
  pairs matched by the regex `,(name)=(value)` over `[a-z0-9_]+`:
  text the regex cannot pair — a `|`-separated second value, an
  unpairable key — is never seen (and a key it pairs nothing from
  matches *everything*, `all([])` being true). A pair whose
  property the block does **not** have matches anyway, because the
  reference defaults the missing property to the pair's own value.
  A `""` key matches unconditionally at its position. No key
  matching, no file, or a file holding neither key answers the
  missing mesh.
- `multipart` consults every case in order and **every** matching
  case contributes its `apply` — an empty result list is an empty
  selection, *not* missing. Here a property the block does not
  have **never** matches (the reference looks it up as None).
  `OR` needs one arm, `AND` needs all, an empty arm or empty
  `when` matches, a `when` value splits on `|` or is a JSON
  boolean, and a value of any other type raises — which the
  reference's per-case catch turns into *skip this case*.
- An `apply` that is a list contributes its **first** element; one
  without a `model` degrades the whole selection to the missing
  mesh on the variants branch while the selection still happened.

**What mcme does.** `src/resource/blockstate.c` mirrors those laws
pair for pair, arm for arm, quirk for quirk — including the two
opposite absent-property rules, which differ on purpose. The index
parses every blockstate file at open and skips an unparseable one
with a warning, the reference's loader catching that error per
file the same way. The answer is `McmeBlockstate`: the selected
models in selection order with `model` as written (NULL when the
apply carried none), `x`, `y`, `uvlock`, and `missing` — true only
for no-file / no-key / no-matching-variant, never for multipart's
empty selection. The verb is `mcme blockstate <pack-dir>
<block[states]>`, human and `--json`; a selection is an answer, so
`missing` exits 0, while a pack directory that does not exist
exits 2 so a typo cannot read as "every block is missing". Java
state properties are strings and a numeric state is refused, not
coerced, because a number can never be the string it pretends to
be.

**How parity was measured.** The reference answers with a mesh,
so the pin observes the selection where it happens: it wraps
`_load_blockstate_model` — the single function every selected
variant and every matching multipart case passes through — records
the raw apply values in order, and returns the missing mesh so no
model file ever opens. What lands in the record plus whether
nothing was recorded and the result *is* the missing mesh is
exactly mcme's two fields. `tools/ref_blockstate.py` runs that
against 27 crafted probes (every quirk above), 500 probes across
the reader's own `java_vanilla_fix` pack's real game files (one
per variant key, the cartesian of each multipart file's `when`
values under a budget), and 3 usage refusals — 530 checks. The
reference's logged exceptions during the run (the bad fixture
file, the numeric `when` value) are its skip paths firing; mcme
answered the same on each.

**Out of domain, recorded.** A `variants` key that is not a JSON
object would crash the reference outright; mcme answers missing
instead. Shapes the reference's own catch skips (a non-object
`when` or `apply`, an empty apply list) mcme skips too. The index
is a linear scan — thousands of files per lookup is fine for a
verb and would not be for a mesher; the index grows a hash when
the mesher arrives, not before.

**Reverses if:** pack stacking arrives (the reference lets later
packs overwrite earlier ones; mcme takes one pack directory), the
model layer needs rotations applied at *this* layer rather than
at mesh time, or a game format retires variants/multipart for
something else entirely — in which case `pack info` and the index
still stand and only `resolve_*` changes.

## D31 — The model layer: the fold carries two keys

**What the reference does.** `_recursive_load_block_model` in the
reader's manager answers one model reference with a single merged
dict, and the merge is far narrower than "merge the parent chain"
suggests:

- The reference splits the model path on its **first** `:`; with
  no `:` the namespace defaults to `minecraft`.
- A file the pack does not have answers the **empty dict**, not an
  error — at any level of the chain, so an unknown parent
  contributes nothing and the child still answers.
- The parent chain folds **first**, then the current model merges
  over the accumulated result, and only TWO keys are carried:
  `textures` merges **per key** (the parent's dict is the base,
  each child key overwrites that one key; keys the child does not
  name survive, and a `"textures": {}` in the child still creates
  the key), and `elements` overrides **wholesale** (whichever
  model in the chain names it last contributes the entire list —
  an empty list wipes it, and the value is taken at face value,
  any JSON type, no validation).
- Every other key — `display`, `gui_light`, `ambientocclusion`,
  `parent` itself — is **discarded**. A model whose only content
  is `display` folds to the empty object.

Texture substitution is not part of this function: `#name`
references are resolved by the mesh layer (`_load_block_model`)
against the folded textures dict, so a fold here is the file's
own truth with `#` refs as written.

**What mcme does.** `src/resource/model.c` mirrors that fold —
per-key textures, wholesale elements, the discard, the empty
answer for a missing file at every level, the first-`: ` split
with the minecraft default — into `mcme_model_load`, which
returns the folded document as a heap `cJSON` object the caller
releases with `cJSON_Delete` (every value a deep copy, so the
answer stands alone). The index parses every model file under
`assets/<ns>/models/` recursively — nested directories are part
of the reference's own glob reach — and skips an unparseable one
with a warning, the reference's loader catching that error per
file too. The verb is `mcme model <pack-dir> <model-ref>`; its
`--json` prints the bare folded document, the reference's own
answer shape, and an empty fold is an answer that exits 0. A
malformed reference is exit 1 (a typo must not read as "no such
model"), a missing pack directory exit 2.

**How parity was measured.** The fold is pure JSON, so no spy was
needed: the pin calls `_recursive_load_block_model` directly — it
IS the function mcme mirrors — and compares mcme's printed
document, parsed back, dict for dict. `tools/ref_model.py` runs
16 crafted probes (the three-deep chain, the empty-elements wipe,
the discarded-keys model, the orphan, the cross-namespace parent,
the bare-path default, non-string texture values, a top-level
array, the unparseable file both readers skip), 241 probes over
the reader's own `java_vanilla_fix` pack (every model file by
`namespace:path` plus bare-path samples — real game data through
both folds), and 4 usage refusals — 264 checks.

**Out of domain, recorded.** Three shapes differ on purpose, each
pinned against the reference's observable behavior rather than
hidden: a `parent` that is not a string would raise on the
reference's `.split` — mcme reads it as no parent (the pin
asserts the reference raises `AttributeError`); a parent cycle
would raise `RecursionError` there — mcme refuses by name, exit
3; and a chain deeper than `MODEL_DEPTH_CAP` (64) is answered by
the reference up to Python's recursion limit (~1000) — mcme
refuses, exit 3, because a real pack's chains run a handful deep
and the cap is the cycle guard that keeps mcme's stack bounded.
Like D30, the index is a linear scan until the mesher arrives.

**Reverses if:** pack stacking arrives (the reference lets later
packs overwrite earlier ones; mcme takes one pack directory), a
real pack needs `display` or `gui_light` to propagate up the
chain (the reference's mesher never reads them from the fold, so
this is not expected), or the mesh layer needs the fold to keep
more than the two keys — in which case the fold widens and this
entry is superseded.

## D32 — The mesh layer: one model, as the game would build it

**What the reference does.** `_load_block_model(model_path,
tint=(1, 1, 1))` folds the model (D31) and builds a `BlockMesh`:

- The fold's `textures` dict is read for **keys beyond
  `particle`** — a truthy beyond plus falsy `elements` answers
  the reference's **missing BLOCK** (the unit cube on
  `missing_no`, opaque), not an empty mesh; `elements` empty as a
  list, an empty string or an empty dict runs zero turns and
  answers the empty mesh, while null, `false` and `0` raise in
  its loop, and any other truthy non-list raises on its first
  element.
- Each element: `from`/`to` default `[0,0,0]`/`[16,16,16]`; the
  box is numpy's `sort(array([to, from]) / 16, 0)` over its
  columns, so a two-number box raises on the missing third column
  and an even four-number box builds from the first three
  columns. An unrotated full cube with six faces runs the
  transparency check per face: a face on an image with any alpha
  below 255 (RGBA-mode images only; RGB-mode is opaque by
  definition) keeps the block from going opaque, and **the
  missing image is not in the transparency table at all**, so a
  full cube on one raises `KeyError` there.
- `rotation` present switches the full-block check off entirely
  and runs `rotate_3d` on each face's corners: axis `"x"` by
  default (an axis that is not one of the three strings leaves
  all three angles at zero and still rotates — an identity),
  angle 0, origin `[8,8,8]` divided by 16, then the 1.21.11
  per-axis `x`/`y`/`z` overrides and the `rescale` bool. A
  two-number origin raises on its unpack.
- Each face: direction selects its four corners from the box
  lut; `uv` defaults from the box per direction (west/east
  swapping the y pair) or takes four numbers divided by 16, and
  any other shape raises `ValueError`; the corner cut is
  `lut[2*int(rot/90):] + lut[:2*int(rot/90)]` — negative
  rotation wraps to its positive twin and 45 degrees does not cut
  at all. `cullface` routes the BUCKET (`None`, an unknown
  direction or a non-string sharing the none bucket); face
  direction never names one.
- Textures: a face's `#chain` walks the dict stripping one `#`
  per step with loop detection (a cycle raises `RuntimeError`),
  an absent key answers itself, a JSON null value answers itself
  the same way (its `dict.get` sees only `None`), a `{sprite:
  ...}` passes through, and anything else raises on its `.split`.
  The resolved reference splits on its first `:` with the
  minecraft default, and the texture list **dedups on that split
  base, namespace-blind** — the first face's full resolution
  names the stored path. A face without a string texture raises.
  An element with `faces` but no textures dict raises on
  `java_model["textures"]`.
- The assembly adds a bucket entry **only for non-empty
  cullface-keyed face arrays** (`FACE_KEYS` includes `None`), so
  empty directions are absent from every output dict.

**What mcme does.** `src/resource/mesh.c` mirrors all of it —
`mcme_mesh_build(models, textures, ns, path, out)` folding first
through `mcme_model_load` and refusing by name exactly where the
reference raises: the missing image on a full cube, a texture
loop, elements without a textures dict, a face without a
texture, a uv that is not four numbers, elements that are not a
list, an origin that is not three numbers. The bucket is
`cullface`-keyed with the none bucket for `None`; the vertex
arrays are **doubles**, the reference's float64 (a float32
vertex drifts onto its own grid the moment rotation makes a
coordinate irrational — the pin caught it at 6e-9). The textures
index (`texture.c`) walks `assets/<ns>/textures/**/*.png` for
the pack_format 4 layout and the image layer (`image_impl.c`)
mirrors the RGBA-with-alpha rule over stb_image, an unreadable
file warned and read opaque. The verb is `mcme mesh
<pack-dir> <model-ref>`; `--json` prints the BlockMesh's own
shape — `face_mode`, the normalized `textures` list,
`transparent` (0/1/2), and each **present** bucket's flat
`verts`/`tverts`/`tint_verts`/`faces`/`texture_indexes`.
Exit 3 on a refusal, 2 on a missing pack, 1 on a malformed
reference. The gate is D29's: the reference guards its entire
texture-and-model load on `valid_pack && pack_format >= 2`, so
`mcme_pack_loads_assets` now gates all three index opens (blockstate
included, retro to D30/D31) and a failing gate answers the empty
index — an answer, exit 0.

**How parity was measured.** The fold plus the build are one
function there, so `tools/ref_mesh.py` calls
`_load_block_model` directly with its default tint and compares
the returned BlockMesh field for field (the reference's missing
image normalized to the token mcme prints; floats with a 1e-9
tolerance, exact for ints). 23 crafted answer probes (real PNGs
behind each transparency outcome), 13 refusals each asserting the
reference's own exception class, 241 probes over the reader's own
`java_vanilla_fix` — 220 answered alike, 21 refused by both —
and 4 usage refusals: 285 checks. The vanilla pack's completion
(copy only, never the original) writes an opaque image behind
every texture its models can name, or every full-cube model
would die in the reference's transparency table.

**Out of domain, recorded.** Four shapes differ on purpose, each
pinned to both sides' own observable: a `textures` value that is
not a dict — the reference raises while **folding** it (its
`.items()`), and mcme's fold drops it the way it drops a
non-string parent (D31's record), so the mesh answers the empty
document; a four-number `from` against a three-number `to` — both
refuse (numpy's array is inhomogeneous), while the even
four-and-four case builds there and mcme refuses past three; an
unreadable PNG — the reference's load dies, mcme warns and reads
it opaque; and the gate's stricter manifest (D29) decides
`valid`/`pack_format` where the reference's own parser is laxer.
The vanilla refusals are not differences: a self-looping texture
key with a face on it (banners, chests, heads) and
`golem_statue`'s textureless elements raise there, mcme exits 3
here.

**Reverses if:** the reference seeds its transparency table with
the missing image (then a full cube on a missing texture would
build translucent there and mcme must stop refusing), the fold
learns to keep more than textures and elements, pack stacking
arrives (later packs overwriting the texture table), or the
blockstate rotation (D33) needs the mesh API to take a rotation
before the verb can answer a selected model rotated.

## D33 — The block-level mesh: selection, mesh, turn, merge

**What the reference does.** `_get_model(block)` is `_get_model`
composed of `_load_blockstate_model`, one apply at a time. The
blockstate file is found by `(namespace, base_name)`; if it holds
`variants`, each key is tried in file order — the `""` key matches
unconditionally, other keys regex-match against the block's
properties (a property the block LACKS matches, defaulting to the
pair's own value) — and each matching key's apply is loaded inside
a try: a load that raises is logged and the loop carries on to the
next matching key, the first that loads being the answer; no match
survives (or none matched) and the missing BLOCK answers.
`multipart` is consulted only when `variants` is absent: every
case whose `when` matches (OR/AND/plain, a `when` value the reader
cannot parse skipping the whole case) contributes its apply's
mesh, each load inside its own try so a refusal skips that case
only, and the function ALWAYS ends in `BlockMesh.merge(models)` —
a merge of nothing is the empty mesh, an answer.

`_load_blockstate_model(apply)` takes a list apply's first element
(a KeyError there being the caller's skip), then reads `model`,
`x`, `y` and `uvlock`. A dict with no `model` key returns the
missing block ACCEPTED — unrotated, untinted, merged in whole for
multipart. `x`/`y` are Python floordiv by 90 (`-45` is step `-1`);
`uvlock` is read and ignored, the reference's standing TODO. The
model is loaded, deep-copied and `.rotate(rotx, roty)`-ed; any of
those raising is the caller's catch — the apply is skipped.

`BlockMesh.rotate` carries one precedence quirk in its guard:
`if rotx or roty and (roty, rotx) in cull_remap_all:` — a NONZERO
`rotx` skips the table test, so an out-of-range pair then raises a
KeyError on the lookup; `rotx` zero with an out-of-range `roty`
returns the mesh untouched; `(0, 0)` short-circuits. When it does
turn, each present cull bucket's vertices rotate by `-90*rotx`
about X then `-90*roty` about Y around `(0.5, 0.5, 0.5)` (the
cull table maps the buckets by the same turns, `None` to `None`);
texture coordinates and tint do not turn. `BlockMesh.merge`
concatenates the parts' buckets per direction under running
vertex counts (faces offset by the count BEFORE each part,
texture indexes offset by each part's own list), makes the texture
list the sorted set-union via `numpy.unique`, and takes the most
transparent part.

**What mcme does.** The walk gained one level:
`mcme_blockstate_select` hands each winning apply to a callback
that accepts or skips it — the lookup's own rules, one
implementation, the lookup being the walk with an accepting
callback. `mcme_mesh_rotate` mirrors the guard's three cases
exactly (false for the raising one, true-and-untouched for the
others); `mcme_mesh_merge` mirrors the sorted texture set and the
index remap by string. The new `mcme_mesh_for_block` composes:
no model means the missing cube accepted UNROTATED, a refusing
build or turn is the reference's caught exception (skip), variants
answer the first accepted apply, multipart merges what survived,
and a selection nothing survived answers the missing cube — with
`floor(x/90)` steps, so `-45` turns by `-1` the way Python floors.
The verb `mcme blockmesh <pack> <block[states]>` answers human or
`--json` (the same mesh document as `mcme mesh`) and is total:
per-apply refusals are skips, answers exit 0, and only 1/2/6 exist
as failures.

**How parity was measured.** `tools/ref_getmodel.py` calls
`mgr._get_model(Block)` directly and compares the BlockMesh field
for field (the missing image normalized to the token mcme prints,
the mesh pin's 1e-9 float rule). 19 crafted probes (x/y turns over
real geometry, both passes at once, negative and non-multiple
steps, the guard's two quirks, the refusal chain with and without
the matching state, model-less applies, list applies, uvlock, a
non-number x, multipart merges with and without a matching `when`,
the empty merge, and both missing shapes), 702 probes over the
reader's own `java_vanilla_fix` behind completed textures (one per
variant key plus the empty probe, the cartesian of when-values for
multipart under a per-file budget), the tint exclusion, one
recorded-difference probe and 5 usage refusals: 731 checks. The
selftest's `test_blockmeshes` covers the same ground from the C
side — the turn's hand-computed corners, the bucket remap, the
multi-face bucket walk, the quirks, the chain, the merges and the
missing shapes — ASan/UBSan clean.

**Out of domain, recorded.** Two shapes differ by record, not by
accident. The block TINT: the reference colours redstone wire,
the stems and the `BlockTints` names (grass, leaves, vine …) by
block while mcme answers the fixed default `(1, 1, 1)` tint
everywhere — the pin computes the reference's own tint function
per probe and skips exactly what it colours, and pins that filter
against four crafted tinted blocks (including the missing-property
case where the reference's lookup itself raises). The MERGE
texture sort: the reference sorts the raw paths, its sentinel
image living at the package's own path, while mcme sorts its
`missing_no` token — so a merge naming the missing texture orders
its list differently on each side; one probe pins each side's own
order (raw paths sorted there, tokens sorted here), the sets and
the geometry agreeing with the indexes remapped through. The D30
record for garbage blockstate shapes stands untouched: a
non-object `variants` and a non-array `multipart` answer the way
the lookup has always answered them.

**Reverses if:** the reference implements uvlock (then texture
coordinates must turn under the lock's rules), the guard's
precedence is parenthesized (then out-of-range steps with zero
`rotx` raise too, and the untouched case disappears), the tint
reaches mcme (then the block-level mesh must multiply tint
vertices per block, and the pin's exclusion inverts into a
comparison), the merge sorts strings by a different key, or
`BlockMesh.merge` grows a face_mode argument.

## D34 — the atlas: many small files on one page

**What the reference does.** The atlas is not in the model reader;
it is the editor's OpenGL layer's `textureatlas` module (a packing
utility the editor vendors). `create_atlas(texture_tuple)` opens
each path as a Pillow frame, sorts the textures by perimeter
(`2w + 2h`) NON-INCREASING and stably — Python's `sorted`, so
equal perimeters keep the input order — sizes the page as
`max(max width, max height, 1 << bit_length(ceil(sqrt(total
pixels)) - 1))`, and packs with `TextureAtlas(size, size)` at
border 0: `PackRegion.pack` gives the first free region that
holds a frame its top-left, marks it taken, and splits it into
the strip BELOW at the frame's own width (`(x, y + h, w, H - h)`)
and the strip RIGHT at the region's full height (`(x + w, y, W -
w, H)`); a taken region tries below, then right; any frame that
fits nowhere raises `AtlasTooSmall`, the page doubles, and the
pack starts over. `generate("RGBA")` pastes every frame at its
placement into one square RGBA image, and `to_dict` answers each
texture's bounds as `(x/size, y/size, (x + w)/size, (y + min(h,
w)) / size)` — the top edge using the SMALLER of the frame's
sides, the module's own quirk — with the dictionary then
reordered to the input tuple's order. The empty tuple answers a
2x2 page with no bounds (`bit_length` of `ceil(sqrt(0)) - 1`,
which is `-1`, so the formula yields 2) and a fully transparent
page. The module supports a packing border; `create_atlas` always
passes 0. The app hands it `mgr.textures` — the manager's table in
filesystem glob order, its `missing_no` sentinel among them.

**What mcme does.** `mcme_image_load` reads a PNG whole as RGBA
through stb with the expansion Pillow's paste into an RGBA canvas
performs (grey, palette-with-transparency and RGB alike),
`mcme_image_write_png` writes one back out, and
`mcme_atlas_build` (new `src/resource/atlas.c`) mirrors the
module rule for rule: load every frame first (one that will not
load refuses the whole atlas by name — the reference's Pillow
raise kills its call at the same file), sort by perimeter with
the input index as the tie-break (qsort is not stable; the tie
IS the index), compute the page the same way including the empty
answer of 2, pack with the recursive region tree — below at the
frame's width, right at the region's full height, x/y recorded
on the frame as the module records them on the packable —
doubling on any fit failure, blit into a zeroed page, and answer
bounds with the `min(h, w)` top edge and names mirroring the
input order. The texture index gained its enumeration
(`mcme_texture_count`/`mcme_texture_at`, directory order); the
verb `mcme atlas <pack-dir> [--out <png>]` SORTS what it is
handed before building — the one feeding difference, below — and
answers human (a summary line) or `--json` `{size, textures:
{path: [u0, v0, u1, v1]}}`, `--out` writing the page PNG.

**How parity was measured.** `tools/ref_atlas.py` loads
`textureatlas.py` by file path — the package chain imports wx,
which needs a display this machine does not have, and the file
itself needs only Pillow — and runs the reference's own
`create_atlas` verbatim over the same sorted tuple: the pin
strips the manager's `missing_no` sentinel (asserted present so
the strip can not silently stop working), mcme sorts its index's
enumeration, and the bounds' keys compare exactly because both
sides name the same pack's paths. Six crafted packs pin each
behaviour — three 2x2s (the perimeter tie on the sorted input,
one RGB image expanding to the same opaque RGBA), a 4x2 among
2x2s (the sort beats the input order), a lone 2x4 (the `min(h,
w)` top edge), 2x2/2x2/3x2 (the estimate fails at 4, doubles to
8), all six together (the `tall`/`wide` tie and a mixed layout),
and the empty pack (the 2x2 page of nothing) — each compared on
name order, page size, every bound at 1e-9, and every pixel of
the page decoded through both decoders. The raw
`java_vanilla_fix` adds real game data: 22 images, the 64x64
banners tying on perimeter, one PALETTE image expanding
identically in Pillow and stb, a 512x512 page. The broken file
is pinned on both sides, the human line and 4 usage refusals
beside them: 67 checks. The selftest's `test_atlases` carries
the same reference-verified expectations from the C side
(every value re-run through `create_atlas` verbatim before
landing), ASan/UBSan clean.

**Out of domain, recorded.** The INPUT ORDER: the app feeds
`create_atlas` the manager's glob order — the filesystem's
directory order decides every equal-perimeter tie there — while
mcme sorts the paths it is handed so the answer never rides on
the disk; the pin pins the algorithm on the sorted tuple for
both sides, and the app's own feeding order is out of domain.
The SENTINEL: the manager's table carries `missing_no` (its file
inside the reader's package), so the app's atlas packs one image
no pack ships — the pin strips it from the reference's input and
asserts it was there. The DEATH-VERSUS-REFUSAL pair: an
unreadable texture makes the reference's pack unmanageable
before any atlas call — its manager reads every image at load for
the transparency flags, and Pillow's `UnidentifiedImageError`
propagates — while mcme's index keeps the file opaque with a
warning (D32's record, standing) and the atlas is what refuses,
by name at exit 6. Both sides answer nothing; each is pinned to
its own way of saying so.

**Reverses if:** `create_atlas` grows a border argument (then
every placement and the page size shift by `2b` and the split
widths change), the perimeter sort stops being stable (then the
input-order tie-break disappears and mcme's sort becomes the only
order), `to_dict` drops `min(h, w)` (then non-square frames'
top edges become h and the tall fixtures change), the empty
input's formula stops answering 2, or the module learns
multi-frame textures (its `Texture` already carries a frame
list — one name could then own several bounds).

## D35 — texture coordinates in atlas space

**What the reference does.** The bounds rule sits one step above
the page: the manager seeds its texture table with
`("minecraft", "missing_no")` FIRST (`resource_pack_manager.py`
at the table's construction — the pin asserts the seed so it can
not silently stop working), the OpenGL layer's `setup()` runs
`create_atlas` over `mgr.textures` and stores the result as
`_texture_bounds`, and `texture_bounds(path)` answers the exact
key or else `bounds[missing_no]` (`resource_pack.py:78-83`). The
chunk builder (`chunk_builder_cy.pyx`, `_extend_blocks`) writes
each block mesh's rows as one float32 table of twelve columns:
the vertex position in model space (the block's world offset
added later, in both stream branches), the RAW texture coordinate
(the reader's own 0..1 space, unclamped — uv `[-4, 0, 20, 16]`
survives as coordinates `-0.25..1.25`, verified), the rectangle,
and the tint. The rectangle is written PER THREE ROWS — one
triangle — by stepping `model.texture_index[cull_dir]`, so a
bucket's rectangles change wherever the texture index changes
between faces; `texture_bounds` picks them. The tint column alone
is then folded with the per-direction `_brightness_multiplier`
and the height-band shade; position, coordinates and rectangle
are left alone. Rows go to the translucent array iff the mesh's
transparency is 1, and a face is skipped when its neighbour's
transparency is 0 (opaque) or when neighbour and self are both 1
— against air (no elements, transparency 2, verified) neither
clause fires. Cull buckets come from ONE line: `cull_dir =
face.get("cullface", None)`, then `if cull_dir not in FACE_KEYS:
cull_dir = None` — full-cube-ness does NOT infer culling, so a
cullface-less full cube answers the none bucket, while a model
with textures and no elements answers the missing BLOCK (the
loader's early return) — which is why the ring's empty model
carries no textures at all. And the mapping into the rectangle is
not Python anywhere: the fragment stage does it per fragment
(`render_chunk_330.frag`), `mix(b0, b1, mod(t, 1.0))` per axis —
a floor-based mod, so a coordinate below zero wraps up, never
clamps.

**What mcme does.** The atlas now ALWAYS packs mcme's own missing
image first — `mcme_atlas_build` synthesizes the sentinel (16x16
RGB, an 8x8-quadrant magenta/black checker, `f800f8` where
`(x < 8) == (y < 8)`, alpha ff — byte-identical to the reader's
`missing_no.png`, verified) and takes the sentinel plus the
caller's sorted paths, reversing D34's strip of the manager's
copy; the empty pack answers the sentinel's own 16x16 page, and
D34's 2x2 empty answer belonged to the stripped input and no
longer reaches either side. `mcme_atlas_bounds` answers the exact
key else the sentinel's rectangle — the reference's rule,
fallback included. The table (`McmeAtlasMesh`, `mcme_atlas_mesh`)
walks a mesh bucket by bucket in the face stream's own order —
six rows per face — and per row answers the rectangle its
triangle draws (`texture_indexes[f * 2 + k / 3]`, the same
three-row step) and the mesh's coordinates mapped into it:
`b0 + (b1 - b0) * (t - floor(t))`, the fragment stage's
arithmetic transcribed. The word for the sentinel is
`MCME_MISSING_TEXTURE` ("missing_no"), which the index and the
mesh answer alike. The verb `mcme atlasmesh <pack> <block>`
builds the block-level mesh through the shared
`build_block_mesh` prelude (D33's selection, mesh, turn, merge)
and answers the page's `size` plus each present direction's
`bounds` (4 per row) and `auvs` (2 per row) under `--json`, the
page line in human. The fixtures answer one namespace lesson:
a colon-less texture reference resolves to `minecraft`, not to
the model's own namespace — proven against the reference in both
directions (a file only under the model's namespace →
`missing_no`; the file only under `minecraft` → found), so the
selftest's textures live under `assets/minecraft/textures/`.

**How parity was measured.** `tools/ref_atlasmesh.py` (83
checks) drives the editor's own chunk builder headless: it loads
`textureatlas.py`, `resource_pack.py` (behind a shim standing in
for the package that imports wx) and the compiled
`chunk_builder_cy` extension by file path, builds an
`OpenGLResourcePack` whose `_texture_bounds` is exactly
`setup()`'s dict — `create_atlas` over the sentinel-first input
both sides are handed — and shadows `get_block_model` with the
manager's own method, which is the very call the pack makes
after its universal-to-java translation; translation is the one
step bypassed, recorded below. One block is placed at world
origin in an 18-cube grid with air in the ring (meshed at loop
zero, so its rows are the stream; each probe gets a fresh
palette and a cleared model cache, because the builder's cache
is keyed by `id()`), and the rows are sliced by the mesh's own
face counts in the builder's cull-id order — none, up, down,
north, east, south, west, the words mcme's JSON uses — with the
array chosen by the mesh's transparency. Per row the pin
compares position, coordinate and rectangle at float32 (1e-6;
every in-domain number is dyadic and would compare exact, but
the table is float32 and element rotation is not dyadic), then
recomputes the auv from the builder's OWN raw coordinate and
rectangle through the transcribed shader formula and compares
it with mcme's — the mesh document itself travels at 1e-9
(D33's ground, re-pinned on this verb so the new columns ride a
mesh both sides agree on), and the atlas size is mcme's `size`
against the reference image's. Eight crafted probes: the ring
block itself (an empty mesh, no rows), a cullface-less full
cube (the none bucket — `cull_dir = face.get("cullface", None)`
verbatim), the same cube with a cullface per face (six buckets,
the cull test walked beside air), a full cube on an alpha
texture (the translucent stream, the other array), three faces
on three textures inside one bucket (the three-row step), UVs
past both edges (the mod wraps, it does not clamp), a texture
the pack lacks (the sentinel's rectangle through the missing
machinery), and a multipart merge (two cubes' rows concatenated
in one bucket). The raw `java_vanilla_fix` adds 69 probes over
real game data — one per blockstate file at its first variant
key (the full key sweep is D33's ground), the completed pack
given the air ring it does not ship (missing, the reference
would answer the missing BLOCK and cull every face) — plus the
broken-image refusal pair (the reference never finishes loading,
Pillow refuses the file in its transparency-flag pass; mcme
builds the mesh and refuses by name at the atlas, exit 6) and
5 usage refusals (1 / 2 / 1 / 1 / 1, the block prelude's own).
`ref_atlas.py` migrated to the sentinel-first input: 74 checks.
The selftest's `test_atlases` was rewritten to the sentinel-first
layout (the doubling probe became a 20x2 image; the old trio no
longer forces the page to grow) and `test_atlas_uv` carries the
same reference-verified expectations from the C side, ASan/UBSan
clean. Gates: `./rebuild.sh test` PASS 5 suites, `./mcme
selftest` PASS 6 suites (0 failures), battery 18/18 rc=0.

**Out of domain, recorded.** The BRIGHTNESS-AND-SHADE fold: the
builder multiplies its tint column by the per-direction
brightness and the height-band shade; mcme's table carries the
mesh's tint as D32 answered it, and the pin skips the tint
columns entirely. The CULLING clauses: the pin keeps them
satisfied (air's transparency 2 fires neither) rather than
re-implementing them — every row the model has is a row both
sides answer, and the rule itself stays D33's ground. The
TRANSLATION step: `OpenGLResourcePack.get_block_model` first
maps universal-to-java through PyMCTranslate; the pin shadows it
with the manager's own method because mcme's domain is the java
pack (D33's record) — the single bypassed step, here said so.
The SENTRY IMAGE's carrier: the reference reads the file inside
the reader's package, mcme synthesizes the same bytes — the
bounds can not tell them apart, and the pin asserts the
byte-identity on the C side. UV UNCLAMPED: the reader does not
clamp element UVs; the wrap is the shader's mod at render, and
nothing repairs the coordinates on the way into the table. The
INPUT ORDER stands as D34 recorded it: both sides are handed the
sentinel plus the sorted paths, and the app's own feeding order
is out of domain. Blocks the reference tints are skipped by rule
and counted (the fix pack ships none that trip it). The
`id()`-keyed model cache the builder sets on the pack is a
hazard of driving it out of the app — the pin clears it per
probe — not a parity question.

**Reverses if:** the fragment stage's `mix(b0, b1, mod(t, 1.0))`
changes (then every auv follows it and the transcription with
it), `texture_bounds` stops falling back to the missing image,
the builder steps the rectangle per face instead of per triangle
(a texture change mid-face is unreachable in the java format
today — both triangles of a face share its texture — but a
format that allowed it would move rows), the manager stops
seeding `missing_no` first (the pin's input assertion fires),
the loader stops routing cull buckets by the `cullface`
attribute alone (then the cullface-less probes move), or the
sentinel's bytes change on either side (the selftest's
byte-identity check fires).

## D36 — the tint column: brightness and the band shade

**What the reference does.** The builder's table folds its last
column twice before the rows are done, and both folds multiply the
mesh's tint into the vertex's colour (`chunk_builder_cy.pyx`). First
the per-direction brightness — `_brightness_step = 0.15` and
`_brightness_multiplier` keyed by the cull direction: `None` and
`"up"` answer `(1,) * 3`, `"north"` and `"south"` `(1 - step,) * 3`,
`"east"` and `"west"` `(1 - step * 2,) * 3`, `"down"` `(1 - step *
3,) * 3` — greyscale, one scalar on all three components. Then the
band shade, applied in BOTH stream branches, after the world offset
lands on the position, to columns 9..12 only (lines 303-311 and
322-330):

    shade = ((y / 32) % 2)
    if shade > 1:
        shade = -shade + 2
    shade = 0.9 + 0.2 * shade

— a triangular wave in the vertex's WORLD height. The modulo is
numpy's float32 `%`, whose remainder carries the DIVISOR's sign; the
fold is `shade > 1`, so the peak belongs to t == 1 and the trough to
t == 0; the scale runs the light 0.9 to 1.1 and back every 64
blocks. The world height is the block's offset plus the vertex's
own: `block_array.dy` is `chunk_offset[1] + sub_chunk_y` (pyx
358-366), added to the position column before the shade reads the
stored position back.

**What mcme does.** The table grew the column: every bucket's rows
carry `tints` (3 per row), and `mcme_atlas_mesh` takes the block's
world height — `block_y` — as the fold's input. The brightness table
sits indexed by McmeCullDir, DOWN first: 0.55, 1.0, 0.85, 0.70,
0.85, 0.70, 1.0. The wave is `band_shade`: fmod into [0, 2), the
period added back below zero (numpy's remainder rule — C's fmod
would answer negative and slide under the trough), folded back over
1 above it, scaled 0.9 + 0.2. The fold runs the builder's order on
doubles: tint x brightness x shade. The mesh's tint under the fold
is D32's default (1, 1, 1); the block tints themselves stay out.
The verb grew `--y <n>` — default 0, any sign, bad or missing
values refusing with usage, exit 1 — because the shade reads one
number the block alone does not carry: x and z never enter the
fold, so `--y` is the one positional input the fold takes. `--json`
prints `tints` per direction beside `bounds` and `auvs`.

**How parity was measured.** `ref_atlasmesh.py` grew the tint
comparison: per row, mcme's `tints` against the builder's columns
9:12 at the table's float32 precision. The fold is NOT dyadic —
0.15, 0.85 and 0.9 are not powers of two — so the storage bound the
D35 pin carried for element rotation now earns its keep on every
tint row. The pin drives the builder at chunk offset [0, y, 0] (dy
= the offset plus the sub-chunk's own 0) and mcme at the matching
`--y`; the position comparison subtracts the offset back, the probe
still meshing at loop zero inside the grid's one-cell padding. The
cull cube (six buckets, all five brightness values) re-runs at
eight world heights: 16 (mid-ramp), 32 (the peak — t == 1, the
`> 1` fold not yet fired), 48 (past the top, folded back), 63
(just under the far side of the wave), 64 (the trough again — the
period), 96 (the next peak), and -1 and -32 (below zero, where
numpy's modulo and C's fmod part ways — fmod would answer -0.03125
and -1 and slide the light under the trough). Two `--y` usage
refusals join the five: 93 checks. The selftest's `test_atlas_uv`
grew the same expectations from the C side: the fold per row over
cube6's own vertex heights with literal anchors on the flat faces
(down 0.495 at the trough, up 0.90625 at the ramp's first notch)
and the wave over the block's height (0.605 at 32, 1.09375 for the
up face above it, 0.495 at 64, 0.605 and 0.4984375 below zero —
the fmod trap written as literals). Gates: battery 18/18 rc=0,
selftest 6 suites 0 failures under ASan+UBSan, `./rebuild.sh test`
PASS.

**Out of domain, recorded.** The BLOCK TINTS — grass, foliage, the
coloured wires — the mesh's own colour before any fold: mcme
answers the default (1, 1, 1) as D32 recorded it, and the pins skip
the blocks the reference tints by the reference's own tint
function, so the fold is compared only where its input agrees.
D35's BRIGHTNESS-AND-SHADE exclusion is SUPERSEDED — this drop is
that column, pinned. AO and the light columns: the builder does not
bake them into this table; what the editor shows for skylight and
block light is a different stage's answer. The x and z offsets:
they ride the position column in the builder and never enter the
fold, so `--y` takes the one number the shade reads, and the verb
says so.

**Reverses if:** the builder's step changes (the brightness table
follows it), the wave's constants move (0.9 + 0.2, the 32, the 64),
the fold's order changes (tint x brightness then x shade is what
the float32 stores round — mcme's doubles hold the product exactly,
so a reorder is invisible at this tolerance unless the reference
starts rounding differently), the fold reads the position BEFORE the
offset lands (then the world-height half of the pin's sweep moves),
numpy's modulo semantics change under the builder (the two
below-zero probes fire), or the reference tints a block mcme's tint
filter no longer skips (the vanilla sweep's tint comparison parts
company).

## D37 — the cull clauses: the rows that survive a neighbour

**What the reference does.** The builder decides which faces get
rows at all, per cull direction, over the neighbour's transparency
ALONE (`chunk_builder_cy.pyx` 279-289):

    for cull_id in range(7):
        if block_model.faces[cull_id]:
            if cull_id:
                ...
                if blocks[get_block(arr, dx, dy, dz)].is_transparent == 0 or \
                   blocks[...].is_transparent == blocks[get_block(arr, x_, y_, z_)].is_transparent == 1:
                    continue

Two clauses, and the test never runs for the none bucket — the
`if cull_id:` gate means a face with no cullface survives whatever
stands beside it, so a full cube without cullface keeps every face
even embedded in stone. Clause one: the neighbour is opaque
(`is_transparent == 0`) — transparency alone, no full-block or
shape condition anywhere in the test. Clause two: the chained
comparison, neighbour and self both translucent (== 1). Air
answers 2 and fires neither. The neighbour is read from the padded
grid, so a block at the sub-chunk's edge reads the padding cell.

**What mcme does.** `mcme_atlas_mesh` gained `beside` — a
McmeTransparency, the neighbour's own D32 classification — and a
bucket whose faces carry a cullface answers no rows when
`beside == OPAQUE`, or when `beside == TRANSLUCENT` and the mesh is
translucent too: dropped whole, before any allocation, so
`row_count` is 0, the arrays are NULL, and the verb prints the
mesh document's dir with no `bounds`/`auvs`/`tints` keys — no rows
for them to name. `present[]` stays the mesh's: the mesh still has
the bucket; the table answers nothing for it. The none bucket
always answers its rows. The verb grew `--beside <block>`: the
neighbour through the same block prelude — its refusal set, its
missing-BLOCK answer (opaque, and it culls, exactly what the
loader does) — the trailing flag pairs accepted in either order,
each once, a bare flag or a repeat refusing with usage, exit 1.
Default: nothing stands beside, the air answer, which fires
neither clause.

**How parity was measured.** `ref_atlasmesh.py` grew the field:
the neighbour fills the whole 18-cube grid — padding included, so
every cull read lands on it — except the probe's own cell at grid
(1,1,1). Nine crafted probes pin every branch: the cull cube and
the glass against the opaque and the translucent field (clause one
fires, neither fires, the chained pair fires), the cullface-less
cube standing under both (the exemption), the missing BLOCK
standing opaque on both sides, and the pack's own air answering the
default — each probe also asserts the reference answers the
neighbour's transparency alike on both sides (the field block's
flag and the spec mcme is told). The vanilla sweep repeats with
stone standing beside all 69 probes. The pin transcribes the two
clauses for its bucket slicing and checks the transcription
against the builder's own row total. 173 checks rc=0; the selftest
gained `cubea` (a translucent full cube) and the same branches
from the C side; battery 18/18, `./rebuild.sh test` PASS.

**Out of domain, recorded.** THE TRANSPARENCY RULE behind the
flags is D32's ground and green, but its shape is worth holding
on to: the reference marks a texture transparent only when its
PNG mode is exactly "RGBA" with any alpha != 255 — the fix pack's
shipped water, lava and banner PNGs are PALETTE images, so the
completed vanilla pack answers no block translucent at all (3
opaque, 66 partial). Real data therefore walks clause one and the
exemption; the chained pair is pinned on the crafted probes,
exact. THE SHAPE-VERSUS-FLAG quirk: the reference culls on the
flag alone, so a partial block with opaque textures culls its
neighbours' faces exactly like a full one would; the game's own
rule asks whether the neighbour's shape covers the face — the map
follows the reference, and the reference does not ask. PER-DIRECTION
neighbours stay out: the block-level verb carries one neighbour,
the same answer in every direction; the direction mapping itself
is D33/D35's ground. The builder's cross-sub-chunk neighbour
reads stay out: the pin's grid is self-contained.

**Reverses if:** the skip clauses change (a shape or full-block
condition joins the transparency test, the none bucket gains a
cull, the chained comparison parts ways), the field's leak
geometry changes (a different padding thickness, a different loop
order — the 18 rows and the head-slice both rest on them), the
culled bucket's JSON shape changes (table keys absent beside the
mesh document), or the prelude's missing-BLOCK answer stops being
opaque.

## D38 — the six neighbours: the table answers the world's own sides

**What the reference does.** The cull has always been PER FACE:
each face reads the neighbour standing in ITS own direction —
CULL_MAP (chunk_builder_cy.pyx 49-55) answers up, down, north,
east, south, west for cull_id 1..6, and D37's two skip clauses
run per face over that one lookup — so six faces can six answers.
The offset rides all three position components: a row's position
is the model's vertex plus `block_array.dx + x`, `dy + y`,
`dz + z` (310-313), the BlockArray's d being the chunk offset
plus the sub-chunk's own y (358-366). The band shade reads the
row's y alone.

**What mcme does.** `mcme_atlas_mesh_dirs(atlas, mesh, block_y,
beside[6], out)` takes one transparency per direction — DOWN
through WEST, the bucket order — and each cullface bucket reads
its own side's answer; the clauses and the none exemption
unchanged (D37). `mcme_atlas_mesh` keeps the uniform neighbour,
one block state on all six sides, now a wrapper that fills the
array. The verb `chunk mesh <world> <pack> <x> <y> <z>` reads the
seven block states out of the world itself — the target plus six
neighbours through the world's own read, `chunk get`'s pinned
ground — meshes each through the block prelude, and answers the
table with the world's own y. The target's chunk absent is exit
2; a NEIGHBOUR's chunk outside the stored world is air — the
world's edge stands open, partial, firing neither clause; any
other read failure is the read's own refusal. JSON carries the
mesh document plus the page's size, the block and the
coordinates; human adds the position to the heading and the six
side answers below the table.

**How parity was measured.** `tools/ref_chunkmesh.py` drives the
reference's builder on the WORLD'S OWN grid: the golden Java
world copied, the pattern written with `chunk set` (ref_java_blocks'
SET ground), the 3x3x3 around the probe read back by that pin's
own region walk — the sector table walked by hand, the palette
unpacked there, no mcme code — and handed to `create_lod0_chunk`
at the probe's world position, the sub-chunk's own y zero. The
interior is ONE cell: the builder meshes the probe alone, the
streams hold only its rows, and every cull read lands on the
world's own cell beside it — no leak arithmetic at all. The skip
clauses are transcribed per side over the walk's six neighbours;
five positions pin it: six different neighbours on six sides at
once (three clause outcomes, at the band's period height), the
chained pair on one side (the band's peak), a neighbour across
the chunk boundary into the golden world's chunk (1, 0), one
across the section edge into the section the write created, and
the cullface-less cube ringed by opaque all round. The row
comparison is D37's own; the position compare now rides the
offset on all three axes — the first pin to exercise nonzero x
and z. The crafted pack's flags assert the fixture's ground per
block; the usage refusals cover argc, a junk coordinate, the
missing world, the missing pack, the absent target chunk, a
dimension without a region directory, and a dimension number the
extractor refuses.

**Out of domain, recorded.** THE BEDROCK GRID: `chunk mesh` reads
Bedrock worlds through the same prelude and table — its world
read is `chunk get`'s pinned ground — but the pin's grid walk is
the region file's; the subchunk codec as a mesh source stays out
until a pin walks it. THE VERTEX CAPACITY: the one-cell interior
never overflows the builder's array, so the stream's split
machinery is the field pin's totals (D37), not this one's — a
whole sub-chunk's table rides those. THE WORLD'S EDGE as data:
the air answer for a neighbour outside the stored world is
mcme's own semantics — the reference's builder only ever sees
the grid it is handed — checked as the prelude's air (partial,
firing neither clause), not adjudicated against a reference
world loader. BLOCK TINTS stay D36's exclusion, the reference's
own tint function the gate; the probes stand in the tint domain.
The GUI's consumption of the table remains unobservable from any
pin.

**Reverses if:** the per-face read changes (a neighbour looked up
per BLOCK instead of per face, or CULL_MAP's offsets no longer
naming the cell each face reads), the offset stops riding all
three position components (dx + x parting ways with dy + y), the
prelude's air answer stops being partial, the edge rule changes
(an absent neighbour chunk refusing instead of reading air, or
the target's absent chunk reading air instead of refusing), or
D37's clauses change.

## D39 — the Bedrock grid: the other format's own data under the table

**What the reference does.** The reference's READER answers
Bedrock worlds from the store itself: the subchunk payload under
its key (x, z as two little-endian ints, then the sub-chunk's y
byte), the wrapper stripped (versions 1, 8, 9), each layer's
palette unpacked by amulet-core's own `_load_palette_blocks` —
name, states, version, in that order. The palette's name carries
the namespace (`minecraft:stone`), and an absent payload is the
store having nothing to say for that sub-chunk. The reference's
BUILDER takes the grid it is handed — the 3x3x3 around a probe,
the sub-chunk's own y zero — and its table is a function of that
grid alone: which format fed the grid never reaches it. So the
same six probes over the same pattern, one store per format,
must answer the same table, and do.

**What mcme does.** `chunk mesh` reads Bedrock worlds through the
same prelude and the same table it read Java worlds with — its
world read is `chunk get`'s pinned ground: the sub-chunk key,
the store's read, the layer's palette, the cell's index, the
spec. It reports the palette's name VERBATIM, namespace
included — what `chunk set` stored as given (D16), what
`chunk get` shows back. The store's edge keeps D38's rule on
both formats: the target's own sub-chunk unstored is exit 2,
what `chunk get` answers there; a NEIGHBOUR's unstored
sub-chunk is air, partial, firing neither clause. No C change:
the verb and the table were D38's, Bedrock-capable from the
start; this drop hands them the other format's data.

**How parity was measured.** `tools/ref_chunkmesh.py` writes the
same six probes into both stores' scratch copies with
`chunk set` — which on Bedrock CREATES the sub-chunks it needs
(ref_bedrock_write's ground) — and writes EVERY cell of each
probe's 3x3x3: the probe at the centre, the named sides on their
axes, air everywhere else, all in the full `minecraft:name`
spelling the stores hold, so the grid the reference meshes is
the pattern itself and the fixture's own blocks never enter a
palette the crafted pack cannot resolve. The world-edge probe's
column at x = 32 stays unwritten: chunk (2, 0) is the store's to
answer, absent. Each grid is read back by its own walk —
ref_java_blocks' region walk for Java, the sub-chunk payloads
through amulet-core's loader for Bedrock (ref_bedrock_write's
own glue: the key, the wrapper strip, the palette reader), both
reporting the store verbatim — and handed to
`create_lod0_chunk` at the probe's world position; mcme answers
`chunk mesh` on the same world; the comparison is D38's own,
unchanged. 15 checks: the crafted pack's flags, six probes per
format, the two walks' edge cells answering air, and the usage
refusals — the ninth an unstored Bedrock sub-chunk, exit 2.
The pin's first Bedrock run disagreed over the namespace alone,
and it was the pin's own doing: a walk that re-added what the
store already carries. The store's spelling is the answer, both
walks and mcme now spell it, and the probes write full names as
real worlds hold.

**Out of domain, recorded.** BLOCK STATES ON BEDROCK: the probes
carry no states, so the store's state spelling through the mesh
path stays ref_bedrock_write's ground until a probe carries one.
LAYERS: the fixtures' payloads are single-layer; the grid takes
the first, waterlogging D15's ground. THE VERTEX CAPACITY stays
D38's exclusion — the one-cell interior never overflows the
builder's array, the stream's split machinery a whole
sub-chunk's table's. THE WORLD'S EDGE as reference data:
absent-payload (Bedrock) and absent-chunk (Java) reading air is
mcme's own semantics — the reference's builder only ever sees
the grid it is handed — now pinned on BOTH formats as the
walk's and mcme's shared answer, not adjudicated against a
reference world loader. BLOCK TINTS stay D36's exclusion. The
GUI's consumption of the table remains unobservable from any
pin.

**Reverses if:** either read stops reporting the palette's name
verbatim (the namespace stripped or re-added on either side),
`chunk set` stops storing names as given (D16) or stops
creating the sub-chunks it needs on Bedrock, the edge rule
changes on either format (a neighbour's unstored sub-chunk
refusing instead of reading air, or the target's reading air
instead of refusing), the loader's wrapper handling changes (the
1 / 8 / 9 strips), or D38's table changes.

## D40 — The whole sub-chunk: the table at sub-chunk scale

**What the reference does.** The builder's loop over a grid is
x, then y, then z (chunk_builder_cy.pyx 273-275), every interior
cell meshed through its own six-direction cull (D38's per-side
clauses, unchanged), each cell's rows landing in the stream its
transparency feeds — and the streams FLUSH: past 10,008 rows
(ARRAY_VERT_COUNT, twelve floats a row) the builder starts a new
container and returns every container flattened into one array
per stream. So a sub-chunk whose cells keep their faces at scale
— the cullface-less wall, partial shells against each other —
crosses the split, and the table's answer is the whole of it:
every surviving row of every cell, in loop order, the totals
whole across the containers. The reference's page is the pack's
texture table with the missing image seeded first,
perimeter-sorted (ties keeping input order), split-packed into
the smallest power-of-two page that holds it — and every stream
row carries the rectangle its face draws from, laid by that
page.

**What mcme does.** `chunk submesh <world> <pack> <x> <y> <z>`
answers the table for the WHOLE sub-chunk holding the position:
the seven layers loaded once (the target chunk carrying its
neighbour sections, at most five chunk loads, absent neighbours
the world's-edge air and the target's absence exit 2, D38's
edge rule at sub-chunk scale), composed into an 18x18x18 grid —
the cells with one layer of every side — the distinct states
meshed once through the prelude, the page built from the pack's
one texture table (every state's index is that same table; one
answers, and concatenating them would pack every texture over
and over), and the builder's own loop over the interior calling
the per-side table with each cell's six real neighbours. The
Java section's grid is read in one call
(`mcme_java_section_grid`, the section's own x + z*16 + y*256
order) and transcribed into the layers' Bedrock order
(x*256 + z*16 + y) once at adoption, so one formula serves
every meshing read on both formats. JSON carries the page's
size, the sub-chunk's coordinates, both streams' totals and the
cells that survive the cull — position, block, and the dirs'
bounds/auvs/tints; a faceless cell is not listed. Human prints
the totals and the seven buckets' row counts.

**How parity was measured.** `tools/ref_chunkmesh.py` fills
sub-chunk (0, 8, 0) — y 128 to 143, past every probe — with six
`chunk fill` boxes per format, 7,708 cells each: the ring_opaque
field; the z 0-3 wall of atlas_cube, whose none bucket keeps all
36 rows of its 1,024 cells (the opaque stream crossing the
10,008-row split); the atlas_three shell; the ring_translucent
core; the air cavern; the second run. The walk the reference
reads with caches what it reads, so the pattern lands before the
walks are built; its grid is the 18x18x18 of the walk's own
reads — a neighbour column in a region file that does not exist
is the world's edge, air, the rule mcme reads it by — handed to
`create_lod0_chunk` at the sub-chunk's world origin; the pin
walks the builder's loop order itself, cursors per stream, and
compares every surviving row (position, tvert, bounds, shader
auv, tint), the per-cell block against the walk's own answer,
the stream totals, the cell order, and the split — the wall's
rows counted past the container boundary. The same six boxes go
through each format's own write path. 17 checks rc=0. The pin
caught four mcme bugs, two of them invisible to the totals: the
none bucket (index 6) never scanned, so every all-none cell
dropped out; the east padding composed into the interior's own
x = 15 column, overwriting those cells with the side's spec; the
per-state texture concatenation multiplying the pack's table
fivefold and shifting every rectangle; and Java's section order
read through Bedrock's layer formula — x and y transposed,
which per-block totals survive (the transposition bijects the
cells and the cull counts permute with the directions) and only
the per-cell block check caught.

**Out of domain, recorded.** BLOCK STATES ON BEDROCK stay
ref_bedrock_write's ground (D39's exclusion) — the fills carry
none. MULTI-LAYER PAYLOADS stay D15's: the layers take the
first, waterlogging unexercised. BLOCK TINTS stay D36's. THE
VERTEX CAPACITY's overflow handling is now pinned through the
split (10,008-row containers, flattened) — what remains
unobservable is the renderer's own cap beyond the builder's
flush. THE PAGE LAYOUT beyond the pack's own table: the pin
adjudicates rectangles on the crafted pack's table; a pack
whose textures overflow the doubling page-growth loop stays
unpinned. THE GUI's consumption of the sub-chunk answer remains
unobservable from any pin.

**Reverses if:** the builder's loop order changes (x, then y,
then z), the stream split's threshold or flattening changes
(10,008 rows a container, containers concatenated), any of the
table's clauses change on any of the seven buckets (the none
bucket's exemption included), the pack's texture table stops
being the page's whole input (one index per build, the missing
image seeded first, the perimeter sort and split pack), the
edge rule changes at sub-chunk scale (a neighbour's unstored
chunk or section reading anything but air, the target's
refusing), the Java section's transcription into the layers'
order changes, or D33-D39's ground changes beneath them.

## D41 — the states: one name, three models, through every read

What the reference does. A palette entry carries a name AND its
states, and the manager resolves the blockstate file's variant
keys first-match in file order (D30's ground: each key's
properties must all match, extra block properties allowed, the
un-stated key the last resort) — so the SAME name meshes three
different ways depending on the states written beside it. The
walks spell the entry back verbatim: name plus `k: "v"` pairs,
keys sorted case-insensitively — the palette's own values,
quoted, on both formats.

What mcme does. Nothing new — and that is the finding. `chunk
set` stores name and states (D16), `chunk get` spells them back
round-trip, the world reads produce the same text
(mcme_bedrock_block_spec on Bedrock, the Java entry_spec, both
SNBT with the walk's own spacing), and the mesh verbs already
parse that spec through D30's selection into the variant's own
model. Every line carried states already; what was missing was
a probe — the pack had no blockstate with variant keys, so the
states rode all of it unobserved.

How parity was measured. The crafted pack gains ONE blockstate —
atlas_turn: axis=x → three_up (partial, none-bucket rows),
axis=y → cube_red (opaque, no cullface), the un-stated default →
ring_cube (opaque, six buckets) — three models the pack already
holds, so its texture table and every page stay D34's exactly
and no bounds pin moves. Four probes per format: two variants,
the un-stated default, and a probe whose stateful NEIGHBOUR's
variant decides its own transparency — atlas_cull's east faces
stand against three_up's partial, and a dropped state there
would read the default ring_cube, opaque, and lose six rows to
the cull. All four written with `chunk set` (261 cells now, 108
more), read back verbatim by both walks, compared through
`chunk mesh`'s document and the builder's table — and the
probes' own sub-chunk (0, 6, 0) through `chunk submesh` on both
formats, the batched read carrying the same specs, the
sub-chunk helpers parameterized by sub with D40's defaults
intact and the split assertion kept where the wall stands. 27
checks rc=0 on the first run; battery rc=0; no mcme change.

Out of domain, recorded. MULTI-KEY STATE ORDER: the walks sort
keys case-insensitively when spelling, mcme's SNBT emits stored
order — one-key probes spell identically by construction, two-
key spelling parity unpinned. NON-STRING STATES: the walk quotes
every value; SNBT types them (`1b` against `"1"`) — the
fixtures' states are strings; int/bool states through the mesh
reads unpinned. The store's typed states through Bedrock read
stay ref_bedrock_write's ground. Multi-layer payloads (D15),
block tints (D36), and the renderer's consumption stand where
they were.

Reverses if: the variant grammar changes (first-match in file
order, subset match, the un-stated key last), the walks' verbatim
spelling changes on either format, `chunk set` stops storing
states or `chunk get` stops spelling them back (D16), the world
reads stop emitting the SNBT spelling the prelude parses, or the
pack's texture table grows a texture — atlas_turn deliberately
adds none.

## D42 — the second layer: the store holds water no mesh reads

What the reference does. A Bedrock sub-chunk payload may carry
several layers — version 8's layer count byte, version 9's count
and y index — and the loader (amulet-core's own
`base_leveldb_interface`) decodes every one, merging each cell's
layers into a main block plus extra blocks (the universal
format's `Block.extra_blocks`) and dropping an extra that is
air. The renderer's builder then reads none of them:
`chunk_builder_cy.pyx` carries no reference to extra blocks
anywhere; the editor hands it cell arrays and the chunk's
palette, whose entry is the main block. Waterlogging is
invisible to the reference's mesh — not by omission but by
construction.

What mcme does. The same, by the rule D15 recorded: the layers
take the first. `chunk get`, `chunk mesh` and `chunk submesh`
answer over layer zero; the codec parses the layer count — the
store's edge rule holds at multi-layer payloads too — and reads
the first. What was missing was the fixture: no write path of
mcme's reaches a second layer, so the rule rode every line
unobserved — the same shape of gap D41 closed for states.

How parity was measured. A two-layer sub-chunk written straight
into the scratch store's own bytes — version 8, the layer count
the reference's own loader reads — the payload's packed array
and palette NBT in `_encode_packed_array` and
`_load_palette_blocks`' shapes, the round-trip proven against
the loader itself before the pin ran. One cell waterlogged
(ring_opaque under ring_translucent), a plain atlas_cull
neighbour beside it, sub-chunk (0, 13, 0) where no probe, fill
or usage case stands (the key asserted absent before the write,
the golden store's own content safe). The walk reports the main
block by name and the layers beneath verbatim (`extras_at`);
the reference's streams over the sub-chunk's grid hold the main
block's rows alone — the translucent stream whole empty,
asserted; mcme answers the same over the whole sub-chunk
through `chunk submesh` and at the cell itself through `chunk
mesh`, where the cull against the waterlogged neighbour fires
on the main block's own transparency: a read that surfaced the
water anywhere would change the totals or the standing faces.
29 checks rc=0; battery rc=0; no mcme change.

Out of domain, recorded. The multi-key state order and the
non-string state spellings (D41's leftovers) stand. Java
waterlogging is a `waterlogged` property — states ground, not
layers. The store's typed states through the Bedrock read stay
ref_bedrock_write's. Block tints (D36) stand where they were.
The renderer's consumption stands deeper now: the builder's
palette never even sees the extra, so no pin can reach the
renderer's treatment of waterlogging through this ground.

Reverses if: the loader's layer rules change (v8/v9 counts, the
air-extra filter), the builder starts reading extra blocks, the
editor's `_sub_chunks` feeds layers rather than cell arrays,
mcme's read stops taking the first layer (the codec's layer
count changes or `chunk get` answers a deeper one), or the
store's edge rule moves the fixture's key.

## D43 — the spellings: what the store typed, the selection answers around

What the reference does. The spec format's conventions, at the
two points both readers meet it: keys in canonical order —
case-insensitive, the store keeping its insertion order and both
spellings drawing the same text from it — and values as the
store's own tags. A variant key matches by comparing the
property's py_data against the key's text: a string compares
equal; a typed value (Bedrock's bytes, ints, and the rest) is
never equal to text, so a key naming a typed state never
matches, while a property the block LACKS matches by the
default-fill (D30's quirk, still standing). Multipart sees the
same truth from the other side: a typed property is in no value
list, like an absent one.

What mcme does. The store side already answered: set stores what
it parsed, typed tags included, on both formats; get spells them
back round-trip, keys sorted, a byte bare with its suffix. What
didn't: the mesh path refused typed states outright — "state
'bit' is not a string — Java blockstate properties are strings",
exit 1 — so a Bedrock world whose palette carried a typed state
(most of them: upside_down_bit and the like) could not be
meshed at all, while the reference meshed the default variant
and moved on. And the walks quoted every value, so their report
and mcme's could never meet at a typed state. One echo leaked in
the middle: the blockstate verb rebuilt its spec line from the
string values alone, so a typed state printed glibc's `(null)`.

How parity was measured. Two probes per format carry the
spellings through `chunk set`, both walks' verbatim spelling,
and the table: one written unsorted with a mixed-case key — the
sorted text is a claim against the store's own order, and the
key's place (Zz after tf) pins that the walks' case-insensitive
rule and mcme's coincide — and one typed (`bit: 1b, num: 5`),
the byte and the int spelled as the writer prints them, the
variant still selecting on axis so a read that mangled the
states would change every row. mcme's fix follows the
reference's matcher: the CLI carries a typed state with a NULL
value — present in the props, and the variants pair and both
multipart arms treat a present-but-NULL state as never equal,
while absent still matches by the default-fill. The blockstate
echo spells a typed state from its tag's own SNBT, so what was
asked comes back. The selftest pins the rule against the quirk
fixture directly: {a: "1", b: typed} answers the second key,
{a: typed} answers missing. Three usage pins that had pinned the
old refusal (ref_blockstate, ref_getmodel, ref_atlasmesh) now
pin the reference's answer — the typed state meshing the
stateless one, naming no variant key. 33 checks rc=0; battery
rc=0.

Out of domain, recorded. A variant key that NAMES a typed state
stays unpinned through the pin's own fidelity limit: the pin's
props are strings parsed from the walk's spelling, so its
reference side would match "bit=1b" where the real reference
would not — the probes keep typed states unnamed by design. The
tint lookup's property reads (D36) and the atlas page's
doubling overflow stand where they were.

Reverses if: the spec format's canonical order or typed
spellings change (the writer's suffixes, the case-insensitive
sort), the reference's matcher changes its py_data-against-text
comparison or its default-fill quirk, the CLI starts coercing
typed states instead of carrying them, or the walks stop
reporting the store's types.

## D44 — the tints: the lookup's colours, the faces that carry them

What the reference does. `_get_block_tint` answers one block
state's colour before any fold: redstone_wire's `power` and the
stems' `age` index their tables by the state's STRING value —
inside the minecraft namespace only — and the name table answers
by the block's full namespaced name, defaulting (1, 1, 1). Both
state-keyed lookups can raise, and do: a state the block lacks
(KeyError), a state the store typed (AttributeError — D43's
finding, reaching the tints too), a value no table holds
(KeyError) — raised inside each apply's own try, caught there,
so every apply is refused and the walk falls through to the
missing block (variants) or the empty merge (multipart). The
colour lands only where the model's face declares `tintindex` —
presence, whatever the value — on that face's four vertices;
every other face carries (1, 1, 1).

What mcme does. The lookup is `mcme_block_tint`
(src/resource/tint.c): the three tables transcribed as their own
bytes divided by 255 — the same IEEE products `_get_colour`
computes — and the rule's shape kept: the state branches only
inside minecraft, only for a present string state, a miss
answering false. The mesh builder's faces had 1.0 hardcoded; a
face declaring tintindex now takes the block's colour, and the
block-level walk hands it there before selection (one wiring
point: every block verb funnels through the one prelude). A
false from the lookup raises the walk's skip flag, refusing
every apply — which the walk's own fall-through already turns
into the missing block or the empty merge, no special case. The
model-direct verb builds with (1, 1, 1): a model carries no
block, so there is nothing to look up.

How parity was measured. The crafted pack grew a full cube whose
up face alone declares a tintindex, on the existing texture so
the page stands as pinned, under five blocks: grass_block and
birch_leaves (two name-table constants), redstone_wire at power
5 and 15, pumpkin_stem at age 7 (the three state branches), and
atlas_tint — a name no table holds, its tinted face white, the
table's default beside the presence rule. Two probes pin the
raises: power "99" (outside the table) and power absent, where
both sides answer the missing block. Every probe rides the fold:
the reference's builder reads the model's tint_verts, mcme's
table carries its own, and the tinted rows must meet the
reference's colour times brightness times shade under D36's
machinery. The vanilla sweep no longer skips the reference's
tinted blocks — it reports them (69 probes, 0 tinted: the
shipped fix pack holds no tintindex models). The selftest pins
the table direct and the tint through the walk — the raise
answering the missing block for a variant and the empty merge
for a multipart stem. 182 checks rc=0; battery rc=0; selftest
PASS 6 suites.

Out of domain, recorded. The pin's props are strings, so the
Bedrock-typed power — a byte, where the reference raises
AttributeError — is answered at the table level (the typed
state's false) but not probed through the pin's own reference
side, whose props are text: the same fidelity limit D43's typed
states carry. A multipart whose cases all raise is pinned empty
by construction; a partial raise cannot occur — the lookup is
per block, not per case. The atlas page's doubling overflow
stands where it was.

Reverses if: the reference's three tint tables change, the
tintindex gate moves from presence to a value, the raise's
catch changes shape (answering something other than the
walked-through fall-through), or the block-level walk stops
handing the build its tint.

## D45 — the growth loop: the page that was too small, twice

What the reference does. `create_atlas` estimates the page from
the frames alone — max(max width, max height, the smallest power
of two at least ceil(sqrt(total pixels))) — and packs: the
perimeter sort (stable, ties keeping the input order), then the
first-fit region tree, each taken frame splitting its region
into the strip below (the frame's width) and the strip right
(the region's full height). A frame no free region holds raises
AtlasTooSmall; the loop catches it, doubles the size, and
retries from a FRESH root — every frame re-placed, the previous
attempt's coordinates overwritten. A frame needs both its
dimensions at most the region's; the bound is exact.

What mcme does. Nothing new — the mirror has held since D34:
the same estimate including the empty answer of 2, the same
sort with the input index as the tie-break, the same tree, the
same for(;;) with size *= 2 and a fresh root each pass. What
was missing was the GROUND: no probe ever fired the loop. The
crafted "double" pack predates D35's sentinel — with the
manager's own 16x16 image in the input the estimate is 32 and
the three small frames fit — so the growth path carried zero
coverage on both sides while every small layout was pinned to
the pixel.

How parity was measured. Two crafted packs whose geometry the
brute force found against the reference's own create_atlas (the
scratch generated real PNGs over ~2k adversarial frame sets and
measured the final page against the estimate formula): grow — a
48x2, a 2x48 and a 32x16 with the sentinel, where the widest
frame sets the estimate's FLOOR at 48 and the first-fit tree
strands the tall one, doubling to 96; and grow2 — a 31x5, a
28x6, a 7x29 and a 27x8 with the sentinel, where the area term
answers 32 and the tree wastes strips at 32 and again at 64,
two growth steps to 128. Both were verified under the pin's
input shape (sorted names, the sentinel sorting last by
perimeter) before entering the pin. The pin compares the page
side, every rectangle at 1e-9, and every pixel after every
retry; the selftest pins the same steps C-side with the
reference's own placements as literals — the sentinel landing
last, in the strip the grown page leaves behind. 89 checks
rc=0; battery rc=0.

Out of domain, recorded. Three or more doublings share the
loop's single body — the retry is one for(;;) — and the
geometry search stopped at two, so a pack needing a third is
unpinned by choice, not by gap. The GUI's consumption of the
sub-chunk answer remains unobservable (D40's record) and stands
where it was.

Reverses if: the estimate formula changes, the tree's split or
its first-fit order changes, the retry stops rebuilding the
root or stops re-placing every frame from scratch, or the
perimeter sort's tie-break moves off the input order.

## D46 — the typed tint state: the raise converges, both ways

What the reference does. `_get_block_tint` reads redstone's
power with `properties["power"].py_str` — a method only
StringTag carries. A byte in the store raises AttributeError
inside the apply's own try, caught there; every apply refused;
the walk falls through to the missing block (variants) or the
empty merge (multipart). D44 pinned the rule at the table level,
where a missing, typed, or unlisted state answers false. What
stayed unpinned was the REAL store's byte: the pin's own props
are text (D43's fidelity limit), so its reference side could
only reach the table-miss branch — KeyError on "5b" — never the
AttributeError the store's byte raises.

What mcme does. Nothing new — D44's raise already answers the
missing block for a typed NULL power, on both formats' stores.
What was missing is the proof that the two branches CONVERGE:
the table-miss (string "5b") and the attribute-miss (byte 5b)
are different exceptions at the same line, caught by the same
try, leaving the same fall-through. The pin proves the
convergence instead of recording around it.

How parity was measured. One probe, `redstone_wire{power: 5b}`,
at (8, 27, 8) — a free band beside D43's spelling probes, in the
sub-chunk none of the fills touch. `chunk set` writes it with
the store's own byte on both formats (D43's typed-write ground);
the java region walk and the bedrock subchunk payload each read
it back verbatim; the pin's reference side takes the walk's
spelling ("5b" as text — the table miss) and mcme reads the byte
(its typed NULL — the attribute miss), and both sides answer
the missing block: the mesh document and every row of the
builder's table, compared on both formats. 35 checks rc=0;
battery rc=0.

Out of domain, recorded. D43's variant-key limit stands and is a
different shape: a variant key that NAMES a typed state stays
unpinned through the pin's string fidelity — the pin's reference
side would match "bit=1b" where the real reference would not.
The raise converges because both exceptions leave the same
fall-through; the matcher does not converge, because the string
comparison itself is the answer. The GUI's consumption of the
sub-chunk answer remains unobservable (D40's record).

Reverses if: the reference's tint lookup stops raising on a
typed state (then the missing-block answer moves), the catch
stops being per-apply (then a raise would propagate instead of
falling through), or the store stops round-tripping the byte
(`power: 5b` would spell differently and the probe's walk side
would part from mcme's).

## D47 — the pin's own fidelity: the palette's tags, not the spelling

What the reference does. The variant matcher compares a key's
text against the block's property TAGS: a StringTag equals the
same text, a byte never equals any string — whichever way the
comparison is written (`str(py_data)` or tag equality), the
byte answers neither `1` nor `1b` against the key `1b`. D41
pinned first-match and the default-fill quirk; D43 pinned that
a key naming a typed state never matches — but only at the C
level, because the pin's reference side took the walk's
SPELLING and re-parsed it into StringTags: props were text
before the reference ever saw them, so a store byte arrived as
the string "1b" and the pin's own reference would have
MATCHED the key `bit=1b` where the real one does not. That was
recorded as out of domain.

What mcme does. Nothing new — D43's matcher already answers by
types (a typed value never equals key text, a string equals
string), and both formats' stores round-trip tag types (D43's
write/read ground). What changed is the PIN: every reference
answer over a store now starts from the walk's palette entry
itself. `block_entry` in the region walk exposes the entry,
both walks gain `tags_at`, and `make_block_tags` /
`ref_get_model(tags=True)` build the reference's blocks without
the TAG_String wrap — the probe grids, the per-side
transparency classifications, and the sub-chunk's 18-cube
palette all riding the store's own tags.

How parity was measured. A crafted blockstate `typed_key` with
two variant keys: `bit=1b` as text first, the un-stated
default second, both pointing at models the pack already holds
(cube_cull, cube_red — the texture table and its page stay
D34's). Two probes on both formats: `typed_key{bit: 1b}` — the
store's byte, no key matches, the default answers cube_red's
none bucket; `typed_key{bit: "1b"}` — the store's string, the
key matches, cube_cull's six. The mesh document and every row
of the builder's table are compared through both walks' tags,
java and bedrock alike: 39 checks rc=0 (was 35); battery rc=0.
The D46 probe rides the same path and now raises the record's
REAL AttributeError (`ByteTag has no py_str`) instead of the
string-side KeyError — same per-apply catch, same missing
block, executed rather than argued.

Out of domain, recorded. The store-side carve-outs are closed:
the typed tint state (D46) and this matcher limit both answer
from the store's own tags now. ref_getmodel's pack-level probes
keep text props by design — no store stands between them and
the lookup, so the fidelity limit never applied there. D40's
GUI consumption of the sub-chunk answer remains unobservable.

Reverses if: the reference's matcher starts stringifying typed
states before comparing (a byte would begin matching `bit=1b`
and the default's answer would move), a walk stops handing its
palette's tags over (the StringTag wrap returns and the pin
reverts to recording around the limit), or the store stops
round-tripping the byte (`typed_key{bit: 1b}` would spell — and
read — differently, and the pair's typed half would part).

---

## D48 — the GUI builds beside the core build

**Chosen.** `./mcgui` is built by `./rebuild_gui.sh`, not by
build.c: the build script, `cli.h`, `bedrock_write.h`, and
`leveldb/log.c` belong to another session's work and mcme's
drops never stage them. The script compiles `gui/*.c` with
`pkg-config`'s GTK4 flags — the system include dirs rewritten
to `-isystem`, because the toolkit's headers are not ours to
lint with `-Werror` — and links them against the core objects
`./rebuild.sh` already produced: everything but the CLI's
`src/app/main.c`, the vendored units included, the same
`-lm -lpthread -lz`, and the same debug sanitizers the core
build uses. The GUI's sources live at `gui/` at the repository
root, because build.c collects every `.c` under `src/` into
`./mcme` and GTK's flags are not in its vocabulary.

**Why.** One flag set per side, neither side guessing at the
other: the core's flags stay where they live (build.c), the
GUI's live in its own script, and the core objects the GUI
links are exactly the ones the CLI was tested with. The
alternative — GUI sources under `src/` — would put GTK headers
into the CLI's build; the alternative — editing build.c —
belongs to the session that owns it. The script clears its own
object dir before compiling, so a gui source that went away
cannot keep linking.

How it was verified. `./rebuild_gui.sh` builds clean under
`-Werror`; `./mcgui --selftest` runs the wire's checks headless
(no display, no `gtk_init`) and asserts its own allocations are
gone through a recoverable leak check, the binary's default
turning LeakSanitizer off for GTK's process-lifetime caches;
`--smoke` under broadwayd opens the window and quits at 300 ms,
rc=0; `./rebuild.sh test` and the pin battery are unchanged
(5 suites, 19/19) — the core bytes did not move.

Reverses if: build.c becomes ours — the script retires into
build.c's targets (a `gui` option linking `./mcgui` beside
`./mcme`, an exclusion so `gui/` may move under `src/`), or
D3's client shape changes to the GUI spawning `./mcme` instead
of linking the core — then the link step goes away entirely
and only the compile of `gui/*.c` remains.

---

## D49 — the client's seam: the core function, not the command's stdout

**Chosen.** The GUI calls the same core function the CLI's
command dispatches to — `mcme pack info`'s whole computation
is `mcme_pack_open` (D3 in practice) — and renders its own
presentation of the answer: the same four lines the command
prints, formatted in the GUI (`gui/packtask.c`), the mirroring
`cmd_pack_info`'s own --json comment already blessed (one
schema, two presentations). The command stays a thin stdout
wrapper in `src/app/main.c`; nothing in the CLI changed.

**Why.** A command's printf belongs to its frontend; the
reading does not. Two frontends over one function can never
disagree about what a pack IS — they can only dress it
differently — and the GUI's worker path (glib thread →
mcme_pack_open → the wire → the visit's label) needs the
answer as data, not as bytes on a pipe. The refusal travels
the same road: `mcme_last_error()`'s text copied immediately
after the failed call, because the core's error slot is one
global and only the worker calls core code while the loop
thread waits.

How it was verified. `./mcgui --selftest`: a crafted
pack.mcmeta in a temp dir summarized headless — the four
lines, format 34, the description — the named refusal for a
directory that is not a pack, and the whole client path
(worker → wire → loop visit) with no display; 1041 checks
rc=0 (was 1029). `--smoke build/blockmesh_fixtures/pack`
rc=0 (the answer lands, the exit code carries ok) and
`--smoke /tmp/mcme-no-such-pack-dir` rc=1 (the refusal
lands) under broadwayd; battery 19/19 and
`./rebuild.sh test` PASS — the CLI bytes did not move.

Reverses if: a command's computation can not be lifted out of
its stdout wrapper (then the GUI either refactors the command
to produce-then-print — touching `src/app/main.c`'s shape —
or spawns `./mcme` and parses its stdout, the
client-by-subprocess shape), or the core's readers grow state
that makes them main-thread-only (then the worker path needs
the thread pool, `mcme_tp`, and the seam moves down a layer).
