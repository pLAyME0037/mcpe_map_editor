# Dev log

Newest entry at the top. Append; do not rewrite history.

Status vocabulary:
**scaffold** = files exist, not functional · **wip** = being written ·
**works** = runs, may lack tests · **done** = runs and covered by tests ·
**blocked** = needs something first

## 2026-10-17 — Phase 5 opens: the pack announces itself from one manifest

### Built

- The resource subsystem (D29): `mcme_pack_open` reads a Java
  resource pack's `pack.mcmeta` into an int32 `format`, a heap
  `description`, a `has_icon` flag and the `root` it came from,
  freed by one `mcme_pack_free`. Three refusals — no readable
  manifest, a manifest that is not the game's `{pack:{...}}` shape,
  no `pack.pack_format` — and a missing description answers the
  empty string, the reference's own default. The verb is
  `mcme pack info <dir>`, human and `--json`, with usage refusals
  on exit 1 and pack refusals on exit 2.
- The sixth selftest suite (`resource`, 6 suites now) writes its
  fixtures under `build/pack_fixtures` and pins the happy path, the
  description default, all three refusals, the null arguments and
  the error-clear the other suites use.
- `tools/ref_pack.py` proves the CLI against
  minecraft_model_reader's `JavaResourcePack` itself: three valid
  packs (a crafted one, a description-less one, and the reader's own
  shipped `java_vanilla_fix`) compared field by field, the four
  not-a-packs the reader marks invalid refused on exit 2 with empty
  stdout, and both usage refusals — 9 checks.

### Notes

- The manifest reader is deliberately stricter than the reference
  where a manifest leaves the domain the game writes: mcme requires
  the integer `pack_format` (the reference keeps any JSON type) and
  a string description (the reference `str()`-coerces anything).
  Recorded in decisions.md#D29 rather than matched: mcme's field is
  int32, and a value it cannot trust is refused, not reported.
- During this drop the leveldb suite failed three `log_count`
  checks: an untracked `000006.log` left in a golden fixture db by
  an earlier selftest run in the session was counted by the
  counter. Removing the untracked artifacts (tracked fixtures
  untouched, `git checkout` restored) made all 6 suites green under
  the default ASan+UBSan+leak run. The leveldb suite's sensitivity
  to leftover compaction files in `tests/golden` is pre-existing and
  recorded here; nothing under `tests/` was staged.

### Next

- Phase 5's core: blockstate resolution — the reference manager's
  `variants` matching (per-property, first-match in file order) and
  `multipart` case lists, then the model parent-merge — measured
  through `minecraft_model_reader`, the same way D29 pinned the
  manifest. Atlas and mesher stay after that (Simplexity 9).
## 2026-10-16 — The name map grows to the whole spec: the 63 blocks no census reached

### Built

- The name table answers every block the Bedrock 1.20.80 spec
  registers, not only the ones a real world's census was known to
  hold (D17). A generator measured all 1,087 blocks against the
  reference's own answers over the cells the state map walks, and
  wrote 264 new rows as 244 new groups in the byte order the lookup
  walks — the 309 rows D17 wrote stand unchanged. Where the answer
  moves everywhere alike the row is a wholesale rename; where it
  varies, value rows whose condition the reference answers
  uniformly, first-match, the empty cell's answer as the default
  last. The wood hanging signs are the conditional family —
  attached_bit or hanging at 1 keeps the floor variant's name, both
  at 0 is the wall variant — the shape bamboo already had.
- The state map inherits the newly-named blocks: 2,207 → 2,496
  single rows, 324 → 366 rescues, and the probe's name-skips go
  from 6,077 cells in 63 blocks to zero. All 12,252 probe cells now
  answer, none skipped and none failed. lit_blast_furnace carries
  facing/lit, light_block carries level, tuff_stairs carries
  oak_stairs' facing, the piston head carries facing/short/type.

### Notes

- 8 cells fabricated on a /tmp copy of the bedrock-chunks fixture
  (level.dat from the bedrock fixture), converted through
  `mcme convert --to java/1.20`, read back through `chunk get`, and
  checked against the reference: 8/8 pass props_match. copper_door
  answers oak_door, border_block and copper_bulb answer as air,
  tuff_stairs answers oak_stairs facing south, light_block answers
  light with its level, the hanging sign answers its wall variant
  at both bits 0.
- Gates green: 5 unit suites under ASan+UBSan, the selftest, and
  the 11-script battery (rc=0); ref_convert's own 15 checks include
  the name pin (573 rows re-asked at their conditions) and the
  state pin.

### Next

- The translate layer's generated tables now cover names, states,
  biomes, the flat map and the legacy map. Phase 4's convert
  surface is complete for the bedrock → java/1.20 pair.
---

## 2026-10-16 — The state map: 3,174 generated rows answer what the keeps guessed

### Built

- translate.c's kKeep, kAxisSuffixes/kMushroom and the
  redstone-wire branch retire into the GENERATED STATE MAP and
  GENERATED STATE RESCUE tables (2,752 single rows + 422 rescue
  rows, emitted by the v3 generator): one row per (block, state,
  value) whose Java answer differs from the default an absent
  property stands for, carrying the reference's own raw value —
  `power` carries `"1"` at signal 1, never the canon `"true"`.
  Rows compose in table order, later puts overwriting earlier
  ones on a shared key; a two-condition rescue row answers the
  cells whose composition alone cannot — first match overwrites
  and stops. The walk binary-searches the block group and scans
  it linearly, conditions tag-type strict (mcme's tag enum is
  amulet's own numbering).
- translate.h's rules 3 and 4 rewritten: the state half is the
  table (raw values, default-fill and false-escape), and a state
  no row answers drops to the default an absent property stands
  for — the reference's own empty-cell answer.
- selftest.c's translate assertions move to the delta model, with
  new cases pinning what the fixture never held: grass_block
  snowy true and furnace lit 1b now name-only answers, the wire
  at redstone_signal 7 writes `power "7"`.
- ref_convert grows `pin_state_map` (check 15 of the battery):
  every row re-asked at its own cell with the spec's own tag
  spellings, the raw spelling compared raw, the delta rules
  re-derived, the walk's composition re-run per row and per
  rescue cell, and the block-group contiguity and byte order the
  binary search needs checked.

### Measured

- The keeps diverged exactly where the fixture never looked:
  snowy and lit answer constant `'false'` whatever tag type and
  value they carry, so kKeep's pass-through wrote `snowy:"true"`
  against the reference's `'false'`; the wire branch hardcoded
  `power "0"` at every signal while the reference answers the
  signal itself, and hardcoded the four sides the reference's own
  empty answer already stands for. The delta model retires all of
  it: 2,207 (block, state, value) cells carry values beyond the
  default, and everything else — equal-to-default and
  canon-`'false'` — collapses to absence, which props_match
  answers the same.
- The bedrock 1.20.80 furnace's state is the namespaced
  `minecraft:cardinal_direction` (string), not an int
  `facing_direction`: east/north/west carry `facing`, the default
  south is absence. mushroom_stem is not registered by the spec
  at all — its pillar_axis was always an unknown state — while
  the stem's Java properties arrive through
  brown_mushroom_block's huge_mushroom_bits, bits 10 → the four
  sides true.
- The 63 name-skip blocks (6,077 probe cells) are D17's name-map
  gaps, counted not fixed: the wood hanging/standing signs where
  the map only has bamboo, the →air families, copper
  doors/trapdoors → oak, tuff, double_slab, the legacy spellings
  (tripWire, light_block, pistonArmCollision,
  lit_blast_furnace, end_brick_stairs, darkoak_wall_sign). The
  fixture never holds any of them.

### Cross-check

- The generator's probe: 1,087 blocks through base cells, every
  single state value, every base-minus-one-state cell and every
  pair — 12,252 cells, 0 fails under props_match, the pin's own
  acceptance function.
- `./rebuild.sh test`: PASS — 5 suites, 0 failures (the
  translate and convert-block assertions rewritten to the delta
  model, ASan+UBSan clean).
- Battery 11/11 rc=0, ref_convert reporting 15 checks — the new
  pin re-asks all 3,174 rows against the reference, raw
  spellings included.
- Eight cells fabricated on a /tmp copy of the fixture store
  (the golden store itself never opened) — wire signal 7,
  furnace cardinal east, snow height 4, water depth 3, mushroom
  bits 10, both door halves, grass snowy true — convert to
  exactly the reference's answers through the full CLI pipeline,
  cross-checked through props_match against ref.answer. No
  census, same as D26: fixture-verified by the user's own call,
  and a restored world can re-run the real-store check.

## 2026-10-16 — The older biome record: the 768-byte table crosses

### Built

- convert collects the 0x2d keys as biome records since D26 —
  both spellings ride the same span, sorted with the Data3D
  record first (the reference's loader takes the 3D arrays when
  they are there, the older table only when they are not) — and
  `chunk_data` loses the 0x2d: it counts versions, markers and
  unknown chunk-data tags, nothing the biome counters also count.
- `mcme_bedrock_biome_2d_decode` (bedrock_chunk.c) reads the
  768-byte spelling the reference's own loader reads it: the
  same 512-byte int16 heightmap half, then 256 raw ids
  materialized as the broadcast `convert_to_3d` materializes —
  16 arrays at absolute cy 0..15 (Biomes3D's defaults, not any
  floor), cell (x, y, z) = table byte 64z + 4x in Java's
  container order. Anything but exactly 768 bytes is refused
  whole and counted once. From there D19's and D20's verdicts
  ride unchanged: cells through the biome map (plains fallback
  for unknown ids, counted), arrays with no section counted,
  the map crossing when every column spells inside the
  dimension's height.
- ref_convert grows `put_2d_record` and three payloads: the end
  chunk's real record (columns spelling their own coordinates,
  sampled table positions holding id x + 4z with 194 — unknown
  to both tables — at column (1, 2)), the all-zero second
  spelling beside the overworld's answering Data3D record, and
  the 767-byte orphan that is not a record. `biome_mirror`
  reads the table the loader reads it: the 3D record wins, the
  table answers only where it stands alone, its broadcast
  sections compared cell by cell.

### Measured

- The broadcast is absolute: `Biomes3D`'s default section counts
  `(0, 16)` cap the 2D→3D fill at sections 0..15 — y blocks
  0..255, the classic world's own height — verified against
  amulet's classes directly, not derived from the wiki. Cell
  (x, y, z) = byte 64z + 4x, the same value down a column: the
  transpose and the `[::4, ::4]` sample compose to that, and a
  probe with byte i = i confirms it on the real code path.
- The biome id table is version-stable where the older spelling
  lives: bedrock (1, 2, 0) holds 69 ids, (1, 20, 80) 86, and
  every shared id spells the same name — ids were only added.
  One map answers both eras' records; no era split, unlike
  D25's flat blocks.
- The reference's own fallback for an id its table does not
  hold is `minecraft:plains` with a warning — mcme's fallback
  since D19, so the fabricated column 194 compares clean
  through the mirror.

### Cross-check

- 5 suites PASS under `./rebuild.sh test` (the new
  `test_biome_record_2d` pins the broadcast mapping, the signed
  heightmap half and both refusal sizes); battery 11/11 rc=0,
  ref_convert reporting 14 checks — the report counts land
  exactly where the design predicted (`biomes_carried` 443,
  `skipped.biomes` 2375, `skipped.heightmaps` 7, the end's map
  and cells carried), the biome parity counting 448 cells where
  it counted 384, and the heightmap check pinning the third map
  under MOTION_BLOCKING.
- The census snapshot is gone — stale /tmp cleanup took
  /tmp/opencode/census_world and no source Bedrock world is
  left on disk — so D26 is fixture-verified by the user's own
  call; the drop's claims rest on the fabricated records and
  the reference's own decode, and a restored world can re-run
  the real-store check this entry could not.

## 2026-10-16 — The pre-palette carry: two planes, two eras, 128 delta rows

### Built

- bedrock_chunk.c grows the GENERATED NUMERICAL MAP (256 id-to-name
  rows from the reference's own ints_to_block, six ids spelling
  `minecraft:numerical`) and GENERATED FLAT ERAS (payload byte to
  packed era stamp), and `mcme_bedrock_subchunk_decode` gains the
  flat branch: versions 0, 2-7 are 4096 block ids then 2048 data
  nibbles — low nibble first, the bytes amulet's flat branch reads
  — composed into one synthesized layer: first-seen (id, data)
  pairs as palette entries `{name, val, version}` stamped with the
  payload's era, `bits` the width that holds the slots, no index
  byte, one layer. Cut short or past the planes it is refused by
  plane name; the encoder's gate is untouched — flat decodes,
  never encodes, before a byte is appended (D12).
- translate.c grows the GENERATED FLAT MAP: 128 delta rows sorted
  (era, name byte order, val), with `flat_lookup` beside
  `legacy_lookup`. The legacy branch in `mcme_translate_block`
  gains one step: a numeric `version` on a `{name, val}` entry
  anchors at its era first, a miss falls to D24's chain exactly as
  the reference falls through; the shape gates are unchanged.
- The header doc in bedrock_chunk.h is rewritten around the two
  formats: the palettized inversion contract, and the pre-palette
  note — read for reading, refused on the way out, with the six
  unmapped ids sharing one name as the reason it can never be
  otherwise.
- tools/ref_convert.py: `decode_payload` gains the flat branch
  through amulet's own loader (bounds and tail the way mcme rules
  them); `load_input` reads either subchunk prefix — 0x2f and the
  pre-1.18 0x2c — normalizes amulet's flat palette into
  loader-shaped `{name, val, version}` entries and carries the
  payload's era beside the layers; the block loop routes flat
  cells through `Reference.answer_flat`; the copy fabricates two
  flat payloads and two refusals under 0x2c; `pin_flat_map` re-asks
  8,032 cells, the 256 ids and the eight era stamps, and asserts
  every row is a real delta in search order.
- Tests: `test_subchunk_flat` pins a full 6,145-byte payload —
  four synthesized pairs in first-seen cell order, era 1.1 stamps
  on v5 and era 1.2 on v7, indices and bits, the encode refusal
  appending nothing, and the three refusals (id plane, nibble
  plane, tail); the translate pins add sandstone val 3 at era 1.1
  against the cap's smooth_sandstone, the stampless and
  other-version boundary answers, jukebox at both eras, and the
  numerical placeholder. The old `{0x05,0,0}` refusal pin keeps
  failing, now for the truncated-flat reason, and its comment
  says so.

### Measured

- The flat branch: `data[0] in {0,2..7}` -> 4096 ids + 2048
  nibbles, `combined = id << 4 | data`, cells read plane byte
  `(x << 8) | (z << 4) | y` — mcme's own XZY index, identity, so
  both decoders compose the same pair from the same bytes. The
  era is `min(chunk_version_to_max[V][1], world)` capped at
  `(1,12,999)` — V 0-5 -> bedrock_1_1_0, V 6-7 -> bedrock_1_2_0 —
  V being the payload byte, which for flat-era worlds is also the
  first byte of the chunk-version record under the game's 0x2c
  key. Packed stamps 16842752 / 16908288; the pin maps stamps to
  tables explicitly, never through `get_version(int)` — int means
  DataVersion on every other path.
- The enumeration: 2 eras x 251 distinct id-map names x data 0-15
  = 8,032 cells -> **128 deltas** (91 era A, 37 era B); the flat
  chain at the cap equals D24's legacy rule on all 4,000 mapped
  pairs; the six unmapped ids (210, 211, 212, 217, 230, 242)
  answer `minecraft:numerical` and pass through. The generator's
  first cut emitted 336 rows; 208 were canonicalization artifacts
  — the chain replica compared raw `"0"` against canonical
  `"false"` — fixed in the replica, and the 128 real deltas
  re-spliced.
- 385,287 real-world subchunk keys, all v9: the flat tables
  answer nothing on the census store by construction.

### Cross-check

- 5 suites PASS under all three gates (`./rebuild.sh test`,
  `./nob test`, `./rebuild.sh release test`); battery 11/11 rc=0
  with ref_convert reporting "the flat map's 128 era rows, all
  256 numerical ids, both era stamps per payload version and every
  unrowed cell answer the way the reference answers" (14 checks);
  goldens untouched (`git status tests/` empty).
- Real world (the census snapshot, which holds no flat payload):
  two release runs, rc=0, each byte-identical to D23's run — every
  file's bytes equal and the JSON reports byte-identical — so the
  new decode path costs nothing where there is nothing to decode,
  and no cross-entry delta is claimed.

## 2026-10-16 — The `{name, val}` carry: 2,330 rows, five defaults, a section written where air stood

### Built

- translate.c grows the GENERATED LEGACY MAP: 2,330 explicit rows
  plus five default rows, generated one-off by re-enumerating the
  reference (D17's precedent — the pin re-asks every row, so the
  generator needs no repo home), with `legacy_lookup` beside it:
  an (name, val) composite-key binary search over the rows, then
  the defaults by name, then name-alone. The file's top comment,
  translate.h's rule 1 and its docstrings are rewritten around the
  loader's own branch.
- `mcme_translate_block` trades its version gate for that branch:
  `states` present or neither field is modern and the entry's
  `version` is never read; no `states` plus a `val` is legacy — a
  numeric val answers row, default, or name-alone in turn (a val
  no int32 reaches never truncates: defaults only), and a
  non-numeric val is refused and counted once (D12). The tick
  path's blockStates answer through the same function, so a
  legacy tick state now carries instead of dropping.
- convert.c and convert.h comments follow the new semantics: the
  dropped-subchunk path counts what the map still refuses, and the
  `legacy_subchunks` field documents both of its causes.
- ref_convert mirrors the branch everywhere: `entry_of` returns
  (name, states, val) the loader's way; the blocks walk no longer
  skips legacy sections — the fixture's cy2 writes and compares
  like any other, `out_ys` asserting every stored subchunk has a
  section; `tick_mirror` branches the same; `Reference.answer`
  grows the cap rule (a `_cap` version and its own cache) and
  drops its legacy-means-air assumption; and the new
  `pin_legacy_map` re-pins all 2,330 rows against the reference,
  each default at every unclaimed byte val plus the extremes, and
  the name-alone rule for each plain name — every battery run.
- Tests: the translate unit flips `{name:"minecraft:log",val:4}`
  from refusal to `minecraft:oak_log{axis: "x"}` and adds the
  name-alone fallback (stone val 99), the formula default
  (noteblock val 200), a val past int32, an entry with neither
  field, and a non-numeric val refusal; the convert pins move
  `blocks` 36,864 → 40,960 and `legacy_subchunks` 1 → 0, and the
  fixture's four v1 positions pin the log row beside the three
  name-alone answers where air stood.

### Measured

- The loader's own rule: `if "states" in block or "val" not in
  block` → modern (states or {}); else `{block_data: IntTag(val)}`
  through `min(world, (1,12,999))` → bedrock (1,12,0) → java
  (1,20,0). `version` gates nothing on either side, and the
  signedness agrees both ways (`ByteTag(200)` is −56; mcme's tag
  is `int8_t`).
- The reference is deterministic — the grass puzzle resolved: val
  0 is grass's only answered value (`grass_block{snowy:false}`),
  every other val passes through, and what looked like two
  processes disagreeing was one run's val 0 versus the other's
  val 1 (an internal translate error returns the input block).
- The enumeration: 328 names × the byte domain −128..255 → 2,330
  rows across 279 names differ from name-alone; the five formula
  names (kelp's age, sapling's type/stage, tnt's unstable,
  noteblock's constant, lava_cauldron's) hold the five defaults,
  proven over the int32 range, and the other 323 names answer
  name-alone outside the byte domain — the domain is complete. All
  row properties are strings, sorted, at most seven.
- The fixture's v1 payload decodes to four entries [stone/0,
  dirt/0, log/4, grass/1] over 4096 non-air cells, so its section
  joins the other nine: 10 subchunks written, none dropped.

### Cross-check

- 5 suites PASS under all three gates (`./rebuild.sh test`,
  `./nob test`, `./rebuild.sh release test`); battery 11/11 rc=0
  with ref_convert reporting "the legacy map's 2330 rows and their
  five defaults all answer the way the reference answers" (13
  checks); goldens untouched (`git status tests/` empty).
- Real world (the census snapshot, whose 0 legacy subchunks make
  D24 inert there by construction): two release runs, rc=0, each
  byte-identical to D23's run — every file's bytes equal and the
  JSON reports byte-identical — so the branch changes nothing
  where the store holds no legacy entry, and no cross-entry delta
  is claimed.

## 2026-10-16 — Entities counted whole: 777 documents, 740,591 actor records, an index that counts nowhere

### Built

- The two entity keys, named as D13 deferred them: `bedrock_keys`
  spells `actorprefix` (+ 8-byte id, 19 bytes) and `digp` (+ a
  chunk key's bytes, 12 overworld / 16 with a dimension) with
  predicates and their inverse builders, shapes measured on the
  real store; the 0x32 tag joins the header's tag set.
- `bedrock_entity` gains `mcme_bedrock_chunk_entities`: the
  block-entity reader factored to take the record's tag and an
  error label, whole-record rule unchanged — every document
  decodes or the record counts once, an empty record counts zero.
- Convert collects 0x32 records like block-entity records and
  reads them after the chunk loop, together: nothing crosses for
  entities, so nothing pairs and there is no carry branch to
  respect. `skipped.entities` counts documents, the new
  `skipped.actors` counts `actorprefix` records, and `digp` keys
  are routed out of `other` to count nowhere — their documents
  are the actor records, counted where they stand.
- CLI surfaces: the drops table prints "entity document(s)" and
  a new "actor record(s)" row; the JSON `skipped` gains
  `actors`, alphabetically and unconditionally.
- wiki/cli.md's convert examples and report prose — stale since
  before D17, still saying `water_layers` — regenerated from a
  real run of the release binary (2 chunks, the actor and
  entity-document rows visible), staged as this session's hunks
  only beside the foreign reflow.
- Tests: the selftest fabricates a two-document list, an empty
  list and an actor+index pair beside the unreadable record, and
  pins `entities` 3, `actors` 1, `other` held at 1 (the pair may
  not leak) plus `actors` 0 in the carry test; ref_convert's
  mirror fabricates the same set, `WANT_REPORT` `entities` 1 → 3
  with `actors` 1 added and every other field equal.

### Measured

- The reference question D17 raised, answered: PyMCTranslate
  ships **no entity data anywhere** — installed 1.2.51 carries
  `block.json.gz` + `meta.json.gz` only, and upstream trees for
  `bedrock_1_20_80`, `bedrock_1_16_20`, `java_1_20_0` and
  `java_1_16_0` contain `block` directories and no `entity`
  directory; `EntityTranslator` is a deepcopy pass-through,
  `amulet.entity_support` is `False`, and the Anvil writer pops
  bedrock's `EntitiesDataVersion`, writing only java-native
  entity sets. The reference output carries no entity documents;
  the fixtures' `entities/*.mca` are empty `{DataVersion}` layer
  stubs. mcme carries none either — a pass-through document is an
  unloadable Java entity, D12's half-write (D23).
- Census of the current store, two independent walks agreeing key
  for key: 15,540 lists — 15,207 empty payloads, 333 with
  content, 0 unreadable — decoding to **777 documents**;
  **740,591** actor records; **38,752** indexes naming 6,381 ids
  of which 6,185 exist (196 dangling) with 734,406 actor records
  no index names — D13's pruned set; no tombstones, and no list
  key spells a zero dimension, so the coordinate-rebuild the
  block entities read with has no second spelling to miss.
- The store moved between entries: D17–D22 ran on a copy taken
  before the game's Oct 15 14:58 save, and the reboot's recopy
  brought current bytes (source and copy verified identical,
  1,451,166 keys both; nether/end subchunk counts still match
  D21 exactly, the overworld lost 6,745). D23's run therefore
  records fresh values — chunks 45,552, subchunks 385,287, blocks
  1,578,135,552, carried 11,255 / 106,170 / 24,555,014 / 45,551,
  items 980 — and claims no cross-entry deltas.
- Prediction before the run (the shipped key rules and the
  shipped reader, linked against the built objects): `entities`
  **777**, `actors` **740,591**, `other` **515**, `chunk_data`
  **215,490** — both release runs printed exactly these, rc=0
  twice, 281 output files each with every sha256 equal and the
  JSON reports byte-identical. The old-routing equivalents on
  the same content, for anyone diffing entries: entities 15,540
  keys, other 779,858.

### Method notes

- A census one-liner opened the golden bedrock fixture with
  amulet's `LevelDB` — the read-write replay produced exactly the
  mutation signature the rules warn about (new `.ldb`+log+
  MANIFEST, `CURRENT` modified). Caught by `git status`, restored
  with `git checkout` plus removing the replay's files, 15 keys
  re-verified. The rule bit once; it costs one command to undo
  and zero to keep.
- Gates: 5 suites PASS under `./rebuild.sh test`, `./nob test`
  and `./rebuild.sh release test` (release last); battery 11/11
  rc=0; `tests/` clean after every gate.

### Next

- D24 — the legacy v1 palettes: subchunks whose payloads predate
  the palette format, the last of the ordered drops.

## 2026-10-16 — The rest of the block entities: 7,067 documents into Java's lists, 21 ids from 10

### Built

- The carry table grows from the containers to every family the
  reference answers for (D22), each row carrying a kind:
  containers keep D15's fields, the shulker box joins them; the
  brewing stand's clock under `BrewTime` and gauge under `Fuel`;
  the spawner's seven shared-name ranges and one `SpawnData{id}`
  from `EntityIdentifier`; both sign sides as four JSON messages
  split on newlines with the escaped-empty pad, black and no glow
  beside them, `is_waxed`; the comparator's `OutputSignal`; the
  beacon's `primary`/`secondary` as `Primary`/`Secondary`; the
  gateway's `Age` and `ExitPortal` list as an `X`/`Y`/`Z`
  compound; the campfire's four `ItemTime`s as `CookingTimes`; the
  jukebox's record through the stack rule without a `Slot`; the
  bookshelf's `last_interacted_slot`; and the empty tier — sculk
  trio, lectern, skull, bell, enchanting table, daylight detector,
  end portal — id and position alone, measured to hold nothing
  else.
- `carry_block_entity` grew a per-kind switch over one base
  compound (id, x, y, z); D15's two gates — coordinates that name
  a block, no `LootTable` — stand before it unchanged. The stack
  rule gained its `with_slot` argument, and a `json_line` helper
  quotes one sign line the way JSON spells it (UTF-8 passes,
  control characters by name or `\u00XX`).
- The report struct's comments now name what carries (D15 + D22)
  and add the LootTable reason the dropped count always counted.

### Measured

- The real store, release: rc=0, 1m57 twice, the two output trees
  byte-identical (one md5). Three fields move, each predicted
  before the run and matched exactly: `block_entities_carried`
  4,194 → **11,261** (+7,067 — 4,376 empty-tier, 1,711 spawners,
  263 campfires, 262 signs, 231 comparators, 102 shulker boxes, 98
  brewing stands, 16 gateways, 4 jukeboxes, 3 bookshelves, 1
  beacon), `skipped.block_entities` 21,214 → **14,147**, and
  `skipped.items` 896 → **964** (+68 stacks inside the newly
  carried families). Every other count is unmoved.
- The predictor reproduces D21's four pins before predicting: all
  7,071 records read through mcme's own whole-record reader (7,054
  OK, 17 refusing — their 169 documents counted as 17), the
  subchunk columns from the scan's own rule, giving 25,408 counted
  (4,194 + 21,214) and items 896 to the document. The split
  between carried and dropped then falls out of the rules: only a
  document in a converting chunk with a readable record can carry
  — the 7,067 gained are all inside them.
- The sign's color is the reference's measured answer, not a
  choice: the game's int takes no path through the universal
  format, so every sign crosses as black with no glow — mcme
  mirrors it as constants and says so in D22.

### Method notes

- SNBT's two escapes (`\"` and `\\`) cannot spell a newline, so
  the modern sign's text is `mcme_nbt_set_string`'d onto the tag
  after the parse — a fabrication technique the pin needs, not a
  parser gap.
- `mcme_nbt_compound_put`'s key length is a claim the path lookup
  checks: writing `ExitPortal` as 11 (it is 10) failed every
  gateway pin while the field was there all along. The pins found
  it; the audit that followed checked every new literal.
- Predicting document counts needs mcme's own reader, not the
  oracle's: amulet parses 169 documents mcme's whole-record rule
  refuses, and each refusal counts once. The probe links the
  built objects and prints per-record status and counts; the
  mirror over amulet's parse supplies the ids.
- Gates: 5 suites PASS under `./rebuild.sh test`, `./nob test` and
  `./rebuild.sh release test`; battery 11/11 rc=0; ref_convert 12
  checks (the store check now fabricates seven D22 documents, the
  entities fixture's sign carries); `tests/` clean.

## 2026-10-15 — The other dimensions: 9,442 chunks in DIM-1 and DIM1, 105,168 keys out of `other`

### Built

- Convert answers the game's three dimensions (D21): a key's own
  dimension is read at its parse (len 9/10 omits it — overworld;
  len 13/14 speaks it at offset 8), and a number outside 0/1/2
  counts `other` like any key with nowhere to go. The scan carries
  the dimension on every reference, the sort's first key is the
  dimension (dim, cx, cz, cy), and the pairing of block-entity,
  tick and biome records to their chunks compares it before the
  coordinates — the fixtures hold nether and overworld chunks at
  the same (0,0), and an end chunk at the overworld's own (1,−2).
- Each dimension's chunks are written where Java reads them:
  `region/`, `DIM-1/region/`, `DIM1/region/`, through the same
  `mcme_java_dimension_dir` the editing verbs name, and the
  directory is made only for a dimension that has chunks — a store
  with no end chunks leaves no `DIM1`, absent as a container list.
  The chunk root gains its dimension's floor and height: yPos is
  −4 overworld and 0 nether/end (384 against 256), heightmap
  columns must sit inside that dimension's height, biome strata
  count from that floor.
- Counts: no new report fields. `chunks`, `subchunks`, `entities`
  and `chunk_data` simply stop being overworld-only; `other`
  becomes precisely "unparseable, or a dimension beyond the
  game's three".

### Measured

- The real store, release: rc=0, 1m57, two runs byte-identical
  JSON. The five moving fields were predicted before the run by
  passing the store's keys through `scan_key`'s own rules in python
  (the predictor's numbers, then the run): `other` 885,435 → **780,267**
  (780,266 unparseable + 1 dimension beyond the three), `chunks`
  36,849 → **46,291**, `subchunks` 340,599 → **392,032** (+31,938
  nether, +19,495 end), `entities` 10,745 → **15,501**,
  `chunk_data` 180,969 → **219,867** — all five exact. The record
  halves follow their own rules into the same run:
  `heightmaps_carried` +9,442 (one per new chunk, none refused) to
  46,290 with the 236 records on chunks without subchunks counted
  beside D20's one (1 → 237); `ticks_carried` +33,494 from all 235
  nether and end records, `skipped.ticks` still 0; `biomes_carried`
  24,977,632 with 628,064 counted; `block_entities_carried` 4,194
  with 21,214 counted — 662 documents across the 168 records, D15
  sending 171 across; `items` 896. `water_positions` and
  `legacy_subchunks` did not move: no second layer and no
  pre-palette version stands in the other dimensions' payloads.
- The fixture pins the halves a report cannot show: `DIM-1/region`
  and `DIM1/region` files with the parity payload's cells position
  for position, overworld yPos −4 beside nether and end yPos 0,
  the nether's own tick in `block_ticks` and its chest in
  `block_entities` with the overworld's lists empty at the same
  coordinates, the column-300 map refused under the nether's 256
  while its cells cross, and the carry test's store leaving neither
  directory behind. The report sits at 5 chunks / 10 subchunks /
  36,864 blocks / 1 `other`, ticks 4/5, biomes 383/386,
  heightmaps 2/6. The entities fixture's own nether keys convert
  now — chunks 2 → 3, three documents carried, `other` 2 → 0 —
  which is D21 working on a store nobody fabricated.
- Gates: 5 suites PASS under rebuild.sh, nob, and `rebuild.sh
  release`; battery 11/11 rc=0, ref_convert 12 checks (the new
  twelfth: the nether document read back from `DIM-1` against the
  fixture's record); `tests/` clean.

### Method notes

- The fixture ships exactly one nether subchunk key; it became the
  seed — both the selftest and ref_convert copy its payload to the
  fabricated nether and end chunks, so the parity cells in the
  output are the fixture's own bytes read through two independent
  walkers.
- One fabricated record pins both heights: a column at 300 sits
  inside the overworld's 384 and past the nether's 256 — the same
  bytes cross or count depending only on the dimension answering.
- The dimension-first sort is the bug class this drop was built to
  starve: the fabricated nether (1,−2) and end (1,−2) sit adjacent
  in key order, and the pairing is caught from both sides — the
  nether's lists must hold its records, the overworld's must stay
  empty at the same coordinates.
- `rebuild.sh` and `nob` compile into the same `build/obj`: run
  the gates one at a time — a concurrent pair tore an lz4 object
  mid-link and produced a phantom `undefined reference` that no
  source contained.

### Next

- Non-container block entities, entities, legacy v1 palettes.

## 2026-10-15 — The heightmap carry: 36,848 maps, 1 counted instead

### Built

- Convert writes the record's other half: the Data3D record's 512-byte
  int16[256] heightmap, already Java's numbers (first air above the
  floor) and already Java's order (x + 16z), packed the way 1.16+
  spells a map — 9 bits to a value, 7 values to a long, none spanning
  two, 37 longs — as the chunk's `Heightmaps` compound with one
  entry, `MOTION_BLOCKING`. A map crosses when the record decodes
  and every column sits inside the world's 0..384 (`heightmap_fits`);
  otherwise that half counts while the cells beside it still cross.
  No map → no key: the store's own shape, D19's rule applied to the
  record's other half.
- Counts (D12): root `heightmaps_carried` per map written;
  `skipped.heightmaps` for a converted chunk whose store holds no
  record, a record that will not decode (its map lost with it), a
  column outside the height, an orphan record's map; a tombstone
  counts neither half. The drops line grows `heightmaps`, the JSON
  grows `heightmaps_carried` and `skipped.heightmaps`, the carried
  sentence reports the total.
- New constant `MCME_BEDROCK_HEIGHTMAP_COLS` (256) beside the biome
  cells' 64: the first build packed through the cells constant and
  the pin caught columns 64..255 reading zero.

### Measured

- The semantics, against the reference's own read: 2,560 columns
  across ten chunks of one real store — 2,188 exactly top + 65 (the
  value is the first air, relative to the floor at −64); the 372
  that differ all have something that does not block motion on top
  (short grass, vines, ferns, torches, flowers, snow layers — 23
  names) or top one of 29 columns the game's own bookkeeping still
  counts below (crimson stairs, trapdoors, a fence, a chain, a
  path) — and every differing value points at a real block below,
  none at nothing. Water, ice and leaves tops never differ: fluids
  and leaves count. That is `MOTION_BLOCKING` — not
  `WORLD_SURFACE` (would count the grass), not
  `MOTION_BLOCKING_NO_LEAVES` (would drop the canopy), not
  `OCEAN_FLOOR` (the store's dominant value, 127 on 4,475,483
  columns, sits at y62 — sea level, not the floor).
- mcme's own verb agrees with the record end to end: `chunk height
  (0,0)` reports surface y112 moss — the record's value 177;
  `chunk height (13,0)` reports a poppy at y63 — the record's
  value 127 counts the y62 ground under it, the plant left out.
- Real world, `--json convert` to java/1.20 (release): rc=0, 1m56 —
  the same 36,849 chunks / 340,599 subchunks / 1,395,093,504 blocks,
  **`heightmaps_carried` 36,848, `skipped.heightmaps` 1**, and
  36,848 + 1 = 36,849 = the chunks exactly (the one chunk whose
  record the store never held; no orphan, no refusal — every
  column sat inside 10..309). Every other count byte-identical to
  D19's run: the two reports differ in exactly the two new fields.
- Gates: 5 suites PASS under rebuild.sh, nob, and
  `rebuild.sh release`; battery 11/11 rc=0, ref_convert 11 checks;
  `tests/` clean.

### Method notes

- Nothing to mirror: amulet's Bedrock reader parks the columns in
  `misc["height"]`, its Java writer packs `misc["height_mapC"]`, and
  nothing bridges them — the golden java fixture's ten chunks each
  hold an empty `Heightmaps` compound. ref_convert pins mcme's own
  spec instead (D18's pattern): its independent packer lays the
  fabricated columns over all 37 longs, and the selftest pins word
  0, word 1's wrap, word 36's four, and the formula everywhere. The
  fabricated columns spell their own coordinates — column i reads
  100 + i — so an order slip in either packing shows as a wrong
  number.
- The fixture's new record is map-only: no arrays (a record may
  hold none — the containers keep their no-key shape) and one
  column at 400, past the world's 384, dropping only the map. The
  reports pin 1/3 (bad map, orphan, junk) and 0/2 (entities
  fixture: both chunks record-less).
- The `chunk height` verb's earlier timeouts were the ASan build's
  speed opening a 258MB store, not a hang: release answers in
  10.8s, and the measurements above went through the reference's
  own decoder in a single process.

### Next

- Other dimensions, non-container block entities, entities,
  legacy v1 palettes.

## 2026-10-15 — The biome carry: 21,696,544 cells, 102,688 counted instead

### Built

- Convert stops counting Data3D records (`0x2b`) as `chunk_data`.
  The scan collects them as `BiomeRef`s holding the chunk's full
  stored key beside the others; the run pairs each chunk with its
  record (LevelDB keys are unique — one record per chunk), and a
  record whose chunk holds no subchunks counts after the loop,
  cells and all — D15's orphan rule, extended. Decode reads past
  the 512-byte heightmap to the arrays from the world floor (−4):
  signed header shifted one (low bit amulet's palette flag,
  ignored), the game's ≥0x80 tail walked byte-wise, width 0 → one
  u32 id, widths 1..16 → Tomcc packed words + i32 palette, cells
  sampled every fourth block into Java's `y*16+z*4+x` order.
  Refusals (width >16, 24+ arrays, empty palette, index past
  palette, truncated payload) drop the record whole, once, into
  `biomes`. The dump name for the tag is `biome3d` (the 0x2d-era
  tag keeps `biome`).
- The map: 86 rows in translate.c between
  `---- BEGIN/END GENERATED BIOME MAP ----`, ids sorted (the
  lookup binary searches), each row's Java name the reference's
  own two-leg answer (bedrock unpack → universal → java) — D17's
  pattern pointed at ids. An id the table does not hold —
  measured, 193/194/195 — lands on plains, the reference's own
  fallback with its warning, and the cell counts:
  `biomes_carried` and `biomes` are per-cell, exclusive.
- Shape: an array at the stratum → a `biomes` container on that
  section, palette the distinct resolved names sorted by name,
  `data` only past one name, bits ceil(log2) at least 1,
  floor(64/bits) values to a long. No array → no key: the game
  reads absent as plains, which is exactly the reference's
  answer, so D15's placeholder container is deleted. The section
  set stays mcme's pinned non-air one.
- Counts (D12): root `biomes_carried`; `skipped.biomes` for
  fallback cells, cells with no section, one per refused record,
  an orphan's cells; the drops line grows `biome cells`, the JSON
  grows `biomes_carried` and `skipped.biomes`, the carried
  sentence reports the total. `chunk_data` loses the `0x2b`
  records wholesale.

### Measured

- Census of the same store: 36,848 overworld records,
  568..21,040 bytes, arrays −4..top contiguous; 21,799,232 cells;
  54 distinct ids — 51 the map names, 3 it does not → 75,424
  cells on the fallback; widths 0..3; 27,264 cells (0.13%) sit at
  strata with no stored subchunk.
- Real world, `--json convert` to java/1.20 (release): rc=0, the
  same 36,849 chunks / 340,599 subchunks / 1,395,093,504 blocks —
  **`biomes_carried` 21,696,544, `skipped.biomes` 102,688**, and
  21,696,544 + 102,688 = 21,799,232 = the census's cells exactly
  (102,688 = 75,424 fallback + 27,264 sectionless; no refused
  record, no orphan in the real store). `chunk_data` 217,817 →
  180,969, exactly −36,848. Every other count byte-identical to
  D18's run. The ASan run reports the same JSON (1m55s release vs
  7m49s under the sanitizers — the sanitizer, not the biome work).
- Gates: 5 suites PASS under rebuild.sh, nob, and
  `rebuild.sh release`; battery 11/11 rc=0, ref_convert 10
  checks; `tests/` clean.

### Method notes

- The mirror needed no reimplementation: amulet decodes `0x2b`
  itself (`_decode_height_3d_biomes`, `_decode_packed_array`), so
  ref_convert decodes with the reference's own decoder, runs each
  id through the live PyMCTranslate pipeline, and lays the cells
  out in Java's container order — all 256 fabricated cells
  compared positionally against mcme's containers (palette order
  is mcme's; content is not). The fabricated mixed array marks
  one cell on the x, z and y axes so a sampling slip in either
  decoder shows as a wrong cell, and its id-194 cell fires the
  reference's `Reverting to plains` warning beside mcme's own
  count of the same cell.
- The table re-pins each battery run: markers present, ids
  sorted, the id set equal to the ones the Bedrock tables name
  (any other id falls back on both sides), every row's answer
  the live reference's — 86 rows.
- The heightmap half rides decoded-past and uncounted: it is not
  a skip, it is the heightmap drop's cargo (next).
- The fixture ships no `0x2b`, so both tools fabricate the same
  three records: six strata from the floor (the id-3 rename,
  two strata with no subchunks beneath them, a uniform id 1, a
  width-2 mixed palette with the marked cells, a uniform id 6,
  15 bytes of 0xFF), an orphan of two arrays, a junk payload.
  The report pins 255 carried / 258 counted; the selftest's
  decoder units cover every refusal on their own.

### Next

- Heightmaps: the record's other half, already read past — then
  other dimensions, non-container block entities, entities,
  legacy v1 palettes.

## 2026-10-15 — The tick carry: 74,361 scheduled updates, none dropped

### Built

- Convert stops counting tick records as `chunk_data`. The scan
  collects overworld records of both tags — `0x33` and `0x3a` — as
  `TickRef`s beside the block-entity refs, sorted chunk-then-tag so
  one walk pairs each chunk with its own records; a record whose
  chunk holds no subchunks counts after the loop, entries and all.
- The carry reads `currentTick` + `tickList`; a tick's `blockState`
  is shaped exactly like a palette entry, so D17's table answers it
  — no second map. An entry carries whole as Java's
  `{i, p, t, x, y, z}` or counts as dropped: `p` 0 (Bedrock records
  no priority to carry and inventing one would reorder
  simultaneous ticks), `t = time − currentTick` as an Int (bounded
  before the subtraction — two full-range int64s would overflow it;
  outside int32 the entry drops), coordinates verbatim. The split
  reads the *translated* name: `minecraft:water`/`minecraft:lava`
  to `fluid_ticks`, every other block to `block_ticks`, both keys
  standing in every chunk, empty where nothing lands.
- Counts (D12, never a failure): `ticks_carried` at the top level,
  `skipped.ticks` for what does not arrive — an entry missing
  x/y/z/time, a t outside int32, a blockState the map refuses (each
  whole or not at all), a record that will not decode (once — half
  a record is not a count), an orphan's entries. Tick keys of other
  dimensions keep the one rule every tag has: `other`. The human
  drops line grew `tick entries`, the JSON grew `ticks_carried` and
  `skipped.ticks`.
- Pins: selftest fabricates four records on its fixture copy —
  a scheduled record whose entries must split through the map
  (flowing_water → `water` to fluid at t 0, red sand to block at
  t −5, magma to block at t 20), an entry with no z, an orphan
  record, a payload that is not NBT — and asserts the lists, the
  counts (3 carried, 4 counted), and the empty-list shape on the
  untouched chunk. ref_convert fabricates the same four on its own
  copy: amulet reads no tick records, so the tool *states* the
  spec — recomputes both lists from the records through the same
  pinned reference — and pins all seven counts in its exact report
  check. 8 checks.

### Measured

- Census: 2,235 tick records, three dimensions — 1,623 `0x33` and
  377 `0x3a` in the overworld, the rest other dimensions. The two
  tags both carry explicit times: `0x33` holds water and lava flows
  from +28 pending to −31,149,718 (31 million ticks overdue),
  `0x3a` holds fire spread scheduled +15 to +36 — a second schedule,
  not a candidate list, which is why both tags carry.
- Real world, `--json convert` to java/1.20: rc=0, 36,849 chunks,
  340,599 subchunks, 1,395,093,504 blocks — **`ticks_carried`
  74,361, `skipped.ticks` 0** (every overworld record's chunk
  converts; no orphan, no undecodable record). `chunk_data` fell
  219,817 → 217,817, exactly −2,000 = the overworld records leaving
  the skip bucket. Every other count is byte-identical to D17's run
  (carried 4,023, water 1,455,655, legacy 0).
- Gates: 5 suites PASS under rebuild.sh and nob with the fabricated
  records among them; battery 11/11 rc=0, ref_convert 8 checks;
  `tests/` clean.

### Method notes

- The pin had to be written rather than mirrored: PyMCTranslate is
  never asked a tick, so ref_convert does for D18 what it did for
  the waterlogging policy — records the spec mcme carries, then
  holds the output to it. The report check stays exact, so the
  dropped-side arithmetic (1 + 2 + 1) is pinned too, not just the
  lists.
- The goldens' db directories were compacted by an outside LevelDB
  open at 13:44 — found by mtime forensics after `git status` grew
  unexpected `tests/` entries mid-task. Every operation of this
  session that touches the fixtures reproduced clean afterwards
  (plain build, both suites, battery, ref_convert, both converts),
  so the write was not this repo's own tooling; `git checkout` +
  `git clean` restored the committed store and the post-restore
  battery re-verified. The four not-mine files and `tests/` stay
  out of every staging list regardless.

### Next

- Biomes: the Data3D record (`0x2b`, the 768-byte biome +
  heightmap payload) — the biome names have to cross D17's table
  the way ticks did. Then heightmaps, other dimensions,
  non-container block entities, entities, legacy v1 palettes.

## 2026-10-15 — The block map's name half: 309 rows, and the reference answers them all

### Built

- `mcme_translate_block` no longer passes block names through. A
  generated table — 309 rows between the GENERATED NAME MAP markers
  in translate.c — says which Java spelling each Bedrock name takes,
  in three shapes: 132 wholesale renames (`flowing_water` → `water`,
  `magma` → `magma_block`, `snow_layer` → `snow`, and the blocks
  Java 1.20 does not have → `air`), 177 rules on one state value
  (`log` + spruce → `spruce_log`, `sand` + red, `torch` + west →
  `wall_torch`, a mushroom's bit count → `mushroom_stem`), and 33
  rules on a *pair* of values where no single value decides — a dead
  coral's colour times `dead_bit`, a cauldron's liquid only once
  `fill_level` is nonzero. Lookup: binary search to the name's
  first row, rows most-specific first, first row whose conditions
  all hold wins, else the name's default row, else the name
  unchanged — the reference's answer for a block it does not rename.
- Integration: the axis-suffix family and the mushroom-stem
  exception are keyed on the *Java* name (`spruce_log` ends in
  `_log`), the reference-forced wire states stay keyed on the
  Bedrock one, and the hand-written red-sand special case is gone —
  the table covers it, so one mechanism answers every name.
  decisions.md D17.
- `tools/ref_convert.py` grew a seventh check: it reads the markers
  back out of translate.c, re-answers every row against
  PyMCTranslate at the pinned versions, and refuses a table not
  sorted in the byte order the binary search walks. The check also
  shook out a latent bug in the tool itself: the answer cache keyed
  on values alone, so `Byte` 10 and `Int` 10 — which the reference
  answers *differently* — shared one entry; the key now carries the
  tag type.
- selftest `test_translate` grew 15 cases across the table's shapes
  (wholesale, one-state, two-condition, numeric values, the →air
  blocks), including `spruce_log` keeping `axis` and an empty
  cauldron staying `cauldron`.

### Measured

- Census of the closed Oct-14 snapshot, all three dimensions:
  392,032 subchunk payloads (0 decode failures) and 2,235 tick
  records → **828 distinct names**, 4,011 distinct (name, states)
  combinations. Java 1.20 has 602 of the names; **226 it does not**,
  and before this entry every one of those passed through the map
  verbatim.
- Generation: all 4,011 real combinations and 3,753 state-product
  combinations checked against the reference — 0 mismatches,
  0 combinations no flat rule could carry, 0 names the reference
  cannot answer. 163 of the names have no `get_specification`
  entry although the translation answers them fine (the spec table
  and the map are separate tables); their rules come from the
  world's own states.
- Gates: 5 suites PASS under rebuild.sh and nob; battery 11/11
  rc=0 (ref_convert now prints 7 checks); goldens untouched.
- Real world, `--json convert` to java/1.20: rc=0, 36,849 chunks,
  340,599 subchunks, 1,395,093,504 blocks;
  `block_entities_carried` 4,023; `skipped.block_entities` 20,723;
  `skipped.items` 842; `water_positions` 1,455,655;
  `skipped.chunk_data` 219,817; `entities` 10,745; `other` 885,435;
  `legacy` 0.
- Residue, counted by walking every output chunk's decompressed
  palettes (742 distinct names): of the 226 java-invalid input
  names, **134 are renamed away** and **92 survive — every one of
  them because the reference itself keeps the name**: spot checks
  through the pinned pair give `planks`+birch → `minecraft:planks`,
  `red_flower`+poppy → `minecraft:red_flower`, `wool`+red →
  `minecraft:wool`, `short_grass` → itself (with the properties
  dropped, `props={}`), while `log`+birch → `minecraft:birch_log`
  and `bed`+red → `minecraft:red_bed` — the table matches those
  answers row for row. The 92 are 83 `minecraft:` names (1.21-era
  content the 1.20.80 bank predates — pale_oak, poplar, resin,
  leaf_litter, wildflowers — plus legacy aliases `leaves`,
  `leaves2`, `planks`, `wool`, `wood`, `red_flower`…) covering
  77,591 positions, and 9 `fxnt:` names the census found in the
  bedrock palettes (61 positions), outside the map's namespace
  entirely. Java/1.20 cannot read those — the bank-version gap,
  named in D17's cost, fixed only by moving the version pair.

### Method notes

- The reference is tag-type sensitive and the world agrees with the
  spec about types: `huge_mushroom_bits` `Int` 10 is
  `mushroom_stem`, `Byte` 10 falls to the default
  `brown_mushroom_block`. Enumeration, the pin, and the selftest all
  ask with the spec's own spelling; the C lookup stays
  type-insensitive because real palettes carry the spec's types.
- Region chunks are zlib-compressed inside the `.mca`, so grepping
  the output for names sees nothing — the first residue scan
  reported a false green (empty name set, "0 residue"). The scan
  that counts walks all 36,849 chunks through the reference tool's
  own decompressing `walk_chunk`; residue claims come from it.

### Next

- D18: the tick carry — pending-tick records (0x33) and random
  ticks (0x3a) split into Java's `fluid_ticks` and `block_ticks`,
  translated through the same table. Then the rest of Phase 4 in
  order: biomes (Data3D), heightmaps, other dimensions,
  non-container block entities, entities, legacy palettes.

## 2026-10-14 — Load test: the game keeps the documents and rewrites the id to its own spelling

### Built

- Nothing; this entry is the experiment D16 closed on. A copy of
  the D15 snapshot holds three full chests (27 lava-bucket stacks
  each) side by side: a control the game itself wrote (`Chest`,
  -994,18,79), one mcme wrote as `minecraft:chest` (-994,20,79)
  and one as `chest` (-993,19,79) — the namespaced spelling
  mcme's own writes used, and a wrong-case one that is neither
  spelling's canonical form. Loaded in the game (1.26.52.3),
  played about five minutes (Time +5,996 ticks), Save & Quit,
  store read back.

### Measured

- Every document survived, and all three ids came back `Chest`.
  The game accepts either spelling on load — nothing dropped, no
  items lost — and rewrites the id to its own spelling at the
  next save. The `minecraft:chest` chest demonstrably worked
  in-game: its contents changed during the session (27 stacks
  down to 4, the player interacting with it).
- The re-serialization is the game's usual touch-up, not the
  id's fate: it added `BlockEntityVersion: 0` and dropped
  `isMovable` — on the control document mcme never touched, the
  same as on the two it did. `Items`,
  `pairlead`/`pairx`/`pairz` and the position carried unchanged,
  and level.dat kept the name and spawn mcme wrote.
- D16 does not reverse: nothing dropped, so there is no repair
  job — mcme's namespaced writes load, work, and the game
  canonicalises them itself. `put` keeps storing what it was
  given; the game's save is the normaliser.

### Method notes

- Two earlier sessions went to the operator's live world instead
  of the prepared copy — the world list shows both under the same
  name. The store told them apart without guessing: in the
  prepared world `level.dat` still held the prep write and
  `Time`/`currentTick` never advanced, while the live world's db
  and `level.dat` carried the session. A world that ticked says
  so in level.dat; document reads alone cannot.
- The load-test world survives at
  `minecraftWorlds/mcme_D16_loadtest` with the post-load store in
  place.

### Next

- None for D16 — the question it named is closed. The load-test
  world and the operator-world strays from the two misdirected
  sessions await disposal.

---

## 2026-10-14 — The write side meets the game's spelling: `put` takes a bare savegame id

### Built

- `mcme_bedrock_put_block_entity` now refuses an id in *neither*
  spelling instead of an id without a colon. The old premise —
  "the reference's decoder requires [a namespace] … the game and
  amulet drop" anything else — measures backwards: the real-world
  store holds 23,806 documents across 43 distinct ids and every
  one is bare (`Chest`, `TrappedChest`, `BrushableBlock`), the
  wiki calls the field "the savegame ID of the block entity", and
  `strings` on the game's `libminecraftpe.so` (v1.26.52.3) holds
  the registry literals bare — `Chest`, `Barrel`, `Hopper`,
  `BlastFurnace` and a dozen more — with no bare
  `minecraft:chest` anywhere. The refuser was refusing the game's
  own format.
- What stays refused is a shape neither spelling has: no id, an
  empty side of the colon, a second colon, a character an id in
  neither form holds (letters, digits, `_`, `-`, `.`) — USAGE,
  before any byte is written. The id itself is stored exactly as
  given: mcme checks the shape both spellings share, not the
  registry, which is the game's and versioned. `Chest` in,
  `Chest` out; `minecraft:chest` in, `minecraft:chest` out — and
  D15's read path takes either back.
- Pins: `test_bedrock_entity_write` refuses an id-less document,
  an empty name and a stray space, and round-trips a bare id
  (put `{id: "Chest"}` at a block, read `Chest` back with the
  position's coordinates set). `test_convert_carry` now puts the
  furnace under the bare id, so the write API itself feeds both
  spellings to convert — same five carried, the same
  `minecraft:furnace` answer under Java. The reference
  (`ref_bedrock_entity`, 27 checks) stores a bare id verbatim,
  cross-reads it against amulet's own loader, and keeps the
  refusal pin on documents in neither spelling, record untouched.

### Cross-check

- 5 suites PASS under ASan (debug + release); battery 11/11
  rc=0; goldens untouched (`git status tests/` empty); release
  selftest PASS.
- Measurements behind the rule: the D15 world snapshot's id
  census (43 distinct, 0 namespaced), the wiki's field
  definition, and the game binary's own strings.

### Known gaps

- Still untested, and only the game can test it: which
  spellings survive a load. mcme's earlier writes are
  namespaced; this world holds none, so nothing in it says what
  the game does with them — or with the wrong-case `{id:
  "chest"}`, which now stores. The experiment is one world
  holding both spellings, loaded, store read back after.

### Next

- That load test: a prepared copy with a `Chest` doc and a
  `minecraft:chest` doc side by side, opened in the game, the
  store compared afterwards — then decide from what the game
  left standing whether put writes one spelling after all.

---

## 2026-10-14 — Convert carries waterlogging and containers, in both spellings Bedrock writes

### Built

- `convert` gained the two drops D14 named as Next. Layer-one
  `minecraft:water` over a block becomes Java's
  `waterlogged: "true"`, but only where the Java block actually
  has that property (the translate map answers); layer-one water
  that waterlogs nothing (over water, over air) counts as
  `water_positions` and never fails — 1,416,949 positions in the
  real world, the fixture pinning 3,280.
- A block-entity document carries when three things hold: the id
  is one of ten container types, x/y/z are integers, and the
  world has filled it (no `LootTable` — a container written empty
  where the world would grant items is the half-answer mcme
  refuses to write). Carried: Java's id, coordinates, the stacks,
  nonempty `CustomName`, `Lock`, smelter timers as ints. Every
  other field stays behind and is counted, never guessed.
- The record read is whole-record (`record_walk`), shared by
  `put` and the convert walk: a record either decodes document by
  document or counts once — half a record is not something mcme
  answers for, and the write splicer must agree with the reader.
  `put` gained the missing insertion point while at it: an absent
  document now sits at the record's end, so putting a *new* block
  entity appends instead of replacing the record's others.
- Three spellings had to meet because two writers authored the
  inputs: ids (`Chest` from the game, `minecraft:chest` from the
  frozen fixture) and stacks (`Name`/`Count`/`Damage` vs
  `id`/`count`/`damage`) are accepted in either spelling and one
  answer leaves — Java's id, Java's `Count`. The goldens were
  never regenerated; both spellings passing is the point.
- The stack rule reads the bookkeeping the game writes beside a
  stack's identity as noise it can drop without dropping the
  stack: `WasPickedUp`, the `Block` compound a block item repeats
  about itself (measured: all 5,455 in the real world are
  exactly name/states/version), `tag{Damage: 0}` the undamaged
  marker. Any other field is item data with no Java spelling —
  the stack drops whole and `skipped.items` counts it.
- The record walk's decoder accepted only modified UTF-8, so one
  emoji in one item name refused the record that held it: three
  records, 621 documents, 480 of them carryable containers
  (measured in the real world). `mcme_mutf8_decode`/`valid`/
  `decoded_len` now accept standard UTF-8's four-byte sequences
  beside modified UTF-8's surrogate pairs — one decoder, both
  editions' writers — and still reject what neither spells
  (overlong, bad continuation, past U+10FFFF).
- The report grows `block_entities_carried` (top level) and
  `skipped.items`; another dimension's keys hit `other` first —
  one rule for every tag, the convert walks the overworld.
  `test_convert_carry` covers both id spellings end to end (docs
  through `put` plus records appended as the game's own bytes),
  the emoji CustomName round trip, the loot drop, and the stack
  allow-list; `tools/ref_convert.py` mirrors every rule in
  Python (6 checks).

### Cross-check

- 5 suites PASS under ASan; battery 11/11 rc=0; goldens
  untouched (`git status tests/` empty); release selftest PASS.
- Real world (the LP world's store snapshotted while the game
  was closed; the game held the world during the run, so the
  snapshot was converted): `--json convert` to java/1.20, 1m33s.
  35,553 chunks, 329,641 subchunks, 1,350,209,536 blocks;
  `block_entities_carried` 0 → 3,467;
  `skipped.block_entities` 23,102 → 20,253; `skipped.items`
  0 → 711; `water_positions` 1,416,949; `other` 886,629;
  `legacy` 0.
- The numbers close end to end. 23,806 overworld documents =
  3,467 carried + 20,253 counted + 86 (the documents inside the
  16 records whose strings are binary blobs — counted once per
  record, by design). The container census: 7,749 documents with
  the ten ids − 4,006 with a `LootTable` = 3,743 eligible =
  3,467 carried + 231 in chunks mcme writes no Java chunk for +
  45 behind those 16 records. Scanning the output `.mca` files
  finds exactly 3,467 block_entities — the report counts what
  the world holds.

### Known gaps

- The write half still wants a namespaced id (`put` takes
  `minecraft:chest`); the game writes `Chest`, and whether the
  game reads back mcme's spelling is untested. D15 is the read
  path.
- 16 records (102 documents) hold binary bytes in a string
  field (`EntityStorageKeyComponent.StorageKey`) — neither
  encoding can spell them, so they refuse and count; 45 of the
  documents behind them are containers. Reading them would mean
  inventing text, which D15 declines.
- 711 stacks dropped for fields with no Java spelling (enchants,
  display names, brew state, nested `Items`); each widening is a
  measurement first, a mapping second.
- Converted output on this world: 228 MB of regions, same
  conversion as the pre-D15 run but with 3,467 containers and
  their stacks standing in it.

### Next

- The write side meets the game's spelling: `put` accepting
  short savegame ids, and a real load test of what the game
  does with namespaced ones.

---

## 2026-10-13 — Compaction writes the game's own blocks (bare DEFLATE)

### Built

- `block_builder_finish` now assembles a block's contents first
  and then chooses how to store them: bare DEFLATE (type 4) when
  that is smaller than raw, raw (type 0) when it is not. The
  trailer still names the choice, and the CRC32C still covers the
  stored bytes plus that type — D9's on-disk rule, untouched. The
  writer's old comment said compressing "would cost golden
  diffability"; the goldens diff merged views through the Python
  binding and never looked at block bytes, while the real cost of
  staying raw sat on disk: the same world at 259 MB in the game's
  hands and 2.27 GB in ours (D14).
- The decode side needed nothing — the fixtures have always
  carried type-4 blocks. The encode side had no coverage: the
  compress suite's round-trip and bound tables gained
  `MCME_COMPRESS_RAW_DEFLATE`.
- `test_compact` now pushes a 64 KiB compressible payload and a
  16 KiB noisy one through both compactions and both reopens, and
  asserts the compacted table is smaller than the compressible
  payload alone. View equality passes even if the write path
  silently stops compressing; the size assertion is what makes
  that failure loud.

### Cross-check

- 5 suites PASS under ASan; reference battery 11/11 rc=0,
  including ref_bedrock_write — Amulet reads the compressed
  tables mcme now writes. Goldens untouched, align.py 0.
- Real world (the copy, world closed): `db compact` over its
  2.27 GB raw table → one 460 MB table in 1m35s;
  `chunks --count` 47,253, the same count the store had before.
  4.7× off mcme's own raw output, with the game's 259 MB still
  ahead by 1.78× — the block-size gap, recorded in D14 as its
  own dial.

### Known gaps

- Stock google/leveldb refuses a world mcme compacted (type 4 is
  not a codec it knows); D14 accepts that — it never read
  Bedrock worlds either.
- Compaction is 36s → 1m35s at this size: deflate over every
  payload, once per compact.
- Block target stays 4 KiB where the game's ~100 KiB blocks
  compress ~1.8× tighter; revisiting that is a separate
  decision.

### Next

- Convert's loudest drops (D12): block entities and waterlogging
  carry-over.

---

## 2026-10-13 — LevelDB tables are read a block at a time; file size is no longer a refusal

### Built

- The table reader streams. `mcme_ldb_table_open` stats the
  file and keeps a `FILE *`: the footer is read from the
  end, the metaindex and index blocks are read and freed,
  and the scan reads one data block at a time — verify,
  decode, copy the entries out, free it. The old path read
  the whole file into memory and refused anything over
  512 MiB, and it did so *after* the read: the ceiling did
  not even save the allocation it was defending.
- The 512 MiB number survives as a per-block guard: an
  index handle naming more than that is refused before
  anything is allocated. That was the ceiling's real job —
  a bogus size must not drive an allocation — and at block
  granularity, where reads actually happen, it still holds
  it while the file beneath can be any size the store has.
- With the slurp went the split ownership that needed
  tracking: an uncompressed block used to alias the file
  buffer while a decompressed one was recorded on the
  handle for `table_close` to free. Now every block read
  is owned by its caller and freed after its iteration,
  compressed or not — one rule — and the `inflated` array
  went with it.

The bug was self-inflicted and total. D11 compaction
writes the store as one table, so editing a real world —
`db put`, `db del`, any write past the log trigger —
compacted it into a single `.ldb`, and every mcme verb
that opens the store then refused that file: `info`,
`chunks`, `fill`, `get`, `convert`, `prune`, all dead on a
world mcme itself had written. A 2.2 GB one, reported
from the field.

### Cross-check

- New selftest `big table`, on a copy of the bedrock-forge
  store: the `.ldb` is spliced with a 512 MiB hole right
  before its footer — every handle still names its offset,
  the footer is still the last 48 bytes, and the file now
  reports over the old ceiling — and the store must open
  and answer with the byte-identical view, sequences
  included. Then the index handle is forged to claim
  nearly the whole file: in bounds, so the bounds guard
  passes; over the block ceiling, so the open must refuse
  it and the message must say the size is why. Both
  halves fail under the old reader, and the hole keeps
  the test instant — the size is what is under test, not
  the volume.
- The real world that reported the bug,
  `LP_pLAyME_28-Aug-26_copy`, whose db is one 2.2 GB table
  mcme's own compaction wrote: `info` reads it with no
  warn; `chunks` counts 46,594 (36,916 overworld / 4,290
  nether / 5,388 end) in 12 s; and the command that
  failed — `chunk prune <world> -200 0 -400 -400 0 -500`
  — prunes all 104 of its columns in 24 s, 46,594 →
  46,490 chunks, the tombstones one 18 KiB log record
  over a view of 1,461,244 keys, under the 8M ceiling.
- `./rebuild.sh test` PASS — 5 suites, 0 failures;
  `./rebuild.sh release` builds; ref tools 28 (bedrock
  column) + 25 (bedrock entity) + bedrock write (PASS) +
  13 (prune) + 4 (convert) + 8 (java blocks) + 8 (java
  fill) + 17 (region chunk) + 14 (inventory) + 9 (region
  write) + 12 (compact); golden worlds untouched — every
  fixture is copied before it is opened.

### Known gaps

- Compaction still writes the whole store as one table.
  It reads back at any size now, but a multi-gigabyte
  single file is not a shape stock LevelDB produces, and
  whether Mojang's own reader takes one has not been
  verified here. Splitting compaction output across files
  is the honest fix.
- The merged view is still built whole in memory, so a
  store's ceiling is RAM rather than file size — this
  world's view is ~2.4 GB of key and value bytes.
- Every successful write reloads the view from disk
  (D9-era choice: one merge implementation), so a prune
  sliced into many batches pays a reload per batch;
  slicing by bytes instead of by count would make any
  realistic prune one record and one reload.

### Next

- Convert's two loudest drops — block entities and the
  waterlogging layer — remain the next phase of that
  feature.

---

## 2026-10-13 — Prune: `mcme chunk prune`, whole chunks deleted by a box of columns

### Built

- `mcme chunk prune <world> <x1> <y1> <z1> <x2> <y2> <z2>
  [--dimension <name|number>]` deletes chunks — the unit the
  game saves — with fill's box as the selector (D13): corners
  in any order, X/Z floored to chunk columns by the rule every
  chunk verb speaks, and **Y parsed and ignored**, because a
  chunk is all of its Y and "delete most of a chunk" is not a
  state any reader keeps. One probe speaks both worlds. Human
  success is silent; `--json` reports `chunks`, `keys`
  (Bedrock) and `region_files` (Java), the field the other
  world does not have staying 0.
- The Bedrock half is a scan: every key
  `mcme_bedrock_key_parse` names for a reached column —
  subchunks in both spellings, the version record, the
  biome/heightmap record, block entities, pending ticks, every
  fixed-shape record addressed by the chunk's x/z and
  dimension — is tombstoned in batches of one log record each,
  sliced so a world-scale prune stays under the log's ceiling
  (D10), all under the exclusive flock the game takes, so a
  world the game has open refuses before a key is touched.
  Keys that are not chunk-shaped — actor records, level
  metadata, portallists — are never touched: mcme deletes
  what it can name and guesses no tag numbers. The chunk count
  is distinct (x, z) among the deleted keys: six subchunk
  records are one chunk, not six. An empty selection writes
  nothing — the log does not grow for a box already pruned.
- The Java half punches holes in two passes: every region
  file the box reaches is opened and read first, so a file
  that will not read refuses the whole prune with nothing
  written (the check fill makes, at the same moment) and a
  file the world does not have is a hole, not an error; then
  each present slot is zeroed by the new
  `mcme_region_remove_chunk()` — the location entry and the
  timestamp, the same bytes a slot the file never had carries,
  the format's own "nothing here". The entry goes before the
  timestamp: a reader racing the write finds the hole, never
  a stale stamp. The payload bytes stay in the file until
  `region compact` rewrites it around the survivors, exactly
  as Bedrock's tombstones stay until `db compact` — prune is
  deletion, not shrink, and the compact verbs own reclaim
  (D10/D11), one verb one truth.
- `java_dimension_dir` left `main.c` and became
  `mcme_java_dimension_dir()` in `world.c`/`world.h`: one
  layout rule beside the probe that decides a world is Java's,
  so fill, get, set and prune all speak the directory the same
  way (0 → region/, 1 → DIM-1, 2 → DIM1, anything else
  usage).
- Exit codes run through the house refusal vocabulary: a junk
  coordinate or a dimension number Java keeps no directory for
  is 1, a path that is not a world or a dimension the game has
  not made is 2, a region file that will not read is 3, a
  store error — including the LOCK the game holds — is 4.

### Cross-check

- Selftest `test_prune`, both halves on fixture copies:
  Bedrock — the fixture's eight keys, a reversed-corner box
  tombstones the six of chunk (0,0) (report 1/6/0), the key
  reads back as a tombstone, the live count drops to 2; the
  same box again is 0/0/0; the column at (1,-2) goes next,
  the nether key only with its own flag; refusals — a NULL
  argument usage, a plain directory not-there, and a second
  writable handle held against the store making the prune's
  flock fail as store (flock is per descriptor, so the two
  handles conflict even in one process). Java — the copy's
  four slots asserted present first, a box at y 300 prunes
  chunk (0,0) alone (1/0/1) with the survivors and
  r.-1.-1.mca standing, `region compact` shrinks the file
  (3 chunks, after < before) and the hole stays a hole, an
  empty column is 0/0, a missing DIM-1 is not-there, a
  dimension number of 7 is usage, and an 8-byte corrupt
  region file refuses as not-readable with its bytes — and
  the other file's — byte-for-byte unchanged.
- `tools/ref_prune.py`, 13 checks: every pruned Bedrock key
  raising KeyError in **amulet's own LevelDB binding** while
  the neighbour and the nether key return bytes; `mcme chunks`
  agreeing before (8 keys, 3 chunks) and after (0, 0); each
  `--json` report exact and human success printing nothing;
  the held LOCK (a python `fcntl` on the store's LOCK file)
  answering 4 with no report; the exit codes 1/2/2; and for
  Java the sector table walked with no mcme code — the slot
  empty, the survivors' frames byte-identical after `region
  compact`, r.-1.-1.mca byte-identical throughout, `chunk get`
  answering 2 for the pruned position, java exits 1/2/1, and
  the corrupt file answering 3 with both files' bytes
  untouched.
- Verified at commit: `./rebuild.sh test` PASS — 5 suites,
  0 failures; `./rebuild.sh release` builds; ref tools 8
  (java fill) + 8 (java blocks) + 12 (compact) + 9 (write)
  + 17 (chunk) + 14 (inventory) + 25 (entity) + 19 (bedrock
  write) + 28 (bedrock column) + 4 (convert) + 13 (prune);
  align.py 0 misalignments; golden worlds untouched — every
  fixture is copied before it is opened, because a LevelDB
  open compacts what it touches.

### Known gaps

- Actor records are not chunk-shaped — they carry an actor id
  where a subchunk key carries an index — so they survive the
  prune; naming actor keys is the entity feature's work (D13).
- Pruned space is reclaimed only when `db compact` or
  `region compact` runs after it; until then a Bedrock store
  only grows its log and a region file keeps its slack.
- No transaction across Bedrock batches or across region
  files: a failure part-way leaves whole batches and earlier
  files landed, the same honesty fill keeps.
- The Bedrock half scans the whole keyspace (O(entries))
  rather than synthesising keys, because only the scan sees
  every companion record a chunk owns.

### Next

- The convert report's two loudest drops — block entities
  and the waterlogging layer — are still the next phase of
  that feature; prune shipped first as asked.

---

## 2026-10-13 — Convert: `mcme convert`, a Bedrock world rewritten as Java 1.20

### Built

- `mcme convert <in> <out> --to java/1.20` runs end to end: a
  Bedrock world becomes a Java world — level.dat, region files,
  and every block the store holds carried through the translate
  map. One pair converts today. `--to` naming another pair is
  not-implemented, `--to` naming no edition is usage, a Java
  input is not-implemented, and `<out>` must not exist: convert
  writes a new world and never mixes into an old one, and any
  failure after the first byte removes the output whole (D12).
- The run scans the store's keys, groups them by chunk, and
  builds each chunk in its own arena: subchunk palettes decode,
  the map answers per entry, and `make_section` writes a Java
  section — the store's index order remapped to the position
  order the Java packing uses, the palette deduplicated through
  SNBT keys and seeded with air, the data packed at the
  format's own widths (4 bits as the floor, the palette's need
  above it, floor(64 / bits) entries to a long, none spanning)
  — the reader `java_chunk.c` already spoke. Each finished
  chunk goes back through `mcme_region_set_chunk`, the append
  path the region verbs made.
- `src/translate/translate.c` is the map. An entry without a
  `version` has no Java spelling and is refused to air,
  counted as `legacy_subchunks`; `sand` with `sand_type:red`
  is a name on the Java side; `pillar_axis` becomes `axis`
  for the log, wood, stem and hyphae names that spell it that
  way; redstone wire answers every input alike — four side
  connections and power 0 — the way the reference answers it;
  `grass_block.snowy` and `furnace.lit` are carried and
  everything else takes the Java default, which is the
  reference's own answer for the same input with empty states.
- The level.dat framing moved out of `read_level_dat` into a
  public `mcme_level_dat_parse()` (`world.c`/`world.h`) —
  gzip, 8-byte header plus plain NBT, header plus gzip, bare
  NBT — so convert reads the input's Bedrock framing and
  writes the output's gzip big-endian one through the same
  door. The Java level.dat is built as a 1.20 world records
  it: `WorldGenSettings` carrying the seed, the name, the
  spawn under `SpawnX`/`SpawnY`/`SpawnZ` — the names the
  probe reads — the game type, and the difficulty, each from
  the input's scope, root or `Data`, the two places Bedrock
  keeps them.
- The report is the promise: `McmeConvertReport` and the
  `--json` shape count the chunks, subchunks and blocks
  written and every drop — block entities, entities, the
  store's chunk-state keys, the legacy subchunks, the
  waterlogged layers, the keys outside the overworld. A drop
  is counted, never a failure: a world full of chests
  converts, and the report says what it left behind.
- Two of the write path's bugs were found before the commit
  by reading the output the way the reference would: four
  NBT key lengths wrong (`block_states` truncated to
  `block_state`, `biomes`, `block_entities` and
  `WorldGenSettings` written with a trailing NUL — every put
  takes an explicit length, so a wrong number is a wrong
  key), and the spawn written as `spawnX` where `world.c`
  reads `SpawnX`, invisible to every Java reader.

### Cross-check

- Selftest `test_translate`: the map, SNBT in and SNBT out —
  every rule one case, and the refusals (a legacy entry, a
  nameless entry, an entry that is not a compound) answered
  as refusals.
- Selftest `test_convert`: the library against a fixture
  fabricated in a temporary directory — the bedrock-chunks
  store copied, the bedrock fixture's level.dat carried
  onto it, neither golden touched: the report's counts
  exact (2 chunks, 7 subchunks, 24576 blocks, 1 legacy, 1
  water layer, 1 other), the output probing as java at
  DataVersion 3463 with both region files, the blocks read
  back through the region layer — `grass_block` with its
  `snowy`, `oak_log` with `axis:x`, `furnace` with `lit`
  and its facing gone, `red_sand`, the wire, `water`, the
  legacy subchunk's air, the fixture block keeping its name
  and losing its states — the level.dat carrying LevelName,
  the seed, the spawn and DataVersion, and the refusal
  classes (an output that exists is usage, a path that is
  not a world is not-there, the pair checks in order so the
  first refusal stands).
- `tools/ref_convert.py`, 4 checks: the `--json` report
  exact; **every one of the 24576 positions — every block of
  every stored subchunk — read back from the output region
  files with no mcme code (ref_java_fill's own sector walk
  and palette unpacking) and compared against PyMCTranslate's
  own bedrock(1, 20, 80) → universal → java(1, 20, 0)
  answers**, under the documented normalizations: booleans
  compare true/1 and false/0 alike, a property only the
  reference has equals mcme's absence when it is the Java
  default (computed by translating the same block with empty
  states) or zero, and a block neither edition knows keeps
  its name and loses its states where the reference passes
  them through; level.dat parity (LevelName, the seed under
  `WorldGenSettings`, the spawn under `SpawnX`/`Y`/`Z`,
  DataVersion 3463); and the exit codes — `--to banana` 1,
  `--to java/1.21` 5, an output that exists 1 with nothing
  written, a missing world 2, a Java input 5.
- Verified at commit: `./rebuild.sh test` PASS — 5 suites,
  0 failures; `./rebuild.sh release` builds; ref tools 8
  (java fill) + 8 (java blocks) + 12 (compact) + 9 (write)
  + 17 (chunk) + 14 (inventory) + 25 (entity) + 19 (bedrock
  write) + 28 (bedrock column) + 4 (convert); align.py 0
  misalignments; golden worlds untouched — the store is
  copied before it is opened, because a LevelDB open
  compacts what it touches.

### Known gaps

- Entities and block entities are counted, not carried: the
  input's chests arrive empty and the report says so
  (`block_entities`, `entities`).
- Ticks — block and fluid — are not carried; a converted
  world starts quiet.
- The waterlogged layer is dropped and counted
  (`water_layers`): a logged block keeps the block, loses
  the water above it.
- Biomes are written as plains for every section; the
  column's biome data is not read yet.
- `Heightmaps` are omitted from the written chunks.
- Legacy v1 palettes become air, counted as
  `legacy_subchunks` — a version 1 entry has no states to
  translate (D12).
- The overworld only: keys carrying another dimension are
  counted as `other` and dropped.

### Next

- Block entities and the waterlogged layer are the two
  drops the report counts loudest; carrying them is the
  next phase of the same feature.

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
