# Dev log

Newest entry at the top. Append; do not rewrite history.

Status vocabulary:
**scaffold** = files exist, not functional · **wip** = being written ·
**works** = runs, may lack tests · **done** = runs and covered by tests ·
**blocked** = needs something first

---

## 2026-10-13 — Java fill: `chunk fill` for both worlds

### Built

- `chunk fill` answers for Java worlds now, same
  box, corners in any order, the probe deciding the
  half that edits — Bedrock's body moved into its own
  branch unchanged, Java's into `java_fill`. A script
  fills one world or the other without knowing which.
- The box goes through the palettes as one pass per
  section, the unit the fill exists for: decode the
  section's 4096 entries once, point the box's
  positions at the target entry, repack once. A box a
  million blocks long costs one loop per section it
  touches, not a palette edit per block. The target is
  reused from the palette when it holds the block and
  appended when it does not — the append-only rule
  `set` established, one truth for both verbs.
- A section the box covers end to end lands the
  format's own shorthand for one block: a single-entry
  palette and no data array, the stale one gone with
  the compound it was written in — what the reference
  itself writes for a section that holds one block.
  The air rules carry over unchanged: air where no
  section covers invents nothing, and a fill that
  changes nothing rewrites nothing — `changed` false,
  the CLI skips the write, every region file keeps its
  bytes.
- The fill runs in two passes over every region file
  the box touches — check, then write — so one bad
  chunk refuses the whole fill with nothing written
  anywhere. Here Java parts company with Bedrock
  fill, which creates the subchunks it needs: a chunk
  document carries bookkeeping (`Status`,
  `Heightmaps`) mcme will not invent, so a chunk the
  region file does not have is exit 2 for the whole
  box. What is promised stops at the pass boundary,
  like Bedrock's honesty across puts: a chunk gone
  between check and fill leaves the earlier writes
  already landed. A region handle never survives its
  own write — open, read, close, then append — so no
  table is ever read stale.
- Refusals ride the shared vocabulary
  (`USAGE`/`NOT_THERE`/`NOT_READABLE`/`INTERNAL`/
  `STORE`) into the same exit codes the rest of the
  write verbs speak; the check's own message stands,
  the write's is wrapped in "chunk: " the way
  Bedrock's is.
- **The bug the ref tool caught before the commit:**
  a packing slot holds one position *of its chunk* —
  `(18, 44, 3)` shares slot sx=2 with in-box
  `(2, 44, 3)`, but they are different chunks' trees.
  The first version tested the box against the residue
  class, so a box reaching a neighbour's column moved
  this chunk's slot too. `mcme_java_fill_box` now
  takes the chunk's `cx, cz` and tests the box against
  the chunk's own world columns, in 64-bit arithmetic
  so a far chunk cannot overflow them. A selftest case
  (a box entirely past this tree's columns) pins it.

### Cross-check

- `tools/ref_java_fill.py`, 8 checks, all on copies
  of the golden worlds, read with no mcme code — its
  own sector-table walk, its own palette unpacking.
  ABSENT: a box reaching a region file the world does
  not have, and a box reaching a chunk the file does
  not have, both exit 2 with every region file byte
  for byte unchanged. FILL: a whole-section box lands
  one entry and no data array, and the walk reads the
  block at every position of the box. NO-OP: the same
  fill again, and air on an all-air section, leave the
  region file identical. PARTIAL: the box's edges and
  the section's own edges keep their air, the palette
  is air plus the block with its Properties read back,
  and `chunk get` agrees with the walk. CROSS: a box
  spanning two stored chunks fills both and every
  neighbour — chunk, section, edge — keeps its block.
  CROSS-FILE: a box in `r.-1.-1.mca` fills chunk
  (-1, -1) there, shorthand included. ROOM: usage and
  absence codes, and Bedrock fill still writes what
  its own `get` reads back after the restructure.
- Selftest `test_java_fill`: partial boxes with
  corners in either order, target reuse without
  palette growth, the box-past-this-chunk case, the
  uniform shorthand with its data array gone, no-ops,
  section creation at both ends in Y order, air that
  invents nothing, every refusal class (nulls, bad
  states, non-compound root, a box past the section
  byte both directions, a data array too short), and
  the file-level pair — check refuses with the bytes
  untouched, fill writes only what changes, the
  second run changes nothing.
- Verified at commit: `./rebuild.sh test` PASS — 5
  suites, 0 failures; `./rebuild.sh release` builds;
  ref tools 8 (java fill) + 8 (java blocks) + 12
  (compact) + 9 (write) + 17 (chunk) + 14 (inventory)
  + 25 (entity) + 19 (bedrock write) + 28 (bedrock
  column); align.py 0 misalignments; golden worlds
  untouched — every write in the suite ran against a
  scratch copy.

### Next

- Phase 4 reservation `convert` is what the roadmap
  names after this.

---

### Built

- `chunk get` and `chunk set` answer for Java worlds
  now, not only Bedrock — the same verbs, one spec
  dialect either way. The probe decides the world kind
  and the half that answers: Bedrock's half moved into
  its own helper unchanged, Java's goes through a new
  palette layer. A script edits one world or the other
  without knowing which.
- The new layer (`java_chunk.h/c`) answers for one
  block, the unit the verbs are: `mcme_java_block_spec`
  names the block at a position out of the chunk's
  section palettes — the 1.18+ layout the golden world
  carries, `sections` at the root, each with its
  absolute Y and a `block_states` compound. The packing
  is the format's own: 4 bits as the floor, the
  palette's need above it, floor(64 / bits) entries to
  a long, none spanning. A position no stored section
  covers, and a section stored without `block_states`,
  are `minecraft:air` — what the format says they hold.
- `mcme_java_set_block` edits the tree the caller
  parsed: the position's entry is reused when the
  palette already holds the block and appended when it
  is not — never replacing an entry in place, which
  would change what every other position holding the
  old entry means — and the section's long array is
  repacked at the bits the palette now needs, old
  indices keeping their numbers. A section the chunk
  does not list is created and kept in Y order, except
  for air, which an uncovered position already holds:
  that edit invents nothing. A no-op — the block
  already stored there — edits nothing at all, and the
  CLI skips the write, the same byte-honesty Bedrock's
  set keeps.
- The write back is the path `region chunk set` made:
  the whole chunk document re-encoded, compressed,
  appended to its region file, the slot repointed — no
  LOCK, since region files have none. What mcme will
  not do is invent a chunk document: a chunk the region
  file does not have carries bookkeeping the game would
  have to trust (`Status`, `Heightmaps`, the rest), so
  both verbs call it "not stored", exit 2 — the answer
  Bedrock gives for a missing subchunk.
- `--dimension` reaches Java's directories the way the
  game lays them out (`region/`, `DIM-1/region/`,
  `DIM1/region/`), which `java_dimension_dir` already
  knew; `chunk fill` stays Bedrock-only, documented.

### Cross-check

- `tools/ref_java_blocks.py`, 8 checks, all on copies
  of the golden worlds. The tool reads the region file
  with no mcme code — its own sector-table walk, its
  own palette unpacking — and compares block for block.
  AIR: the golden sections, stored without
  `block_states`, read as air at every position, JSON
  echoes coordinates. ABSENT: an absent chunk, a
  dimension without a region directory, junk
  coordinates, a broken block spec, a Y no section byte
  could name take their documented codes. SET: writes
  the reference walk finds at the right positions with
  the right palette — the first edit into an untouched
  section makes a palette of air plus the block, states
  carried as Properties the reference reads — and
  `get` prints the names the walk reads, neighbours
  still air. REUSE: a held block does not grow the
  palette. NO-OP: not one byte of the region file
  moves, air on air included. SECTIONS: one appended
  above the stored ones, one spliced in before them,
  both in Y order in the reference's walk. ROOM:
  silent writes, and Bedrock still answers its own way
  after the restructure.
- Selftest `test_java_chunk`: the golden chunk's air,
  an edit round-tripped through the region write and
  read back by the CLI's own path, the format's packing
  decoded where the test states it, palette append and
  reuse, the no-ops, Y-ordered section creation, and
  every refusal class — null arguments, states that are
  not a compound, a Y past the byte, a chunk that is
  not a compound, a palette without a data array, a
  data array too short, an index outside its palette.
- Verified at commit: `./rebuild.sh test` PASS — 5
  suites, 0 failures; ref tools 8 (java blocks) + 12
  (compact) + 9 (write) + 17 (chunk) + 14 (inventory)
  + 25 (entity) + 19 (bedrock write) + 28 (bedrock
  column); align.py 0 misalignments; golden worlds
  untouched — every write in the suite ran against a
  scratch copy.

### Next

- `chunk fill` for Java worlds: the box verb Bedrock
  has, through the palettes this entry just made —
  one section repack per section the box touches, not
  one edit per block. The Phase 4 reservation
  (`convert`) remains what the roadmap names after.

---

## 2026-10-12 — Compaction: `mcme region compact`

### Built

- `mcme_region_compact()` (region.h/c) rewrites a
  region file packed: live chunks only, in slot order,
  from sector 2 on — the counterpart to set's
  append-then-repoint, reclaiming the slack every set
  leaves behind (dead payload versions, holes, tails no
  entry claims). Each chunk is copied as its raw frame —
  length, compression byte, data — so nothing is
  decompressed: gzip, zlib, stored and external payloads
  ride through byte-identical, and timestamps are carried
  over. The counts written are the counts that are true,
  the sectors each payload actually occupies, not what
  the file was carrying.
- The new file is written beside the old as
  `r.<rx>.<rz>.mca.tmp` and renamed over it: the file
  changes in one step, a reader finds the old file or
  the new one, never a half-packed one, and any failure
  — a write that will not land, a rename that will not
  happen — removes the .tmp and leaves the original
  untouched. Compaction never leans on the old file's
  sector counts either: every payload's size comes from
  the length field the reader trusts.
- Two refusals are the file's own honesty. A payload
  header mcme cannot read refuses the whole file — a
  compactor that skipped what it could not parse would
  drop it. A payload bigger than the 255 sectors a
  slot's count byte can claim is refused too: mcme
  reads such a chunk (the length field says so), but a
  rewrite could not record it honestly, so the file is
  not one mcme will write. Silent on success, like
  `db compact` and every write verb: what happened is
  the inventory's next listing. `compact` is matched
  before the world path, like `chunk set`.
- The selftest taught itself the format's arithmetic
  the hard way: a length field counts compression byte
  plus data, so a 255-sector length is a 256-sector
  frame, and an entry's offset lives in bytes 0..2 with
  the count in byte 3 — a craft attempt that put the
  offset in the count byte made a slot vanish, and the
  file the test thought it was refusing was being
  compacted as empty. (The same off-by-one walked into
  the ref tool twice; both now write the entry as
  `[offset-be24, count]` in one poke.)

### Cross-check

- `tools/ref_region_compact.py`, 12 checks, all on
  copies of the golden world. PACK: slack made the
  honest way — two writes to one chunk — then the
  tool's own sector-table walk finds the same slots
  with byte-identical frames, compression and stamps,
  counts each payload's own, offsets contiguous from
  sector 2 in slot order, size exactly header plus
  those sectors, the superseded payload's bytes gone
  from the file, the stamp the surviving chunk carries
  is this session's write rather than the fixture's,
  and amulet_nbt loads every payload the new table
  points at. INVENTORY: the read verbs report the
  compacted file's own numbers. IDEMPOTENT: a second
  compaction changes no byte. ROOM: a missing region
  file is exit 2, a corrupt one exit 3, a frame that
  will not read exit 3, a chunk too big for a slot to
  claim exit 3 with the 255-sector message, Bedrock /
  dimensions / coordinates / a missing argument take
  their documented codes — silent on every path,
  `--json` included.
- Selftest `test_region_compact`: snapshots every slot
  before the rewrite, then pins the packed layout
  (offsets, honest counts, carried stamps, byte-equal
  frames), the last write winning, the golden chunks
  read back whole, a byte-identical second compaction,
  slot order over append order — a chunk written first
  to the higher slot moves down and its stamp moves
  with it — the empty file's 8192-byte compaction, and
  every refusal class.
- Verified at commit: `./rebuild.sh test` PASS — 5
  suites, 0 failures; ref tools 12 (compact) + 9
  (write) + 17 (chunk) + 14 (inventory); align.py 0
  misalignments; golden worlds untouched — every write
  in the suite ran against a scratch copy.

### Next

- Java blocks: `chunk get`/`chunk set` answering for
  Java worlds through the palette layer — Bedrock has
  both verbs, Java has the chunk document now but no
  per-block way in. After that, the Phase 4 reservation
  (`convert`) is what the roadmap still names.

---

## 2026-10-12 — Writing a Java chunk: `mcme region chunk set`

### Built

- `mcme_region_set_chunk()` (region.h/c) replaces one
  chunk append-only: the document is encoded big-endian
  and compressed as zlib, the payload lands after the
  file's current end (zero-padded to a sector boundary —
  for a file that does not exist, that same pad writes
  the 8 KiB header), and the slot's location entry
  repoints at it with the timestamp the caller gave.
  The entry is written last, so a reader racing the
  write still finds the old chunk whole. In-place reuse
  was never an option: it would lean on the sector
  counts, the one header field the reader deliberately
  ignores because real files carry them stale.
- A slot the world has not stored gets a chunk, and a
  region file that does not exist is created — but the
  region directory must already be there. mcme writes
  into a store the world has; it does not invent one,
  and it will not rewrite a file it cannot read. The
  refusal vocabulary is McmeWriteRefusal itself —
  region.h includes bedrock_write.h for it, with a
  comment saying the enum is the CLI's shared write
  vocabulary and merely lives where it was born — so
  every code comes from the one `refusal_exit` mapping:
  USAGE (null argument, non-compound root, a payload
  past the 255 sectors a slot's count byte can claim),
  NOT_THERE (no region directory), NOT_READABLE (file
  present but not a region mcme reads), STORE, INTERNAL.
- `mcme region chunk set <world> <cx> <cz> <snbt>`:
  the SNBT is the read verb's own output, so the loop
  is read → edit → write with no second format to learn.
  The document parses before any file is touched, the
  verb is silent on success like every write verb, and
  `set` is matched before the path the way `entity set`
  is — a world directory named `set` reads as `./set`
  for the other verbs. cli.md documents the verb before
  the code; the usage text gained the line.
- ASan earned its keep on the first draft: the
  encode-failure path leaked the encoder's buffer. The
  selftest's first attempt at the oversized refusal also
  taught the format's real edge — NBT strings cap at
  65535 bytes, so a 3 MB string is not a document at all;
  the test now builds a 500 000-int array through the tag
  API, which is over the ceiling the right way.

### Cross-check

- `tools/ref_region_write.py`, 9 checks against the
  reference, all on copies — the golden world is only
  ever read. WRITE: a chunk's SNBT comes from
  `mcme region chunk`, is edited through amulet's own
  tags (one field changed, one added) and sent back;
  amulet_nbt.load of the payload this tool's sector-table
  walk decompresses must equal that document deep-equal,
  and the slot's timestamp must be minutes old rather
  than the fixture's. FIDELITY: every byte outside the
  slot's entry, its timestamp and the appended tail is
  the original file, bit for bit. ROOM: the inventory
  reports exactly the payload the walker decompressed;
  an absent chunk is created and counted; a fresh
  region file comes out as zeros plus slot 4's entry at
  sector 2 with a fresh timestamp, and the inventory
  finds its chunk at the world coordinates given; no
  region directory is exit 2, a Bedrock or missing world
  exit 2, a corrupt region file exit 3, DIM-1 and DIM1
  are looked in while a dimension Java has no directory
  for is usage, bad coordinates / unparseable /
  non-compound documents / a missing argument are usage —
  with nothing on stdout at any point, success or fault.
- Selftest `test_region_write`: read-back pins (present,
  timestamp, zlib, DataVersion), the three-range byte
  diff after each of two writes to the same chunk, the
  fresh file, and all five refusal classes including the
  255-sector ceiling.
- Verified at commit: `./rebuild.sh test` PASS — 5
  suites, 0 failures; ref tools 9 (write) + 17 (chunk)
  + 14 (inventory) + 25 (entity) + write PASS (19) +
  28 (column); align.py 0 misalignments; golden stores
  untouched — every write in the test suite ran against
  a scratch copy.

### Next

- Compaction: every write appends, so slack accumulates
  (two writes to one chunk already show it) — a verb
  that rewrites a file without the dead payloads is the
  safe counterpart to this one's append-then-repoint
  discipline. After that, blocks: `chunk get`/`chunk
  set` answering for Java worlds through the palette
  layer this write now makes possible.

---

## 2026-10-12 — Reading a Java chunk: `mcme region chunk`

### Built

- `mcme_region_of()` (region.h/c): the sharding rule as a
  function — floor(chunk / 32) on each axis, the rule
  `r.<rx>.<rz>.mca` names are written by. C division
  truncates toward zero, which would file chunk -1 under
  region 0, so the negative side is adjusted; the mask in
  `slot_for` finds the same chunk at slot 31 of the file
  the rule names, and the two are pinned together in the
  selftest for 0, 31, 32, -1, -32 and -33.
- `mcme region chunk <world> <cx> <cz>` reads one Java
  chunk through the region container: coordinates to file
  to slot to inflated payload to NBT, printed by the same
  writers `nbt dump` uses — SNBT by default, the JSON view
  under `--json` — so the two commands answer `jq`
  identically and print byte-identical text for the same
  document. `(cx, cz)` are chunk coordinates, the unit
  `mcme chunks` counts in, not blocks.
- The chunk is the unit: a slot the world does not store
  is exit 2 naming the coordinates; a payload that will
  not inflate, a compression byte mcme does not know, or
  NBT that will not decode is exit 3 — any part of which
  mcme cannot read is not half an answer. JSON-mode
  failures print nothing at all.
- `--dimension` and the argument handling now share one
  helper with the inventory (`java_dimension_dir`,
  `parse_coord`, `extract_dimension` with the verb
  argument), so the two region verbs cannot drift apart on
  where a dimension lives or how a refusal reads.
- `chunk` is matched before the world path, like
  `entity set` and `entity del`: a world directory named
  `chunk` reads as `./chunk`. cli.md documents the verb
  before the code; the usage text gained the line.

### Cross-check

- `tools/ref_region_chunk.py`, 17 checks against the
  reference. READ: for every chunk in the golden world's
  manifest, `--json region chunk` must equal amulet_nbt's
  own `load` of the payload this tool's sector-table walk
  inflates — deep-equal plain Python, each side through
  its own decoder, with the first differing path printed
  on disagreement. PRINT: the SNBT must be byte-identical
  to `mcme nbt dump` on ref_region.py's golden .nbt AND
  parse back through amulet's `from_snbt` into that same
  tree — the printed line is not just stable, it is
  readable; the JSON must equal `nbt dump --json`'s.
  ROOM: an unstored chunk (exit 2, silent in JSON), a
  missing region file, a Bedrock world, DIM-1 and DIM1,
  a dimension Java has no directory for, bad coordinates,
  a missing argument, and a temp region whose payload
  inflates to garbage refusing whole as exit 3.
- Selftest: the sharding pins above beside the container
  checks.
- Verified at commit: `./rebuild.sh test` PASS — 5
  suites, 0 failures; ref tools 17 (chunk) + 14
  (inventory) + 25 (entity) + write PASS (19) + 28
  (column); align.py 0 misalignments.

### Next

- Writing back: a chunk replaced in its region file, the
  write half this read half proves first — the way the
  block entity record went, read before rewrite.

---

## 2026-10-12 — What the region file holds: `mcme region`

### Built

- `mcme_region_chunk_payload()` (region.h/c): the
  five-byte payload header — length and compression
  byte — read without decompressing a byte. It
  validates exactly what `mcme_region_read_chunk`
  validates before it starts reading (zero, the region
  limit, past the end of the file), so the two cannot
  disagree about what a slot claims; and it deliberately
  does not name-check the compression byte: an inventory
  answers what the file says, and `read_chunk` is what
  decides what mcme can open. Compression 7 or 127 is
  reported, not refused.
- `mcme region <world> <rx> <rz>` inventories one Java
  region file from the container's own tables: which
  chunks, each payload's compression and size, each
  timestamp — no chunk's NBT is opened, which is the
  whole point of the verb. Rows carry world chunk
  coordinates, so the listing joins against anything
  else that speaks in chunks without times-32
  arithmetic, and entries come back in the file's own
  slot order.
- `--dimension` picks the Java directory the way the
  game lays it out: overworld → `region/`, nether →
  `DIM-1/region/`, end → `DIM1/region/`. Java stores
  nothing else, so any other number is usage rather
  than a directory mcme would invent.
- One condition one exit code: 0 listed (an empty
  region is an answer, not a fault), 1 usage, 2 not a
  world / not a Java world / no region file at that
  name, 3 a file that is present but is not a region
  file mcme reads — the header will not validate, or a
  present slot's payload header will not read — 4
  store.
- `parse_coord` and `extract_dimension` now take the
  verb their messages belong to. Every existing call
  site passes `"chunk"`, so those messages are
  byte-identical; `region`'s say `region:` instead of
  claiming its arguments are a chunk problem.
- cli.md first: the JSON shape (`path`, `rx`, `rz`,
  `chunks`, `bytes`, `entries` with `x`, `z`,
  `compression`, `bytes`, `timestamp`), the text form,
  and the exit table, then the code that answers to it.

### Cross-check

- `tools/ref_region_inventory.py`, 14 checks against a
  reader written from the format's own description: for
  every region in the golden Java world the JSON and
  the text are each compared field for field, entry for
  entry in order, against sector tables Python parses
  itself. Plus an empty region listing on exit 0 in
  both forms, a length-0 payload header refused as exit
  3 (and silent under `--json`), compression 7 reported
  as `"7"` rather than refused, and the missing file /
  Bedrock world / DIM-1 / DIM1 / dimension 7 / bad
  coordinate / missing argument refusals each taking
  their documented code.
- Selftest `test_container` grew: every fixture chunk's
  payload header pinned (lengths 248, 252, 252, 251 and
  250 — all zlib, the file's own timestamp), empty
  slots, caller errors, and a hand-built region that
  declares length 0, a payload past the file's end and
  a claim over the region limit (all refused) and
  compression 7 (reported).
- Verified at commit: `./rebuild.sh test` PASS — 5
  suites, 0 failures; ref tools 14 (region inventory)
  + 25 (entity) + write PASS (19) + 28 (column);
  align.py 0 misalignments.

### Next

- A Java chunk read verb: `mcme_region_read_chunk` is
  proven against the `.nbt` goldens, but no verb yet
  puts a Java chunk's contents in an agent's hands.

---

## 2026-10-12 — Changing what's in the chest: `chunk entity set`/`del`

### Built

- `mcme_bedrock_put_block_entity()` and
  `mcme_bedrock_del_block_entity()` (bedrock_entity.c/h):
  the write half, answering with `McmeWriteRefusal` like
  every other write verb and opening `db_dir` with the
  exclusive LOCK the way `mcme_bedrock_set_block()` does,
  so one condition is one exit code across the CLI.
- The record is spliced, not re-encoded: only the changed
  document's bytes leave, only its new bytes arrive, and
  the documents beside it are written back as the exact
  bytes they arrived as. "The sign beside the chest is
  untouched" is a claim about bytes, so it is checked as
  bytes — both in the selftest (prefix + this document +
  suffix IS the store's new value) and against amulet,
  which computes each document's extent itself.
- The position argument wins: the document's own `x`, `y`
  and `z` are set from it, overwriting or injecting,
  because a document naming another block would be stored
  where nothing finds it. `id` must be a namespaced
  string — what `_decode_base_entity` requires to keep a
  document at all — so what mcme stores is what the game
  and the reference both read back, rather than what they
  silently drop.
- Writes create: a chunk with no record gets one, the way
  `chunk set` creates a subchunk. A record the game
  deleted counts as none too — adding an entity to a
  chunk that has none is the operation, not a
  resurrection of anything. That is the opposite of a
  deleted subchunk, which is still refused: ground the
  game removed carries meaning, an empty entity list
  does not.
- Removing the last document removes the record's key:
  mcme does not keep a record holding nothing, which is
  also what the game stores — no key at all. The store
  answers a tombstone and the read still says exit 2.
- Reads were tightened to agree with that: a read now
  requires the WHOLE record to decode, not just the part
  up to the match. Half a record a read would answer
  from is half a record a write would refuse to rewrite,
  and one rule beats two — cli.md now says "any part of
  which mcme cannot read is exit 3".
- Every false return says what was refused: a missing
  record answers "no block entity is stored at block
  (…)", not the store layer's "key not found".
- `mcme chunk entity set <world> <x> <y> <z> <snbt>` and
  `mcme chunk entity del <world> <x> <y> <z>`, documented
  in cli.md before the code. The document is SNBT — the
  same form the read prints, so read, edit, write back is
  one loop. Silent on success, like `chunk set` and
  `chunk fill`: the exit code is the answer. `set` and
  `del` are matched before the path, so a world directory
  named `set` reads as `./set`.

### Cross-check

- `tools/ref_bedrock_entity.py` grew from 8 to 25 checks
  and now runs both directions: mcme reads what the
  reference reads (whole document), and the reference
  reads what mcme wrote — as bytes. The record after an
  edit must be the record before it with only the edited
  document's extent exchanged; the replacement's length
  is derived from the two records, never assumed. Added:
  coordinate injection landing where the argument said,
  a set into a chunk with no record, the last document's
  record disappearing, a second deletion being exit 2,
  an un-namespaced `id` refused with the record
  untouched, and a junk record refusing both verbs
  without writing a byte.
- Selftest `test_bedrock_entity_write` pins the same
  from the library: the byte-for-byte splice, the
  neighbours reading back unchanged, the tombstone a
  last removal leaves, and the USAGE / NOT_THERE /
  NOT_READABLE refusals with their messages.
- Verified at commit: `./rebuild.sh test` PASS — 5
  suites, 0 failures; ref tools 25 (entity) + write
  PASS (19) + 28 (column); align.py 0 misalignments.

### Next

- The region inventory verb: what the `.mcr` files hold,
  without opening the stores inside them.

---

## 2026-10-12 — What's in the chest: `chunk entity`

### Built

- `mcme_bedrock_block_entity()` (bedrock_entity.c/h),
  the read half of a chunk's block entities: a chest's
  inventory, a sign's text, a spawner's rules. None of
  it is in a subchunk — each chunk holds one record
  (key tag 0x31) of one NBT document per entity,
  concatenated with no framing, which is the shape
  `mcme_nbt_parse_prefix()` already reads for block
  palettes.
- The record is a search, not a fetch: the documents
  are matched by the `x`, `y`, `z` each one carries,
  and the first match answers. A document with no
  coordinates is not addressable by position —
  amulet's own decoder drops it — so it is skipped
  rather than matched or refused. A record that will
  not decode fails the call naming the chunk: world
  data mcme cannot read, not an absence.
- `mcme chunk entity <world> <x> <y> <z>` prints the
  document verbatim — SNBT, or JSON under `--json`,
  which is a view of the NBT and not a round trip.
  Nothing is added to the entity's own fields, and the
  keys come back in the order the world wrote them.
  Exit 2 when no entity is stored at that block, which
  includes a record the game deleted.
- 0x31 joins the tag names (`block-entity`) and pointedly
  does NOT join the chunk markers: a record of entities
  says a chunk exists only because something else
  already stored its blocks, and counting it would
  report half-written chunks.
- The chunk/offset rule — floor division by 16, since
  a negative block is in the chunk below — moved from
  bedrock_write.c's private helpers into bedrock_keys
  as `mcme_bedrock_chunk_of()` / `mcme_bedrock_offset_in()`,
  because a third module now needs it and a rule written
  three times is a rule written wrong twice.

### Fixture

- `tools/ref_bedrock_entities.py` writes
  `tests/golden/worlds/bedrock-entities`: three records
  (a chest with two stacks, a sign, and a document with
  no coordinates; a furnace in a second chunk; a chest
  in the nether) over the golden subchunk payloads, put
  verbatim. Nothing is written until amulet's own
  `_unpack_nbt_list` reads back what was built, and the
  tool refuses an entity whose own coordinates fall in
  another chunk — the mistake that made the first
  version of this fixture unreachable.
- `tools/ref_bedrock_entity.py`, 8 checks: each document
  compared WHOLE against amulet's decode — dicts, lists,
  strings, numbers — so a reader that renames a field,
  drops one or reads a byte as something else fails.
  Plus the coordinateless document matching no position,
  the same local block one chunk over being nothing,
  a deleted record (exit 2) and a junk record (exit 3).
- Selftest `test_bedrock_entity` covers the same from
  the library: three documents in one record, the chunk
  addressing at a negative coordinate, the dimension
  key, usage faults, deletion and undecodable data.

### Next

- The write half: changing one entity means rewriting
  every document of the chunk's record, which wants the
  read proven first — it now is.

---

## 2026-10-12 — How high the ground is: `chunk height`

### Built

- `mcme_bedrock_column_height()` (bedrock_write.c/h)
  answers for the single column through a block: the
  subchunks the store holds for it, and the topmost
  and bottommost non-air block. The height comes from
  the payloads, never a heightmap record — mcme's own
  edits do not update one, so a height that trusted it
  would report the world before the last write. That
  is also why it is a read the editor can offer at
  all: nothing new to learn about the format, only the
  decoder already proven against the fixtures.
- The column is found by asking the store for every Y
  the subchunk key byte can name — point lookups, not a
  scan, so a column costs 256 reads against a world
  that may hold millions of keys. `has()` is the
  presence test and `get()` the content, because the
  two disagree on purpose: `has()` answers for a key
  the store still has, including one the game deleted,
  and only `get()` can tell a deletion from an absence.
  A subchunk the game deleted is not in the list; the
  surface drops to the one below it, which is what the
  reference agrees with. Both spellings of the prefix
  are asked, so a pre-1.18 world is read under its own.
- `mcme chunk height <world> <x> <z>`, taking block
  coordinates — the column through them, so two
  columns of one chunk are two answers. Text or
  `--json` (`subchunks`, `surface`, `floor`; null when
  the column holds no ground, which is an answer and
  not a fault). Exit 2 when no subchunk is stored for
  the column, which is the difference between ground
  the world never had and a column of air it did.
- Air means `minecraft:air`, `minecraft:cave_air` or
  `minecraft:void_air`; everything else is ground,
  the fluid blocks included, since a pool has a
  surface.
- A stored payload this build does not read fails the
  whole column, naming the subchunk: world data mcme
  cannot measure is not air. Same class `chunk stats`
  reports, same exit 3.

### Cross-check

- `tools/ref_bedrock_column.py`, 28 checks: the golden
  store's own column (six subchunks, negative Y, both
  wrappers, a version-1 payload), a second chunk, a
  second local column of the same chunk, the nether
  key, an unstored column (exit 2, silent), a deletion
  (list shrinks, surface drops to y 47), a planted
  junk payload (exit 3), and a `chunk set` into ground
  that was never saved becoming the next height
  read's surface where amulet finds the block.
- Selftest `test_bedrock_column` covers the same
  ground from the library side, the expectations
  derived from the fixtures' own index pattern.

### Next

- The region inventory verb that says which chunks a
  world actually stores, or block-entity reading.

---

## 2026-10-12 — Making ground: `chunk set` writes where nothing was stored

### Built

- `mcme_bedrock_subchunk_air()` (bedrock_write.c/h)
  builds the subchunk a write starts from when the
  world has not stored one: version 8 or 9, a single
  layer whose palette is only `minecraft:air`, and
  the Y index byte for version 9 — the version that
  carries one. It is the shape amulet writes for a
  new subchunk, which is what the cross-check reads
  back.
- `mcme_bedrock_subchunk_get_or_create()` decodes a
  stored subchunk, or hands back that all-air one for
  a key the store does not have. The version is taken
  from a neighbour — 9 when a neighbour carries the Y
  index, 8 otherwise — so a created subchunk sits
  quietly in a world that mixes versions. A deleted
  subchunk stays an error: the game removed it, and
  mcme does not resurrect it.
- `chunk set` and `chunk fill` both write into ground
  that was never saved now. mcme writes the subchunk
  payload and nothing else — not the version record,
  not the heightmap — so it remains an editor of a
  stored world rather than a generator. `chunk get`
  is untouched: reads report the store, so ground
  never saved is still exit 2, and `chunk stats`
  still counts it as air and says so.
- `McmeWriteRefusal` names the classes a write can
  be refused in, and both write verbs report them
  through an out-parameter, so one condition is one
  exit code across the CLI: the subchunk the game
  deleted is `2`, world data mcme cannot read — or
  the all-air marker, which has no layer to fill —
  is `3`, a store failure `4`. Before this, a
  corrupt subchunk was `4` on `set` and `2` on
  `fill`, and neither agreed with `get`, which
  already said `3`.
- The selftest asserts the version matching (cy 3
  beside a version-9 neighbour comes back version 9
  with its index; an isolated chunk comes back 8),
  the air that surrounds the new block, a fill that
  creates a whole subchunk without disturbing the
  one beside it, and the refusal classes. The
  cross-check plants a payload amulet can carry and
  mcme cannot read, and confirms both verbs exit 3
  and write nothing. Nineteen checks.

### Fixed

- The old "refuse to create" cases named coordinates
  that did not mean what their comments said: `(0,
  0, 48)` is subchunk (0, 3) cy 0, not (0, 0) cy 3.
  It never mattered while every absent subchunk was
  refused alike; creation made the difference
  observable, and the test was wrong, not the code.

### Next

Ground can now be made as well as edited. What still
separates a world mcme can edit from one it can write
whole is the rest of a save — the version record, the
heightmap. The declared next features are unchanged:
the region inventory verb that says which chunks a
world actually stores, and heightmap reading for
terrain shape.

---

## 2026-10-12 — What a box is made of: `mcme chunk stats`

### Built

- `mcme_bedrock_count_box()` (bedrock_write.c/h):
  the tally of a box — layer zero, the layer the
  editing verbs act on — one row per distinct block,
  sorted most frequent first, ties by spec so two
  runs over one world agree. A subchunk the box
  touches that is not stored, is deleted, or holds
  the all-air marker counts its positions as
  `minecraft:air`; the not-stored and deleted ones
  are also reported through `missing`. A payload
  that does not decode fails the call: that is
  world data mcme cannot read, not an absence.
- `mcme_bedrock_block_spec()` renders a palette
  entry as the spec text the CLI speaks — the name,
  states in braces when it has any. `chunk get`
  uses it now instead of its own copy, so get and
  stats cannot drift apart.
- `mcme chunk stats <world> <x1> <y1> <z1> <x2>
  <y2> <z2> [--dimension ...]`: `<count> <spec>`
  lines (nine-wide count column), most frequent
  first; stderr `N subchunks not stored, counted as
  air` unless `-q`. The one verb in the chunk family
  where a hole is exit 0: reading a hole is
  answerable — it is empty — writing one is not.
  `--json` carries `missing_subchunks` and the
  sorted `blocks` array, as wiki/cli.md documented
  before the code existed.
- The selftest counts the fixture's own indices by
  hand over a 4x16x4 box and compares row for row,
  then the hole (one missing, one air, volume 49),
  a planted undecodable payload, a Y past the key's
  byte, and the null arguments.
- The cross-check counts the filled box as exactly
  its eight tnt, tallies an eight-position box
  against one `chunk get` per position, parses
  `--json`, and confirms the deep-Y refusal.
  Nineteen checks, all green.
- Alignment debt repaid: the byte-column checker had
  holes — it skipped string-continuation lines and
  lost its paren stack inside multi-line comments —
  so earlier commits carried misaligned continuation
  lines it never saw. The checker now models both,
  and every line it flags across main.c,
  bedrock_write.c and selftest.c is reindented to
  the rule: content column = the call's paren
  column + 1.

### Next

An agent can ask what a box holds before it edits
it, and get an answer in its own units — counts and
specs. What is still missing is what the world
looks like: the region inventory verb that says
which chunks a world actually stores, or heightmap
reading for terrain shape.

---

## 2026-10-12 — A box at a time: `mcme chunk fill`

### Built

- `mcme_bedrock_fill()` / `mcme_bedrock_fill_check()`
  (bedrock_write.c/h): one payload write per
  subchunk the box touches, the positions outside
  the box inside those subchunks left alone, fluid
  layers never touched. The check names the first
  missing subchunk; fill runs it again under the
  caller's LOCK, so the store cannot change between
  the question and the edit, and a missing subchunk
  refuses the whole fill with nothing written. Box
  corners in any order. A subchunk that exists but
  cannot be edited fails the fill part-way — there
  is no transaction across puts — documented on the
  header.
- `mcme chunk fill <world> <x1> <y1> <z1> <x2> <y2>
  <z2> <block> [--dimension ...]` (src/app/main.c):
  the verb. A Y the key's signed byte cannot address
  is exit 1 before anything is opened, so every
  later failure is "not found". Success silent.
- The selftest grew the two cases that carry the
  contract: a box spanning two subchunks with
  reversed corners — every covered position holds
  the new block, every uncovered one matches the
  fixture, the new palette entry appended once, the
  waterlogged neighbour's fluid layer intact, the
  subchunk beyond the box byte-identical — and a
  box with a missing subchunk: refused, and the
  store still byte for byte the fixture.
- The reference cross-check fills through the CLI
  and reads the grid back through amulet: covered
  positions changed, the rest equal to what they
  were, no palette append, a subchunk the box does
  not reach byte-identical, exit 2 for a hole.
  Thirteen checks, all green.

### Next

mcme can now read and write any block of a stored
world, in any dimension, one at a time or a box at
a time. What is missing is what an agent plans
with: a box read (`chunk get` over a range), or the
inventory of what a region holds.

---

## 2026-10-12 — Any dimension: `--dimension`

### Built

- `--dimension <overworld|nether|end|number>` on
  `chunk set` and `chunk get`. Parsed per command
  from its own arguments the way `chunks --count`
  is; names exist because numbers are for machines.
  Without the flag the overworld — and the key omits
  a zero dimension the way the game does, so naming
  the overworld changes nothing on disk. An unknown
  name or a missing value is exit 1.
- The cross-check grew the case that matters: the
  same coordinates in dimension 1 edit the
  dimension's own key — amulet confirms the block
  changed there and the overworld subchunk is byte
  identical after — and `get --dimension 1` reads
  it back. Eleven checks, all green.

### Next

Box fill: one block spec across a box of positions,
spanning subchunks, one LOCK for the whole edit.

---

## 2026-10-12 — Read it back: `mcme chunk get`

### Built

- `mcme chunk get <world> <x> <y> <z>`
  (`src/app/main.c`): prints the block at the
  coordinate as the same spec `chunk set` accepts —
  the name, states in braces when the palette entry
  has any. The text is canonical — SNBT as the
  printer emits it — so it compares against another
  `get`, not against hand-typed text, and it can be
  passed straight back to `set`: the round-trip
  finds the same palette entry by byte comparison
  instead of appending a second one. The entry's
  version is not printed; the spec syntax has no
  version field. The all-air marker prints
  `minecraft:air`. Under `--json` the same text is a
  field, escaped, with the coordinates echoed.
  Reading does not take the LOCK — the game can hold
  the world open.
- Exit codes mirror `chunk set` (2 for a subchunk
  that is not stored), with 3 added: a payload that
  does not decode, or an index that points outside
  its palette, is world data wrong, not an i/o
  failure. One path caught by hand: an absent key
  made `mcme_leveldb_get`'s own "key not found"
  surface as exit 4; checking `has` first turns it
  into the promised exit 2.
- `tools/ref_bedrock_write.py` grew the read side:
  both edits read back through `chunk get`, the
  printed text is set again at a neighbour position
  with the palette entry reused (amulet confirms no
  append happened), and `--json` is parsed and
  compared field by field.

### Next

The earlier candidates stand, one answered: agents
can now verify their own edits. A box fill across
subchunks, or `--dimension`.

---

## 2026-10-12 — The first editing verb: `mcme chunk set`

### Built

- `mcme chunk set <world> <x> <y> <z> <block>`
  (`src/app/main.c`): sets one block, D3's first
  editing verb. The block is a namespaced name with
  optional SNBT states in braces —
  `minecraft:oak_log{pillar_axis:"x"}` — parsed with
  the existing SNBT parser. No registry: the name
  goes into the palette as given, the way the
  reference tools carry it. A subchunk that is not
  stored exits 2, not 4: the edit refuses to create
  chunks, so the CLI checks before the edit decides.
  Success prints nothing, like `db put`. Overworld
  only — the `--dimension` flag waits for the verb
  that needs it.
- `mcme_bedrock_subchunk_key()` (bedrock_write.c/h):
  the subchunk addressing — floor-div 16 for
  negatives, Y-range check — pulled out of
  `mcme_bedrock_set_block` so the CLI's pre-check and
  the write itself share one key computation and
  cannot drift apart.
- `tools/ref_bedrock_write.py`: the reference
  cross-check, opposite direction to the store
  tests. mcme writes through the verb; amulet reads
  the world back through `_load_palette_blocks` —
  the path that loads worlds Minecraft loads. It
  asserts the edited position holds the new block,
  every other position in the subchunk unchanged,
  the new palette entry byte for byte what amulet
  itself would write for the same block (so
  `pillar_axis: "y"` appends instead of reusing the
  stored `"x"` entry), the version-9 wrapper keeps
  its Y index, the neighbouring chunk is untouched,
  and a refused edit exits 2 having created nothing.

### Fixed

- Continuation-line alignment across the write-path
  files. The house rule is continuation under the
  opening argument; several declarations — including
  two from the previous commit — sat a few columns
  off. Alignment only, no behaviour changed.

### Next

A verb that exercises more than one block (a box
fill, or `--dimension`), or reading blocks back
(`chunk get`) so an agent can verify its own edits.

---

## 2026-10-11 — First write: set one block

### Built

- `mcme_bedrock_set_block()`
  (`src/world/bedrock_write.c`): builds the
  subchunk key, opens the store with the exclusive
  LOCK, decodes the stored payload, sets the
  block, re-encodes, writes the value back. The
  subchunk must already exist — a missing one is
  refused, not created, because creating chunks
  means the version record, the heightmap and the
  records a real save also touches.
- `mcme_bedrock_subchunk_set()`
  (`src/world/bedrock_chunk.c`): the domain
  mutation. Find-or-add on the layer-zero palette
  — the new entry is serialised and compared byte
  for byte against the stored entries — set the
  index, grow the stored width only when the edit
  forces it. The width only ever grows: a no-op
  edit leaves every byte alone, and a layer stored
  wider than its palette needs (real worlds
  contain such layers) stays that way, because
  narrowing would rewrite indices the edit did not
  touch.
- `mcme_bedrock_block_entry()`: builds a palette
  entry the way the reference writes one — name,
  states, version in that order, version 17694720
  ("1, 14, 0, 0"), the version amulet assigns a
  block that arrives without one.
- `tools/ref_bedrock_store.py` and the golden
  fixture `tests/golden/worlds/bedrock-chunks`:
  eight subchunk payloads under the keys their
  coordinates dictate, put through amulet-leveldb
  itself. The selftest edits copies and checks the
  store's answer against the original payloads:
  width growth 4 → 5, index reuse, a no-op coming
  back byte-identical, a waterlogged subchunk's
  fluid layer untouched, the compact form growing
  to width 1, version-9 wrappers and a non-zero
  dimension surviving, the neighbouring chunk
  unchanged byte for byte.

### Next

The CLI verb — `mcme chunk set` — and the
reference cross-check that reads mcme's write
back through amulet's own decoder.

---

## 2026-10-11 — Chunk keys: the builder, and a wiki correction

### Built

- `mcme_bedrock_key_build()` (`src/world/bedrock_keys.c`):
  the inverse of the key parser. Dimension written only when
  nonzero — the game omits a zero dimension, never stores four
  zero bytes. The signed Y index byte only on the two subchunk
  prefixes; any other tag with an index is refused, because no
  other record has a byte to spend. The selftest checks the
  built bytes against hand-written arrays, not through the
  parser, so a builder bug cannot hide inside a parse that
  shares it; the parser's own cases keep building their keys
  with the test-local builder, as before.

### Corrected

- world.md claimed chunk payload values are zlib, or raw LZ4
  block from 1.18.2 on. They are stored uncompressed. Every
  implementation this tree is validated against hands the value
  straight to the subchunk parser — amulet-core and
  mcbe-leveldb decompress nothing — and amulet-leveldb, built
  from Minecraft's own LevelDB fork, stores `put` values byte
  for byte. The fork's compression lives inside the database
  files: table blocks, bare DEFLATE type 4, which table.c
  already reads. compress.h and bedrock_chunk.h corrected to
  match. The write path is being built on this fact, so it had
  to be right first.

### Process lesson

- amulet-leveldb's `LevelDB()` constructor compacts on open:
  probing a golden store directly rewrote it (new MANIFEST, new
  table, retired log). Golden stores are opened read-only or
  copied to /tmp first — the fixture discipline the store tests
  follow, which the probe skipped.

### Next

The first editing verb: set one block — read a subchunk,
decode, find-or-add the palette entry, set the index, re-encode,
write the value back.

---

## 2026-10-11 — Subchunk codec: encode now inverts decode, byte for byte

### Built

- `mcme_bedrock_subchunk_encode()` (`src/world/bedrock_chunk.c`): the model
  back into payload bytes, appended to a buffer. Two phases — every refusal
  is decided before the first byte is appended, and the test pins that
  contract by checking the buffer is still empty on every refused encode.
- The property: every payload the decoder accepts encodes back byte for
  byte. Asserted against all thirteen golden fixtures by comparing the
  encoder's *output* with the reference payload itself, plus the inline
  accepted shapes (all-air marker, version 1 minimal, version 9 negative Y),
  plus a from-scratch model landing on hand-written bytes.

### Why it holds: the encoder writes what the model records

`bits` is the stored width and is never re-derived from the palette — a
two-entry palette stored at width 16 round-trips as width 16, not as the
width someone would pick today. Palette NBT is forced little-endian
regardless of what the document records. Padding bits are the single
documented exception: the model carries block indices, not the unused high
bits of each word, so they come back zero — which is what the reference
encoder and every payload this tree has seen store there anyway.

Refused before the first byte: versions outside {1, 8, 9}; a shape the
wrapper cannot carry (version 1 without exactly one layer, version 9 without
an index, an index on 1/8, more than 255 layers); widths outside the enum;
an empty palette or one with missing roots; a bits==0 layer that is not one
all-zero entry; an index its width cannot hold. Whether an index points
*inside* its palette is deliberately not this function's business — the
decoder does not enforce it either, and encode exists to invert decode.

### Next

Chunk writes: build the model, encode, compress (zlib or raw LZ4 from
1.18.2 on), key it, hand it to the store — plus the CLI verb (D3). The
store layer has been ready since compaction landed.

---

## 2026-10-11 — Chunk payloads: the subchunk codec lands, decode half

### Built

- `mcme_bedrock_subchunk_decode()` (`src/world/bedrock_chunk.c`): the bytes
  under a 0x2f/0x2c key parsed into layers of 4096 XZY block indices plus
  their little-endian NBT palettes. Versions 1, 8 and 9; every width the
  game writes (0,1,2,3,4,5,6,8,16); multi-layer (waterlogged) subchunks; the
  sign-extended Y index version 9 carries; the all-air marker as zero
  layers. All from an arena; strict about everything it does not fully
  understand, and it says what it found when it refuses.
- `mcme_nbt_parse_prefix()` in the NBT library: a block palette is several
  NBT documents concatenated with no framing, so the parser now reports
  where each one ended. `mcme_nbt_parse()` is a thin wrapper over it, with
  its behaviour unchanged.
- `tools/ref_bedrock_subchunk.py` + `tests/golden/subchunks`: thirteen
  fixtures encoded by amulet 0.10.64's own paletted-storage writer, each
  verified back through amulet's decoder before a byte is dumped. Every
  width, both storage-count wrappers, a waterlogged pair, the version-1
  wrapper with legacy `{name, val}` palettes, and negative Y.

### The bits==0 detail

At width 0 there is no palette length field: one NBT compound follows the
header and fills the layer. The wiki sentence can be read the other way, and
amulet's encoder can never settle it because it floors at one bit per index —
but amulet's decoder and mcbe-leveldb, both battle-tested on real worlds,
read it without a length. The `v8-bpb0` fixture pins our decoder to them.

### Caught by the fixtures, not by review

The first fixture generation built its index arrays transposed in z/y:
amulet's encoder takes [x][y][z] arrays and permutes internally, while the
disk is XZY. Widths 1 through 4 hid it — their small-modulus index patterns
are symmetric under the swap — and width 5 exposed it at once, which is the
case range doing its job. The C decoder had been right all along; the
fixture's raw indices were not.

### Next

The encode half: `mcme_bedrock_subchunk_encode()` reproducing the fixture
payloads byte for byte from the model, then chunks written through the store
layer — where compaction already waits to handle them.

---

## 2026-10-11 — Compaction: the log is now flushed, not just refused

### Built

- A table writer (`mcme_ldb_table_write` in `table.c`): uncompressed blocks,
  4 KiB target, restart every 16 entries with the array seeded at offset 0,
  full-last-key index entries rather than upstream's shortest separator, an
  empty metaindex, and the padded 48-byte footer. It writes files the
  directory's own reader accepts, which is the only standard this tree has.
- `mcme_leveldb_compact()`: rewrites the merged view as one new level-0
  table, appends a VersionEdit to the MANIFEST, and removes what it replaces.
  `mcme db compact <world>` is the CLI form (D3 again: scriptable, takes the
  LOCK).
- Auto-compaction in `mcme_leveldb_write()`: past 4 MiB of log
  (`MCME_LDB_LOG_COMPACT_AT`, upstream's write-buffer default) the next write
  compacts first, so a long editing session flushes itself and the 32 MiB
  ceiling of D10 becomes a backstop rather than a wall.
- The reader now applies `DELETED_FILE` records with their levels instead of
  walking past them, and tracks `next_file_number`. Both were latent bugs the
  compaction edit exposed: a store whose MANIFEST had ever deleted a file —
  including any world the game itself had compacted — would make the old
  reader demand files that were correctly gone. Writing the writer is what
  found the reader wrong.

### The shape (decisions.md#D11)

Full rewrite into one level-0 table; tombstones preserved; the property under
test is that compaction does not change the merged view, byte for byte,
sequences included. Crash-safety is ordering — table (fsync), then MANIFEST
edit (fsync), then deletions — so a crash at any point leaves a store a reader
accepts: an unreferenced table before the edit, unreferenced leftovers after.

### Verified against the reference, not against ourselves

- The Amulet binding's full dump of the `bedrock-tables` fixture (3775 keys,
  tables at more than one level, tombstones in both log and table) is
  byte-identical before and after `mcme db compact` — which also proves the
  binding accepts the MANIFEST edit and the new table, i.e. Mojang's recovery
  reads what mcme wrote.
- The binding then *writes* to the compacted store and mcme reads it back —
  proving the edit's `NEXT_FILE`/`LOG_NUMBER` do not collide with the next
  writer's minted numbers — and mcme compacts again on top of those writes,
  which the binding then reads.
- 300 CLI `db put`s of 16 KiB crossed the 4 MiB trigger mid-session; the log
  was replaced by a table and the binding read all 300 values back through
  the mixed table+log store.
- The C selftest pins view identity, tombstone survival, repeated
  compaction, post-compaction writes, and the one-table-no-log shape, each
  through a reopen — the disk is the state, the handle's memory is not.

### Next

Chunk writing. The store layer can now be grown, shrunk and flushed by the
code that will need all three.

---


### Built

- `mcme_leveldb_open_rw` + `mcme_leveldb_write` (batch), `_put`, `_delete`.
  One WriteBatch = one log record, so a torn tail drops the whole batch and
  never half of it — the same guarantee stock LevelDB gives.
- `mcme_ldb_log_append` in `log.c`: fragments records across 32 KiB blocks
  exactly the way upstream's `Writer::AddRecord` does, padding the trailer
  with the leftover bytes when fewer than seven remain. Its correctness is
  checked by writing with it and reading back with `mcme_ldb_log_read`, and
  by the reference library reading a store `mcme db put` produced — including
  a 33 KB value that crosses a block boundary.
- `mcme db get|put|del <world> <key-hex> [value-hex]` — the CLI surface, per
  D3: the write path is scriptable, and `get` is read-only so it works on a
  store the game has open while `put`/`del` take the lock.
- Sequence numbers come from `max(MANIFEST's last_sequence, highest in the
  data)`. Either alone is wrong: the MANIFEST can be ahead of the data (batch
  written, manifest updated, crash before flush) and the data ahead of the
  MANIFEST (crash between the two). Re-using a sequence would make an old
  copy of a key sort newer than the write that replaced it.
- Every writable handle takes an exclusive `flock` on `LOCK` — the lock
  Minecraft takes — and refuses a store the game has open. `flock` dies with
  the fd, so a crash leaves no stale-lock state.

### Why there is no log rotation

LevelDB recovers only logs numbered at or above the MANIFEST's log number,
because it assumes everything older was flushed into a table first. A writer
that starts a new log without that flush makes every record in the old log
invisible to every reader that trusts the MANIFEST — including mcme's own.
So: one log, appended for the life of the handle, and past 32 MiB the write
refuses with an error rather than growing a store only this session could
read. Rotation is not a missing feature at this scope; it is an unsafe one
until table writing and compaction exist.

### Bugs found

- `arrsetlen(buf, 0)` trips `-Wtype-limits` inside stb_ds's unsigned
  capacity comparison. A length-zero reset needs a variable, not a literal.
- The first write test pointed at the world folder rather than its `db/`
  subfolder; `mcme_fs_read` refused the directory, which is the tool working
  as designed and the test not.

### Known gaps

- **No table writing, no compaction.** The log is the only write surface.
- **The 32 MiB ceiling is a session limit.** A world edited past it needs a
  reopen and, eventually, a compactor.
- **`mcme db` is raw.** It takes hex keys; friendlier verbs come with the
  chunk-edit layer.

---

## 2026-10-11 — `mcme chunks`: Bedrock chunk inventory

### Built

- `src/world/bedrock_keys.c` + header: decodes a store key into
  (dimension, x, z, tag). Handles the format's one awkwardness — the dimension
  is omitted entirely when it is 0, so a 9-byte key is an overworld chunk and a
  13-byte one has a dimension, and only the tag says which. Chunk markers are
  0x2c (pre-1.18 subchunk), 0x2f (1.18+ subchunk) and 0x76 (chunk version).
  0x2d (biome/heightmap) parses but is never a marker: a chunk with a biome
  record and no subchunk is still one chunk.
- `mcme info` now prints `keys:` and `chunks:` for Bedrock, plus
  `chunks_by_dimension` in JSON. The old "database: present" placeholder is
  gone; an unreadable store says so rather than reporting zero chunks.
- `mcme chunks <world>` (was reserved): text, `--count` (one number, nothing
  else), and `--json`. Works for Java too, off the region manifest.
- `mcme_world_probe` now fills `db_path`, the `db/` directory itself, which is
  what `mcme_leveldb_open` wants. Callers were forming the world folder path
  and failing; the module resolves it once instead.

### The count is derived, and the derivation is the whole test

"How many chunks" is not a number a store keeps. A chunk is present if any
*marker* key marks it, so the inventory collects coordinates and counts them
distinct: the forge fixture's 23 live keys include several markers per chunk,
and counting keys reports 24 chunks in a nine-chunk world.

The forged fixture was redesigned for this (`tools/ref_leveldb_table.py`):
nine chunks across three dimensions, several keys each, two of them written
only under 0x2f. That last pair is deliberate — Amulet 0.10.64 does not know
0x2f and reports those chunks absent (7, not 9), and both counts are pinned in
the golden dump's header so the difference is stated rather than left for
someone to discover in a modern world that reads as empty.

`src/world/selftest.c` asserts the exact numbers against the fixture — 9
chunks, 5/2/2 by dimension, 2 modern-only — and asserts `db_path` resolves to
`db/`.

### Bugs found

- The regenerated fixture was dumped without `--batch-out` (default is empty),
  so the golden batch file kept the old design's tombstones while the store had
  new ones. Three leveldb selftest failures, all pointing at a fixture that no
  longer described itself. The default should probably be derived from `--out`.
- An assertion that `mcme_last_error() == NULL` — it never returns NULL, only
  an empty string. The "not a chunk is not an error" contract is now checked
  against `[0] == '\0'`.

### Known gaps

- **No writes.** Unchanged; log-append is next in the LevelDB layer.
- **`chunks` re-opens the store for `info`.** Two commands, two opens. Fine
  for now; a shared inventory call moves up when a third consumer appears.
- **Java chunk count comes from the region manifest**, not from walking chunk
  headers, so a region with unallocated chunks counts them anyway.

---

## 2026-10-11 — LevelDB reader: real stores, and what they hid

**Status: done.** `mcme_leveldb_open` reads a Bedrock store end to end —
log records, SSTables, the MANIFEST's live set, and a merge by internal key
across all of them. Three fixtures pass byte-for-byte against the Python
`leveldb` binding: 15 keys from a log-only world, 3775 from a compacted one,
44 from a forged one.

The header is `include/mcme/leveldb.h`; the implementation is
`src/world/leveldb/{format,log,table,db}.c`. It is read-only by design (D9):
no locking, no writes, no compaction.

### The four things the format did not say

Writing the reader turned up more about LevelDB than the format description
predicts. All four are in `decisions.md#D9`; the short version is that
Minecraft's fork is not upstream LevelDB, and the differences are the kind
that fail quietly:

- **Blocks are compressed with bare DEFLATE, called type 4.** Upstream knows
  0 (none) and 1 (snappy). Every real Bedrock world writes type 4. The reader
  refused it, which reads as "an optional feature is missing" when it actually
  means "no Bedrock world will ever open".
- **The five-byte block trailer is outside the handle's size.** Off by five,
  and the restart array lands on the wrong bytes — surfacing as a nonsense
  compression type rather than as an off-by-five.
- **The checksum covers the stored bytes**, so a deflated block is checksummed
  compressed. Checksumming the decompressed form gives blocks that decode
  perfectly and still fail verification.
- **Data blocks are ~100 KiB here, not 4 KiB.** So a fixture built by driving
  Minecraft's own LevelDB has one block and one index entry, and never tests
  the index walk taking a second step.

### Eleven bugs, each with a test

Six were found against real stores before the reader would open anything:

1. `mcme_ldb_mask_crc` rotated 17 instead of 15 — invisible on self-consistent
   test data, rejected every real store.
2. Log record checksums covered the payload only; the type byte is included
   too, and the check belongs per *fragment*, not per reassembled record.
3. MANIFEST tags 6/7 were swapped and compact-pointer (5) was unhandled, so
   any compacted store failed to parse.
4. `read_block` ignored the trailer entirely.
5. `db.c` formed `NULL + 0` for zero-length values.
6. `mcme_leveldb_open` left `*out` untouched when its arguments were NULL.

Five more came out of making the fixtures small enough to read:

7. Refusing compression type 4 (above).
8. **The block cursor stopped after one entry.** `block_iter_next` advanced to
   the start of the value instead of past it, so the second entry was parsed
   out of value bytes and the walk died. One entry per block, which looks like
   a small world rather than a broken one — and no fixture could have caught
   it, because every real fixture has exactly one block.
9. That failure was *silent*: `block_iter_next` returns false both when a
   block ends and when it is damaged. The iterator now carries a `failed` flag
   and all three callers check it.
10. `load_golden` asked `sscanf` for two tokens, so a line whose value is zero
    bytes was dropped — a reference that reads one entry short.
11. `memcmp(NULL, x, 0)` in the test and in `db.c`/`nbt.c`. Legal-looking,
    undefined, and reported by the sanitizer build.

### The forged fixture

`tools/ref_leveldb_table.py` writes a store byte by byte rather than asking
LevelDB for one: eight data blocks, a restart point every eight entries, and
blocks alternating between stored and bare-DEFLATE. It is the only fixture in
which the index walk takes a second step, and it is why bug 8 is fixed rather
than merely suspected.

The generator is validated by reading its own output back with the reference
library before any golden data is written — a forgery the real library refuses
is worthless, and it was: the first three attempts were refused, each for a
real mistake (a 46-byte footer where the format wants a fixed 48; checksums
over uncompressed bytes; and user keys where tables store internal keys).

### Known gaps

- **No writes.** D9 puts reads first; log-append comes next.
- **Snappy is not implemented**, by choice — Minecraft does not write it. It is
  refused by name and offset rather than guessed at.
- **Filters are never read.** They accelerate point lookups and this reader
  only scans; an unknown filter is refused as a store we were not built
  against.
- **`mcme info` does not yet count Bedrock chunks**, and `mcme chunks` does not
  exist. The library is ready; the CLI is not wired to it.
- **No fuzzer for the table or log readers.** The NBT and decompress harnesses
  exist; these parsers touch the same untrusted data and should get one.

---

## 2026-10-10 — Phase 3 (part 1): compression, Anvil regions, `mcme info`

**Status: done** for Java. Bedrock detection done; the LevelDB reader is not
started.

Compression, the Java Anvil region container, world detection, and the first
command that exercises all three at once. See [world.md](world.md) for the
format contract and [decisions.md#D8](decisions.md) for the zlib change.

### Built

- **`include/mcme/compress.h`, `src/compress/`** — gzip, zlib, LZ4 and stored,
  with a `max_out` ceiling on every decompression and magic-byte sniffing. This
  is the only place in the tree allowed to include `<zlib.h>` or `<lz4.h>`.
- **`include/mcme/region.h`, `src/world/region.c`** — Java Anvil `.mca` reader:
  sector table, timestamps, chunk extraction and decompression.
- **`include/mcme/world.h`, `src/world/world.c`** — world detection plus
  `level.dat` metadata for both editions.
- **`mcme info <world>`** — identifies a world and summarises it. Exits `2` for
  "not a world" and `4` for "not a readable directory", deliberately.
- **`mcme nbt dump`** now strips gzip instead of refusing it.
- **Golden data:** `tools/ref_worlds.py` (amulet-core 1.9.50),
  `tools/ref_region.py` (amulet_nbt 2.1.8), `tools/ref_compress.py` (Python
  `zlib` and `lz4.block`).
- **`build/fuzz_decompress`** — the third fuzz harness.

### The zlib reversal

Phase 1 vendored `miniz` for gzip and never compiled it, because it needs a
CMake-generated `miniz_export.h`. Phase 3 needed gzip for real, so the choice
was revisited and **miniz lost**: zlib is already a hard dependency of GTK4 via
libpng, so vendoring a second implementation would add a dependency rather than
remove one. Recorded as [D8](decisions.md). `third_party/miniz` stays on disk,
unused, behind `mcme_compress.h` for a possible static build later.

### Bugs found

**`mcme_fs_read` on a directory.** `fopen` succeeds on a directory, `fseek` to
the end succeeds, and `ftell` reports `LONG_MAX` — which reached `malloc` as
2^63 bytes and aborted under ASan. The `size < 0` guard did not catch it because
`LONG_MAX` is positive. Found by pointing the region reader at
`tests/golden/worlds/java/region` instead of an `.mca` file, which is exactly
what a user does when they mean "this world". Fixed with `fstat` +
`S_ISREG`, which also stops us blocking forever on a FIFO. Regression test in
`src/core/selftest.c`.

**SNBT compound key ordering.** The case tiebreak was applied at the first
differing *character*, so `structures` sorted below `Status`: both start with
`s`, and the tiebreak fired before the comparison ever reached the `r` versus
`a` that should decide it. The reference compares case-insensitively first and
only uses case to break a full fold-tie, so it prints `Status` first. Phase 2's
eleven golden documents happened to contain no such key pair, so the bug
survived a byte-exact test suite — it surfaced only when a real Anvil chunk,
which does contain `Status` and `structures`, went through it. Fixed, and now
pinned by hand-written cases in `src/nbt/selftest.c`, where it belongs.

That is the general lesson: **golden data only covers what it happens to
contain.** A fixture of empty chunks found a sorting bug that eleven carefully
constructed NBT documents did not.

### Notes

- Bedrock `level.dat` is an 8-byte header followed by NBT that may or may not
  then be gzipped. Minecraft gzips it; amulet's writer does not.
- The Bedrock `lastOpenedWithVersion` tuple is printed in full (`1.10.0.0.0`)
  rather than truncated to a depth that looks plausible.
- `fuzz_decompress` run for 121 s: **5,366,633 execs, exit 0, no artifacts**.
  An earlier run under `-rss_limit_mb=512` reported an OOM that did not
  reproduce from the saved artifact; it was libFuzzer's own corpus plus ASan
  shadow accounting, not our code. Run it with the limit off.

### Next

1. Bedrock LevelDB reader — vendored via its C API behind `mc_leveldb.h`, then
   `mcme info` gains a real chunk inventory and `mcme chunks` comes off the
   reserved list.
2. `wiki/porting/` — linked from [README.md](README.md) and [cli.md](cli.md)
   but not written yet, so those two links are currently broken.
3. `.lang` list is hardcoded in `main.c` (18 ids) instead of reading the
   directory.

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
