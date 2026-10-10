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

For Bedrock, chunk inventory is **not** printed yet: the LevelDB reader is not
built, and a `chunks: 0` for a world that has chunks would be worse than saying
so.

### Reserved, not yet implemented

These parse and then exit `MCME_EXIT_NOT_IMPLEMENTED`. They exist so the surface
is settled early and tests can assert the gap.

```
mcme convert <in> <out> --to <platform>/<version>   # Phase 4
mcme chunks <world> --count                         # needs the LevelDB reader
```

## Writing agent-friendly commands

When adding a command, do all four:

1. Give it an `--json` shape and document it here before implementing.
2. Pick the narrowest correct exit code from the table.
3. Make it work on a path argument, not a GUI dialog.
4. Add a case to `tests/unit/` that runs it and asserts the exit code.
