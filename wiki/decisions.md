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
