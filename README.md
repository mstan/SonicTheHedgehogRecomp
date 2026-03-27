# Sonic the Hedgehog — Recompiled Runner

> A native x64 static recompilation of Sonic the Hedgehog (Sega Genesis/Mega Drive). Green Hill Zone is fully playable — all 3 acts including the Robotnik boss fight.

[![Gameplay Demo](https://img.youtube.com/vi/tMkbDKK5y38/maxresdefault.jpg)](https://youtu.be/tMkbDKK5y38)

The game runner for the [Genesis 68K Static Recompiler](https://github.com/mstan/segagenesisrecomp). Takes 530+ statically recompiled C functions generated from a Sonic 1 ROM and runs them natively inside [clownmdemu](https://github.com/Clownacy/clownmdemu), an open-source Mega Drive emulator core.

## Status

Green Hill Zone (Acts 1–3) is fully completable. Later zones are partially functional with ongoing function discovery.

| Feature | Status | Notes |
|---------|--------|-------|
| Rendering (VDP, sprites, tilemaps, scroll) | ✅ Works | Sprite art, flowers, animated tiles all correct |
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

This runner hosts that generated code inside clownmdemu, which provides VDP rendering (graphics), Z80/FM/PSG (audio), and I/O (controllers). The generated 68K code runs on a Windows Fiber that interleaves with VDP scanline rendering via a cooperative yield model.

A TCP debug server (port 4378) provides live game state inspection, time-series Sonic state queries, VRAM inspection, and scriptable input injection for automated testing.

## Prerequisites

- Visual Studio 2022 (MSVC)
- CMake 3.16+
- SDL2 2.28+ (bundled)
- A Sonic the Hedgehog (Genesis) ROM file (.bin/.md/.gen/.smd)

## Build and Run

```bash
git clone --recursive https://github.com/mstan/SonicTheHedgehogRecomp.git
cd SonicTheHedgehogRecomp
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 -DENABLE_RECOMPILED_CODE=ON
cmake --build build --config Release

# Launch — file picker appears if no ROM argument given
build\Release\SonicTheHedgehogRecomp.exe
# Or specify ROM directly:
build\Release\SonicTheHedgehogRecomp.exe path\to\sonic.bin
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

## Debug Tools

A TCP debug server listens on port 4378 while the game runs. Use `tools/dbg.py` for CLI access:

```bash
python tools/dbg.py ping              # Check connection
python tools/dbg.py sonic_state       # Sonic's position, velocity, status
python tools/dbg.py sonic_history 100 200  # Time-series state
python tools/dbg.py object_table      # Active objects
python tools/dbg.py read_memory 0xFF0000 32  # Memory inspection
```

## Adding New Functions

When the game hits an uncompiled function, it logs `dispatch miss: $XXXXXX` to the console. To compile it:

1. Play the game (or use the interpreter build for full coverage)
2. Run `python tools/diff_coverage.py` to find missing functions
3. Add safe entries to `game.cfg` (check `blacklist.txt` for known-bad interior labels)
4. Regenerate with the recompiler, rebuild

## License

[PolyForm Noncommercial 1.0.0](LICENSE.md) — free for non-commercial use. See [LICENSE.md](LICENSE.md) for details.

ROM files are NOT included — you must provide your own legally obtained copy.
