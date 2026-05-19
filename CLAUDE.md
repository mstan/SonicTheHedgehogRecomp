# CLAUDE.md — SonicTheHedgehogRecomp

This is the Sonic 1 release repo. The canonical session brief lives in the
shared submodule:

→ **`segagenesisrecomp/CLAUDE.md`** — read this first.
→ **`segagenesisrecomp/PRINCIPLES.md`** — the 25 rules.
→ **`segagenesisrecomp/DEBUG.md`** — always-on ring inventory + TCP commands.

## What's in this repo

- `tools/` — Sonic-1-flavored probes (60+ Python scripts).
- `segagenesisrecomp/` — submodule. Holds the shared runner (`runner/`),
  recompiler, canonical docs, and per-game generated/spec code for both
  games.
- `CMakeLists.txt` — Sonic 1 build config; sources the runner from
  `segagenesisrecomp/runner/`, per-game files from
  `segagenesisrecomp/sonicthehedgehog/`.
- `_build_native.bat`, `_build_oracle.bat` — build wrappers.

## Editing shared code

The shared runner lives in `segagenesisrecomp/runner/`. When editing it,
you affect both Sonic 1 and Sonic 2 builds. Submodule commit order
applies (see below).

## Submodule commit order (PRINCIPLES.md #20)

1. Commit `segagenesisrecomp/` (the submodule) first.
2. Bump the submodule pointer in this repo second.
3. Sonic 2's release repo bumps independently.
