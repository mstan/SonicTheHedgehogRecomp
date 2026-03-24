# Sonic 1 Recompiled Runner

## RULE 0: Never Modify Generated Output

**sonic_full.c and sonic_dispatch.c are READ-ONLY.**

- Codegen bugs -> fix in the recompiler (`../segagenesisrecomp/genesisrecomp/recompiler/src/`)
- Game-specific patterns -> game.cfg or runtime workaround in glue.c
- If you think you need to edit generated output, STOP. Find the right layer.

## Build

```bash
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
build/Release/SonicTheHedgehogRecomp.exe sonic.bin
```

Requires: MSVC 2022, CMake 3.16+, SDL2 (bundled), a Sonic 1 ROM named `sonic.bin`.

## Architecture

Single-threaded Windows Fiber model:

1. `main.c` — SDL2 host. 60 Hz loop: reset sync -> run game frame -> Iterate -> service VBlank.
2. `glue.c` — Fiber bridge. Routes m68k_read/write to clownmdemu bus. Contains 6 runtime workarounds (documented in file header and STATUS.md).
3. `stub_clown68000.c` — Replaces clown68000 interpreter. DoCycles dispatches to game fiber for scanline interleaving.
4. `audio.c` — FM/PSG mixing to SDL ring buffer.

Game code runs on a game fiber. ClownMDEmu_Iterate() calls DoCycles ~500 times (once per scanline sync), which switches to the game fiber. The game fiber yields back when it hits WaitForVBlank (func_0029A8).

## Key Paths

| What | Path |
|------|------|
| Recompiler source | `../segagenesisrecomp/genesisrecomp/recompiler/src/` |
| Game config | `../segagenesisrecomp/sonicthehedgehog/game.cfg` |
| Generated output | `../segagenesisrecomp/sonicthehedgehog/generated/` |
| Emulator core | `../segagenesisrecomp/clownmdemu-core/` |

## Known Bugs and History

See [STATUS.md](STATUS.md) for known bugs, runtime workarounds, development history, and debug methodology.
