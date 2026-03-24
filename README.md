# Sonic the Hedgehog — Recompiled Runner

The game runner for the [Genesis 68K Static Recompiler](../segagenesisrecomp/). Takes 337 statically recompiled C functions generated from a Sonic 1 ROM and runs them natively inside [clownmdemu](https://github.com/Clownacy/clownmdemu), an open-source Mega Drive emulator core. This is a **tech demo with known bugs** — the game boots to the title screen, loads Green Hill Zone, and gameplay runs, but jumping doesn't work and audio is garbled.

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
#   segagenesisrecomp-v3/segagenesisrecomp/
#   segagenesisrecomp-v3/SonicTheHedgehogRecomp/

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

See [STATUS.md](STATUS.md) for details on known bugs, runtime workarounds, and development history.
