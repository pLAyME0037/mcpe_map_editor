# mcme — Minecraft Map Editor

A from-scratch **C11** Minecraft world editor and converter. A rewrite of the
Amulet Map Editor's *behaviour*, not its code.

- **CLI-first.** Everything is reachable from `mcme <verb>` so it is testable by
  scripts and agents. The GUI is a client of that CLI, not the other way round.
- **Simplexity.** The world formats are irreducibly complex; the code refuses to
  add more. See [wiki/simplexity.md](wiki/simplexity.md).
- **Process lives in the repo.** [wiki/](wiki/) holds the dev log, the decision
  record, and the parity checklist against the original.

## Status

Early but reading. Core primitives, the CLI contract, NBT, Anvil regions and
the Bedrock LevelDB store all work, each byte-checked against the Python
reference. See [wiki/dev-log.md](wiki/dev-log.md) for what is actually done
and what is next. Writing exists only for Bedrock — log-append plus compaction
to tables (`mcme db`); Java formats are not implemented.

## Build

```
./rebuild.sh          # bootstrap nob, build ./mcme (debug, ASan+UBSan on)
./rebuild.sh release  # optimized build
./nob test            # build and run the unit tests
./mcme selftest       # run self-tests inside the binary
```

Requires: a C11 compiler, `git`, and (for the GUI phase only) GTK4 dev files.

## Layout

```
build.c           build script (nob.h)
rebuild.sh        bootstraps the build tool
include/mcme/     public headers, one per subsystem
src/core/         arena, log, fs, event queue, thread pool, lang, config
src/nbt/          NBT + SNBT                    (Phase 2)
src/world/        chunk/section model, anvil/, leveldb/, schematic/  (Phase 3)
src/translate/    version + block translation   (Phase 4)
src/resource/     resource packs, models, atlas (Phase 5)
src/render/       GL renderer, chunk mesher     (Phase 6)
src/app/          CLI entry point + app shell   (Phase 7)
src/ui/           GTK4 widgets                  (Phase 8-10)
src/edit/         edit canvas, behaviours, tools, operations
src/convert/      world-to-world conversion
assets/           shaders, lang files, icons
tests/            unit/ golden/ fuzz/
tools/            reference-dump scripts that drive the Python build
third_party/      vendored single-file C libraries
wiki/             the project's memory
```

## Licence

Personal study project. The original Amulet Map Editor is proprietary; this is
an independent reimplementation that reads the same file formats. Commercial
code from the original (licence nag, update check, sponsor blocks) is
deliberately not ported.

Vendored libraries keep their own permissive licences (public domain / MIT /
BSD-2) in `third_party/`.
