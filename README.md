# Sonic the Hedgehog — Recompiled Runner

> **WARNING: This is an early prototype / work in progress.** It is not production-ready, not feature-complete, and has significant known bugs. The game boots and runs, but core mechanics like jumping are broken, audio is garbled, and 6 runtime hacks are required to prevent crashes. This repo is published for educational and archival purposes — expect rough edges everywhere.

The game runner for the [Genesis 68K Static Recompiler](../segagenesisrecomp/). Takes 337 statically recompiled C functions generated from a Sonic 1 ROM and runs them natively inside [clownmdemu](https://github.com/Clownacy/clownmdemu), an open-source Mega Drive emulator core.

## Status

This is a proof-of-concept prototype. It demonstrates that static recompilation of Genesis games is viable, but it is far from a polished or correct implementation.

| Feature | Status | Notes |
|---------|--------|-------|
| Rendering (VDP, sprites, tilemaps, scroll planes) | Works | |
| SEGA logo + voice sample | Works | |
| Title screen / menus | Works | |
| All zones (GHZ and others via attract demo) | Works | |
| Sonic movement (run, roll, slopes, loops, springs) | Works | |
| Ring pickups | Works | |
| Enemies, item monitors | Works | |
| HUD (score, lives, rings, timer) | Works | |
| Palette fades | Works | Was broken, fixed via register save/restore |
| Function dispatch | Works | 337 discovered functions, zero misses so far. Undiscovered functions likely exist on untested code paths |
| **Jumping** | **Broken** | No height — joypad timing offset causes yvel to be overwritten to 0 |
| **Scattered rings (damage)** | **Broken** | Rings scatter visually when Sonic takes damage, but can't be picked back up |
| **Audio** | **Partial** | "SEGA!" sample plays, music/SFX faintly audible but garbled. Z80/FM don't advance during game code |
| **Some sprite art** | **Broken** | Flower tiles use wrong art (VRAM timing issue) |
| **Boot sequence transitions** | **Broken** | SEGA -> Sonic Team Presents -> title -> level transitions too fast. In-game transitions work fine |

The runner contains 6 runtime workarounds for timing and state management issues that don't exist in a real interpreter. See [STATUS.md](STATUS.md) for the full breakdown.

## How It Works

The [recompiler](../segagenesisrecomp/) analyzes a Sonic 1 ROM binary and emits native C functions for every 68K subroutine — 337 functions total. These generated functions use the same memory layout and register state as the original 68K code, but execute as compiled x64 instead of interpreted instructions.

This runner hosts that generated code inside clownmdemu, which provides VDP rendering (graphics), Z80/FM/PSG (audio), and I/O (controllers). The generated 68K code runs on a Windows Fiber that interleaves with VDP scanline rendering via a cooperative yield model.

## Prerequisites

- Visual Studio 2022 (MSVC)
- CMake 3.16+
- SDL2 2.28+ (bundled)
- A Sonic the Hedgehog (Genesis) ROM file

## Build and Run

```bash
# Clone both repos side by side:
git clone --recursive <segagenesisrecomp-url>
git clone <SonicTheHedgehogRecomp-url>

cd SonicTheHedgehogRecomp
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release

# Place your ROM in the project directory
build\Release\SonicTheHedgehogRecomp.exe sonic.bin
```

## Controls

| Key | Action |
|-----|--------|
| Arrow keys | D-pad |
| Z | A button |
| X | B button |
| C | C button |
| Enter | Start |
| F5 | Toggle turbo mode |
| F12 | Screenshot |
| Escape | Quit |

## Known Issues

See [STATUS.md](STATUS.md) for the full list of known bugs, runtime workarounds, architecture limitations, and development history (including which investigation approaches worked and which were dead ends).

## License

[PolyForm Noncommercial 1.0.0](LICENSE.md) — free for non-commercial use. See [LICENSE.md](LICENSE.md) for details.
