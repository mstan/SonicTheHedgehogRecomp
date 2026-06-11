# Sonic the Hedgehog — Recompiled Runner

> A native x64 static recompilation of Sonic the Hedgehog (Sega Genesis/Mega Drive). Green Hill Zone is fully playable — all 3 acts including the Robotnik boss fight.

[![Gameplay Demo](https://img.youtube.com/vi/IINTsq1JBg8/maxresdefault.jpg)](https://youtu.be/IINTsq1JBg8)

The game runner for the [Genesis 68K Static Recompiler](https://github.com/mstan/segagenesisrecomp). Takes 530+ statically recompiled C functions generated from a Sonic 1 ROM and runs them natively on the engine's clean-room backend (own VDP / bus / Z80 scheduling, ymfm FM synthesis). The AGPL [clownmdemu](https://github.com/Clownacy/clownmdemu) core is used only by unshipped development builds as a conformance oracle.

## Status

Green Hill Zone (Acts 1–3) is fully completable. Later zones are partially functional with ongoing function discovery.

| Feature | Status | Notes |
|---------|--------|-------|
| Rendering (VDP, sprites, tilemaps, scroll) | ✅ Works | Sprite art, tilemaps, animated tiles all correct |
| SEGA logo + voice sample | ✅ Works | |
| Title screen / menus | ✅ Works | |
| Boot sequence transitions | ✅ Works | SEGA → title → level load all functional |
| Sonic movement (run, roll, slopes, loops) | ✅ Works | |
| **Jumping** | ✅ **Fixed** | `addq.l #4,sp` + `rts` pattern now handled by recompiler |
| Ring pickups | ✅ Works | |
| Enemies, item monitors, springs | ✅ Works | |
| Bridges, spike helixes, swinging platforms | ✅ Works | Interior label splits identified and fixed |
| HUD (score, lives, rings, timer) | ✅ Works | |
| Palette fades | ✅ Works | |
| **Audio** | ✅ **Mostly fixed** | Music and SFX play correctly. Minor timing drift over long sessions |
| Function dispatch | ✅ 530+ functions | Zero dispatch misses on GHZ. Later zones may have undiscovered functions |
| **Green Hill Zone (all 3 acts + boss)** | ✅ **Complete** | First zone fully playable end to end |
| Later zones | ⚠️ Partial | Functions discovered via interpreter coverage logging. Some objects missing |
| Save states | ✅ Works | 9 slots (Shift+F1-F9 save, F1-F9 load) |

## How It Works

The [recompiler](https://github.com/mstan/segagenesisrecomp) analyzes a Sonic 1 ROM binary and emits native C functions for every 68K subroutine — 530+ functions total. These generated functions use the same memory layout and register state as the original 68K code, but execute as compiled x64 instead of interpreted instructions.

This runner hosts that generated code on the engine's clean-room backend, which provides VDP rendering (graphics), Z80/FM/PSG (audio — ymfm + superzazu z80), and I/O (controllers). The generated 68K code runs on a cooperative fiber that interleaves with VDP scanline rendering via a yield model — backed by Win32 Fibers on Windows and by `ucontext` on macOS/Linux (see `segagenesisrecomp/runner/fiber_compat.{h,c}`).

The runner builds and runs natively on Windows (MSVC), macOS (Apple Silicon & Intel), and Linux. SDL2 provides windowing, rendering, audio, and `SDL_GameController` gamepad support on every platform.

A TCP debug server (port 4378) provides live game state inspection, time-series Sonic state queries, VRAM inspection, and scriptable input injection for automated testing.

## Prerequisites

- A C compiler:
  - **Windows:** Visual Studio 2022 (MSVC)
  - **macOS:** Apple Clang (Xcode Command Line Tools)
  - **Linux:** Clang or GCC
- CMake 3.16+ (plus [Ninja](https://ninja-build.org/) on macOS/Linux)
- SDL2 2.28+ — bundled on Windows; `brew install sdl2` on macOS; `libsdl2-dev` (or distro equivalent) on Linux
- A Sonic the Hedgehog (Genesis) ROM file (.bin/.md/.gen/.smd)

## Build and Run

> **No prebuilt binaries are distributed — build from source below and supply your own ROM.**

### Engine checkout (one-time, shared by all game projects)

The recompiler engine ([segagenesisrecomp](https://github.com/mstan/segagenesisrecomp))
lives in a single canonical checkout at the workspace root, shared by every
game project (Sonic 1/2/3/…). Clone it once next to this repo, then link it in.
The link is gitignored, so no project "owns" the engine:

```bash
# Layout:  <workspace>/segagenesisrecomp        (shared engine, recursive clone)
#          <workspace>/SonicTheHedgehogRecomp
git clone --recursive https://github.com/mstan/segagenesisrecomp.git
git clone https://github.com/mstan/SonicTheHedgehogRecomp.git
cd SonicTheHedgehogRecomp
scripts/link-engine.sh        # macOS/Linux — creates a symlink
scripts\link-engine.bat       # Windows     — creates a directory junction (mklink /J)
```

### Native Build (recompiled code drives the game)

**Windows (MSVC):**

```bat
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 -DENABLE_RECOMPILED_CODE=ON
cmake --build build --config Release

# Launch — file picker appears if no ROM argument given
build\Release\SonicTheHedgehogRecomp.exe
build\Release\SonicTheHedgehogRecomp.exe path\to\sonic.bin
```

**macOS / Linux (Ninja + Clang/GCC):**

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
ninja -C build SonicTheHedgehogRecomp

# Launch — a ROM path is required (no file picker on macOS/Linux)
./build/SonicTheHedgehogRecomp "path/to/Sonic the Hedgehog.bin"
```

### Interpreter Build (for function discovery)

The interpreter build runs the full clown68000 68K interpreter alongside the generated code. It provides coverage logging to discover which ROM addresses are executed — essential for finding functions that the native build hasn't compiled yet.

```bash
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 ^
    -DHYBRID_RECOMPILED_CODE=ON ^
    -DENABLE_RECOMPILED_CODE=OFF ^
    -DCLOWNMDEMU_SKIP_CLOWN68000_INTERPRETER=OFF
cmake --build build --config Release
```

## Controls

| Key | Action |
|-----|--------|
| Arrow keys | D-pad |
| Z | A button (jump) |
| X | B button |
| C | C button |
| Enter | Start |
| Tab (hold) | Turbo mode |
| Shift+F1-F9 | Save state |
| F1-F9 | Load state |
| Escape | Quit |

## Discovering and Adding New Functions

When the native build encounters a function that wasn't statically compiled, it logs a **dispatch miss** to `dispatch_misses.log` and to stderr.

### From Native Build (dispatch misses)

Play the native build and explore areas where you see missing behavior. Dispatch misses are logged automatically.

```bash
# After playing, check what was missed:
cat dispatch_misses.log
```

### From Interpreter Build (full coverage)

The interpreter tracks every PC address the 68K executes. Play through any area and the interpreter logs every code path.

1. Build and run the interpreter build (see above)
2. Play through the target area
3. Dump coverage via TCP: `python tools/tcp_cmd.py '{"id":1,"cmd":"coverage_dump"}'`
4. Diff against the dispatch table: `python tools/diff_coverage.py`
5. Add new entries to `game.cfg`, checking each against `blacklist.txt`
6. Audit for bad splits: `python tools/audit_all_splits.py`
7. Regenerate with the recompiler, rebuild

### The Blacklist

Some ROM addresses look like function entry points but are actually **interior labels** that split parent functions in half. The `blacklist.txt` file lists all known interior labels. The recompiler's `blacklist_file` directive in `game.cfg` enforces this at compile time.

## Debug Tools

A TCP debug server listens on port 4378 while the game runs. Use `tools/dbg.py` for CLI access:

```bash
python tools/dbg.py ping              # Check connection
python tools/dbg.py sonic_state       # Sonic's position, velocity, status
python tools/dbg.py sonic_history 100 200  # Time-series state
python tools/dbg.py object_table      # Active objects
python tools/dbg.py read_memory 0xFF0000 32  # Memory inspection
```

## License

[PolyForm Noncommercial 1.0.0](LICENSE.md) — free for non-commercial use. See [LICENSE.md](LICENSE.md) for details.

ROM files are NOT included — you must provide your own legally obtained copy.
