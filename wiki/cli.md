# `mcme` command-line contract

This is a **stable interface**. Scripts and agents depend on it. Add to it; do
not change existing meanings. If you must break it, bump the major version and
note it in [dev-log.md](dev-log.md).

## Invocation

```
mcme [global options] <command> [command options] [args]
```

## Global options

| Option | Effect |
|---|---|
| `-h`, `--help` | Print help for the given command, exit `MCME_OK` |
| `-V`, `--version` | Print version, exit `MCME_OK` |
| `--json` | Machine-readable output on stdout. Diagnostics stay on stderr. |
| `-q`, `--quiet` | Suppress non-essential output on stderr |
| `-v`, `--verbose` | `MCME_LOG_DEBUG` on stderr. Repeatable is not supported; one level is enough. |

`--json` is the important one: **when set, stdout is exactly one JSON value and
nothing else.** No progress bars, no banner, no trailing prose.

## Exit codes

Declared in `include/mcme/cli.h` as `McmeExit`. **Never renumber.**

| Code | Name | Meaning |
|---:|---|---|
| 0 | `MCME_OK` | Success |
| 1 | `MCME_EXIT_USAGE` | Bad command line: unknown command, missing arg |
| 2 | `MCME_EXIT_NOT_FOUND` | Path, world, or key does not exist |
| 3 | `MCME_EXIT_BAD_FORMAT` | Path exists but is not the expected format |
| 4 | `MCME_EXIT_IO` | Read/write failed (permissions, disk full) |
| 5 | `MCME_EXIT_NOT_IMPLEMENTED` | Recognised but not built yet |
| 6 | `MCME_EXIT_INTERNAL` | Should not happen; a bug |

**Rule for agents:** an exit code of `MCME_EXIT_NOT_IMPLEMENTED` (5) is not a
failure to report — it is a known gap. Check it against
[porting/parity.md](porting/parity.md) rather than treating it as an error.

## Output discipline

- **Human mode:** prose on stdout, progress and warnings on stderr.
- **JSON mode:** one JSON value on stdout, diagnostics on stderr.
- Errors go to stderr in both modes. In JSON mode an error is *not* printed as
  JSON — the exit code carries it. This keeps stdout parseable even when the
  command fails halfway.

## Commands

### `mcme selftest`

Runs the built-in self-tests for every subsystem compiled in.

```
mcme selftest              # human summary
mcme selftest --json       # {"subsystems":[{"name":"core","passed":3,"failed":0}],...}
```

Exit `0` if all pass, `MCME_EXIT_INTERNAL` if any fail.

### `mcme lang`

Works on the `.lang` translation files shipped in `assets/lang/`. The format is
the original project's, unmodified.

```
mcme lang list                       # ids, one per line
mcme lang list --json                # {"languages":["en","en_GB",...]}
mcme lang get <key> [--lang <id>]    # resolved string, falls back to en then the key
mcme lang get <key> --lang de --json # {"key":"...","lang":"de","value":"..."}
```

`lang get` exits `MCME_EXIT_NOT_FOUND` if the key is absent in every language.
By default it falls back to returning the key itself with exit `0`, because
that is what the UI does.

### `mcme config`

Works on the user config, which is **JSON** (not the original's pickle).

```
mcme config path                 # where the config file lives
mcme config get <dotted.key>
mcme config set <dotted.key> <json-value>
mcme config dump --json
```

### `mcme nbt dump`

Reads **raw (uncompressed) NBT** and prints it. Implemented in Phase 2.

```
mcme nbt dump <file>                  # SNBT, one line
mcme nbt dump <file> --json           # the same tree as JSON
mcme nbt dump <file> --le             # little-endian (Bedrock); default is big-endian
mcme nbt dump <file> --path a.b.0     # print only that subtree (dotted, lists indexed)
mcme nbt dump <file> --indent 2       # pretty-print
```

Exits `MCME_EXIT_BAD_FORMAT` if the bytes are not NBT, `MCME_EXIT_NOT_FOUND`
if `--path` does not resolve, `MCME_EXIT_USAGE` for a bad command line.

SNBT is the default because it is faithful: it keeps the type distinctions
(`4`, `4L`, `4f`) that JSON flattens away. `--json` exists so a tree can be fed
to `jq`, and trades type fidelity for a stable shape — note that infinity and
NaN become `null`, since JSON has no literal for them.

**Gzip is stripped here, not in the parser.** A file starting with `1f 8b` is
inflated before parsing. Compression is deliberately kept out of
`mcme_nbt_parse`: NBT has no framing of its own, and conflating the two would
make it impossible to read one NBT stream out of the middle of a file — which
is exactly what reading Bedrock chunk data requires. A corrupt gzip stream exits
`MCME_EXIT_BAD_FORMAT`.

Bedrock's `level.dat` is *not* gzip-framed the way Java's is: it carries an
8-byte header first. Use `mcme info` on the world directory rather than `nbt
dump` on that file.

### `mcme info`

Identifies a world directory and summarises it. Read-only; never writes, never
opens a database. Implemented in Phase 3.

```
mcme info <world>            # human-readable summary
mcme --json info <world>     # the same facts as one JSON object
```

```
$ mcme info myworld
path:     myworld
format:   java
name:     World Created By Amulet
data:     DataVersion 3463
played:   1791643789
leveldat: gzip
regions:  2
chunks:   5
```

Exit codes:

| Code | Meaning |
|---|---|
| `0` | A world was identified |
| `2` | The path exists but is not a world |
| `4` | The path is not a readable directory |

`2` and `4` are distinguished on purpose so a script can tell "wrong directory"
from "no permission".

Fields vary by edition and only appear when present. Java reports
`data: DataVersion <n>`; Bedrock reports `version:` from its
`lastOpenedWithVersion` tuple, printed in full (`1.10.0.0.0`) rather than
truncated to a plausible-looking depth. `leveldat:` reports how the file was
framed, which is the most useful clue when a world is mis-detected.

For Bedrock, chunk inventory comes from the LevelDB store: `keys:` is every
live key, `chunks:` is the distinct (dimension, x, z) that any chunk-marker key
marks. Several markers can address one chunk, so the two numbers differ and
neither is derivable from the other. A store that cannot be opened prints
`database: present, unreadable: <reason>` rather than a chunk count of zero —
zero would say the world is empty, which is the one thing that is not known.

### `mcme chunks`

What chunks a world contains. Counts distinct chunk coordinates; for Bedrock a
chunk counts once no matter how many marker keys address it, and the count is
grouped by dimension because a store mixes dimensions in one flat keyspace.

```
mcme chunks <world>            # text summary, per-dimension breakdown
mcme chunks <world> --count    # one number and nothing else
mcme --json chunks <world>     # path, format, keys, chunks_by_dimension
```

```
$ mcme chunks mybedrock
format:   bedrock (leveldb)
keys:     23
chunks:   9
by dimension:
  overworld  5
  nether     2
  end        2
```

`--count` prints a bare number so `mcme chunks w --count | xargs ...` needs no
parsing. Under `--json` the number is a field instead; the two are not printed
together.

Exit codes: `0` counted, `1` usage, `2` not a world, `4` store present but
unreadable.

Chunk markers are the subchunk prefix in both spellings the game has used
(0x2c pre-1.18, 0x2f after) and the chunk-version record (0x76). Tags that
ride along with a chunk — 0x2d biome/heightmap — are not markers and are never
counted.

### `mcme chunk set`

Sets one block in a Bedrock world. The first editing verb (D3: anything worth
doing is a verb) and deliberately the smallest one: one coordinate, one block,
no undo.

```
mcme chunk set <world> <x> <y> <z> <block>
```

The block is a namespaced name, optionally with SNBT states in braces:

```
$ mcme chunk set mybedrock 12 64 -30 'minecraft:stone'
$ mcme chunk set mybedrock 12 64 -30 'minecraft:oak_log{pillar_axis:"x"}'
$ mcme chunk set mybedrock 12 64 -30 'minecraft:netherrack' --dimension nether
```

States must parse as one SNBT compound; the whole spec is otherwise unvalidated
— mcme has no block registry and stores the name as given, the way the
reference tools do. The version written for a new palette entry is 17694720
("1, 14, 0, 0"), amulet's default for a block that arrives without one.

`--dimension` names the dimension: `overworld`, `nether`, `end`, or a raw
number (the store's own dimension codes). Without it, the overworld. A zero
dimension is omitted from the key the way the game omits it, so naming the
overworld changes nothing on disk.

The subchunk must already exist. The edit refuses to create chunks, so an
absent subchunk is exit 2, not exit 4: the caller asked to edit what is there,
and what is not there is "not found". A no-op edit — the same block that is
already stored — is exit 0 and leaves the payload byte-identical.

The store is opened with the game's own LOCK for the write, so a world
Minecraft has open is refused, like `db put`.

Exit codes: `0` set, `1` usage (bad coordinate, bad block spec), `2` not a
world, not a Bedrock world, or the subchunk is not stored, `4` store error.
Success prints nothing.

### `mcme chunk get`

Prints one block of a Bedrock world as the same spec `chunk set` accepts, so
`set` followed by `get` prints what went in and an agent can verify its own
edit. The printed text can be passed straight back to `set`.

```
mcme chunk get <world> <x> <y> <z> [--dimension <name|number>]
```

```
$ mcme chunk get mybedrock 12 64 -30
minecraft:oak_log{pillar_axis: "x"}
```

The text is canonical — SNBT as mcme's printer emits it, with the printer's
spacing, which is not necessarily the spacing that was typed — so compare
against a `get` of the same block, not against hand-typed text. The palette
entry's version is not printed: the spec syntax has no version field, and the
version mcme writes for entries it adds is a constant.

Under `--json` the same text is a field, escaped, with the coordinates
echoed: `{"block":"minecraft:stone","x":12,"y":64,"z":-30}`.

Exit codes: `0` printed, `1` usage, `2` not a world, not a Bedrock world, or
the subchunk is not stored, `3` the stored payload is not the format mcme
reads, `4` store error. Reading does not take the LOCK — the game can hold
the world open.

### `mcme chunk fill`

Sets a box of blocks to one block — the whole box, in one command, one
payload write per subchunk the box touches.

```
mcme chunk fill <world> <x1> <y1> <z1> <x2> <y2> <z2> <block>
                                  [--dimension <name|number>]
```

```
$ mcme chunk fill mybedrock 12 64 -30 30 70 -12 'minecraft:stone'
```

The corners are given in any order; both are inside the box. Every subchunk
the box touches must already be stored: the fill checks first, under the same
LOCK it then edits with, so a missing subchunk refuses the whole fill — exit 2,
nothing written — instead of filling around a hole. Positions the box only
reaches into keep what they held, and the subchunks' fluid layers are never
touched.

The cost is the subchunks touched, not the blocks in the box; a box entirely
inside one subchunk is one payload write. A subchunk that exists but cannot be
edited (a payload mcme cannot read) fails the fill with the subchunks before
it already written — there is no transaction across puts — which is why the
check comes first.

Exit codes: `0` filled, `1` usage (bad coordinate, bad block spec, a Y the
format cannot address), `2` not a world, not a Bedrock world, or a subchunk
the box touches is not stored, `4` store error. Success prints nothing.

### `mcme db`

Raw key/value access to a Bedrock store. This is the write path's CLI surface
(D3: anything worth doing is a verb) and it is deliberately raw — Bedrock keys
carry binary chunk coordinates, so keys and values are hex on the command
line.

```
mcme db get <world> <key-hex>             # print value-hex, or exit 2
mcme db put <world> <key-hex> <value-hex>
mcme db del <world> <key-hex>
mcme db compact <world>                   # flush the log into one new table
```

```
$ mcme db get mybedrock 656467652f73696e676c65
6e6577657220696e20746865206c6f67
$ mcme db del mybedrock 656467652f73696e676c65 ; echo $?
0
```

`get` opens read-only and does **not** take the store's LOCK, so it works on a
world the game has open. `put`, `del` and `compact` open writable, take an
exclusive `flock` on `LOCK` (the same lock Minecraft takes), and refuse with
exit 4 if the store is locked. Writes append to the store's log as one
WriteBatch record each; past 4 MiB of log the next write flushes everything
into a new SSTable first (compaction, see
[decisions.md](decisions.md)#D11), so a long session does not depend on
anyone remembering to run `compact`. `compact` is that flush as a manual verb:
the same view, one table, no logs. A write past 32 MiB with compaction failing
(disk full, permissions) refuses rather than growing a store only that session
could read.

Exit codes: `0` ok, `1` usage, `2` key absent (for `get`), `4` store locked or
unreadable.

### Reserved, not yet implemented

Parses and then exits `MCME_EXIT_NOT_IMPLEMENTED`. It exists so the surface is
settled early and tests can assert the gap.

```
mcme convert <in> <out> --to <platform>/<version>   # Phase 4
```

## Writing agent-friendly commands

When adding a command, do all four:

1. Give it an `--json` shape and document it here before implementing.
2. Pick the narrowest correct exit code from the table.
3. Make it work on a path argument, not a GUI dialog.
4. Add a case to `tests/unit/` that runs it and asserts the exit code.
