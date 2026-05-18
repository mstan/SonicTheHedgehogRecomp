# CLAUDE.md — SonicTheHedgehogRecomp

This is the Sonic 1 release repo. The canonical session brief lives in the
shared submodule:

→ **`segagenesisrecomp/CLAUDE.md`** — read this first.
→ **`segagenesisrecomp/PRINCIPLES.md`** — the 25 rules.
→ **`segagenesisrecomp/DEBUG.md`** — always-on ring inventory + TCP commands.

## What's in this repo

- `runner/` — the shared engine (consumed by both Sonic 1 and Sonic 2
  builds). Contains `glue.c`, `cmd_server.c`, `oracle_trace.c`,
  `frame_snapshots.c`, `reverse_debug.c`, `crash_report.c`, `audio.c`,
  `main.c`, `hybrid.c`, etc., plus `sonic1_spec.c` / `sonic1_hybrid_table.c`
  / `sonic2_hybrid_table.c` (the latter two will relocate into the submodule
  in Wave 4).
- `tools/` — Sonic-1-flavored probes (60+ Python scripts).
- `segagenesisrecomp/` — submodule (the shared engine + recompiler + this
  repo's canonical docs).
- `CMakeLists.txt` — Sonic 1 build config.
- `_build_native.bat`, `_build_oracle.bat` — build wrappers.

## Asymmetric placement note

The shared runner currently lives here, in Sonic 1's release repo. Sonic 2
references it via `../SonicTheHedgehogRecomp/runner/`. This is a
known structural smell that Wave 4 of the active improvement plan will
fix by promoting `runner/` into the submodule.

Until then: when editing files under `runner/`, you are editing shared
engine code that affects Sonic 2 as well as Sonic 1.

## Submodule commit order (PRINCIPLES.md #20)

1. Commit `segagenesisrecomp/` (the submodule) first.
2. Bump the submodule pointer in this repo second.
3. Sonic 2's release repo bumps independently.
