# Sonic the Hedgehog — Recompiled Runner

> A native static recompilation of Sonic the Hedgehog (Sega Genesis/Mega Drive). Supply your own supported ROM.

[![Gameplay Demo](https://img.youtube.com/vi/IINTsq1JBg8/maxresdefault.jpg)](https://youtu.be/IINTsq1JBg8)

The game runner for the [Genesis 68K Static Recompiler](https://github.com/mstan/segagenesisrecomp). Original 68K game routines are generated as C and run on the engine's clean-room Genesis backend, with its VDP, bus, Z80 scheduling and ymfm audio synthesis.

## Status

The native game runner supports the original game routines, levels, bosses,
music and sound effects. Adaptive widescreen and high-refresh presentation are
optional enhancements with bounded validation coverage.

| Area | Behavior |
|------|----------|
| Main game | Uses the original game logic and level data. |
| Audio | YM2612 and PSG music and sound-effect synthesis. |
| Launcher | Audio-device and sample-rate selections persist between launches. |
| Experimental widescreen | Adaptive views with stage-specific scene composition. See the [widescreen guide](docs/EXPERIMENTAL_WIDESCREEN.md). |
| High refresh presentation | Optional presentation above the native simulation rate. |
| Save states | Build-specific snapshots; no cross-build compatibility promise. |

See the [release notes](docs/RELEASE_NOTES_1.0.0.md) for the current update.

Windows x64 builds default to the shared batched VDP renderer. The 6,000-frame
Green Hill measurement gained 20.5% uncapped FPS and used 10.6% less process CPU
time with rendering and audio synthesis retained. Focused renderer checks passed. This result
applies to Sonic 1 on Windows; it is not a performance claim for other games or
ports. Build with `-DGENESIS_VDP_PLANE_IMPL=SCALAR` for the LLE reference.

## Experimental adaptive widescreen

Enable **Mods → Widescreen** in the recomp-ui launcher or in-game Escape menu.
Choose **16:9**, **21:9**, **32:9 (experimental)**, or **Adaptive** and resize/maximize the window.
Fresh installs enable **Fit to screen** by default; saved aspect/mod choices are preserved. The mod uses a separate custom scene renderer, with an
edge-anchored HUD, expanded objects/rings, and wider title/special-stage scenery.
Adaptive has no 32:9 cap; `--widescreen stage` remains a full-stage-width overview
experiment for the command line. Settings persist next to the executable.
The jump-SFX stop/unwind correction is included regardless of renderer mode.
See [the experiment's modes, limits and validation](docs/EXPERIMENTAL_WIDESCREEN.md).

## High refresh presentation

Choose **Mods > High refresh rate > 120 Hz, 144 Hz, or Display** in the launcher
or Escape menu. Display follows the monitor refresh rate. The default is Off.
Movement and camera positions interpolate between completed gameplay frames;
gameplay, audio and native animation keep their original rates. Menus and
special stages retain completed images. The option works with native output
and adaptive widescreen.

The Audio menu also offers **Unfiltered audio**, disabled by default. It bypasses
the native low-pass filters; the separate enhanced FM path retains its own sound.

## How It Works

The [recompiler](https://github.com/mstan/segagenesisrecomp) analyzes a Sonic 1 ROM binary and emits native C functions for discovered 68K routines. These generated functions use the same memory layout and register state as the original 68K code, but execute as compiled x64 instead of interpreted instructions.

This runner hosts that generated code on the engine's clean-room backend, which provides VDP rendering (graphics), Z80/FM/PSG (audio — ymfm + superzazu z80), and I/O (controllers). The generated 68K code runs on a cooperative fiber that interleaves with VDP scanline rendering via a yield model — backed by Win32 Fibers on Windows and by `ucontext` on macOS/Linux (see `segagenesisrecomp/runner/fiber_compat.{h,c}`).

The runner builds and runs natively on Windows (MSVC), macOS (Apple Silicon & Intel), and Linux. SDL2 provides windowing, rendering, audio, and `SDL_GameController` gamepad support on every platform.

Development builds with `GEN_ENABLE_TRACE=ON` provide a TCP debug server (default port 4378) for game-state inspection and automated testing. Production releases disable this server.

## Prerequisites

- A C compiler:
  - **Windows:** Visual Studio 2022 (MSVC)
  - **macOS:** Apple Clang (Xcode Command Line Tools)
  - **Linux:** Clang or GCC
- CMake 3.16+ (plus [Ninja](https://ninja-build.org/) on macOS/Linux)
- SDL2 2.28+ — bundled on Windows; `brew install sdl2` on macOS; `libsdl2-dev` (or distro equivalent) on Linux
- A Sonic the Hedgehog (Genesis) ROM file (.bin/.md/.gen/.smd)

## Build and Run

Download Windows builds from [Releases](https://github.com/mstan/SonicTheHedgehogRecomp/releases), or build from source below. ROMs are not included. The verified target is Sonic 1 JUE REV00; REV01 is not currently a qualified target. An unverified ROM warning is not evidence that another revision works.

### Source checkout

```bash
git clone --recursive https://github.com/mstan/SonicTheHedgehogRecomp.git
cd SonicTheHedgehogRecomp
```

The committed engine and recomp-ui submodule revisions identify the source combination for the build. An existing engine checkout can be selected explicitly with `-DGENESIS_RECOMP_ROOT=/path/to/segagenesisrecomp`. Use the game's own `game/game.toml` and discovery data when regenerating.

### Native Build (recompiled code drives the game)

Before configuring, copy your ROM to
`game/sonic.bin`. Generated C is not checked in;
CMake builds the current recompiler and regenerates it from this ROM whenever
the ROM, config, discovery data, or recompiler changes.

**Windows (MSVC):**

```bat
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release

# Launch — pass the ROM path, or omit it to use the Windows file picker
build\Release\SonicTheHedgehogRecomp.exe path\to\sonic.bin
```

**macOS / Linux (Ninja + Clang/GCC):**

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
ninja -C build SonicTheHedgehogRecomp

# Launch — a ROM path is required (no file picker on macOS/Linux)
./build/SonicTheHedgehogRecomp "path/to/Sonic the Hedgehog.bin"
```

### Development builds

The supported game build uses generated routines with the clean-room Tier-3 interpreter as a fallback. To investigate a report, build with `-DGEN_ENABLE_TRACE=ON`; production builds use `-DGEN_ENABLE_TRACE=OFF -DGEN_DEV_TRACE=OFF -DSONIC_REVERSE_DEBUG=OFF`. Keep diagnostic output in an isolated test profile and retain the exact build and ROM identity with captures.

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
| Escape | In-game menu |

## Audio & video enhancements (opt-in)

The runner includes an optional **verified-enhancement shadow** layer — QoL
audio/video improvements that run alongside the authentic hardware emulation and
substitute only after continuously proving they still match it (reverting loudly
if they ever stop). **Off by default** (output is byte-identical to raw hardware
emulation); enable per-run via environment variables:

| Variable | Values | Effect |
|----------|--------|--------|
| `GENESIS_SCREEN` | `raw` (default), `crt`, `trinitron`, `composite`, `linear` | Present-time color model — maps the Genesis 9-bit gamut through a CRT/phosphor model (gamma + lifted black). `raw` is bit-identical passthrough. |
| `GENESIS_AUDIO_SHADOW` | `0` (default) / `1` | Arms the YM2612 FM shadow — a parallel `ymfm` chip with a relaxed output low-pass that keeps the cleaner, less-aliased highs. |
| `GENESIS_FM_LADDER` | unset (default) / `off` | With `off`, renders the FM shadow through ymfm's ladder-free `ym3438` (no YM2612 DAC crossover crunch). Needs `GENESIS_AUDIO_SHADOW=1`. |

```bash
GENESIS_AUDIO_SHADOW=1 GENESIS_FM_LADDER=off GENESIS_SCREEN=crt ./SonicTheHedgehogRecomp sonic.bin
```

Full design, verifier algorithm, and rationale: `segagenesisrecomp/docs/SHADOW_ENHANCEMENTS.md`.

## Function-discovery evidence

The native runner writes unresolved dispatch entries to
`dispatch_misses.toml`. The Tier-3 fallback similarly writes executed leads to
`floor_coverage.toml`. Both are valid TOML discovery snippets using
`[functions].extra`, but they are evidence rather than authority: verify every
address against the disassembly and fix the appropriate `gen_disasm_*`
extractor or checked-in discovery TOML before rebuilding. Never seed a runtime
address blindly; see `segagenesisrecomp/PRINCIPLES.md` §16.

Interior-label misses are kept separate in `interior_label_misses.log` because
they require a code-generation fix, not a new function entry.

## Debug Tools

With `GEN_ENABLE_TRACE=ON`, the TCP debug server listens on port 4378 by default (or the configured development port). It is absent from production releases. Use the engine checkout's `tools/dbg.py` for CLI access:

```bash
python tools/dbg.py ping              # Check connection
python tools/dbg.py sonic_state       # Sonic's position, velocity, status
python tools/dbg.py sonic_history 100 200  # Time-series state
python tools/dbg.py object_table      # Active objects
python tools/dbg.py read_memory 0xFF0000 32  # Memory inspection
```

## Contributors


## License

For this project's code covered by the [PolyForm Noncommercial License](LICENSE.md),
[PERMISSIONS.md](PERMISSIONS.md) grants standing permission for voluntary
donations, sponsorships, and advertising revenue, including support for broader
development initiatives. Payment must not buy access to the software, games
built with it, or their updates, features, or game content, including early access.

Matthew Stanley has authority from the relevant copyright holders to grant
these permissions for the project's PolyForm-covered code. Third-party
components and code explicitly licensed under other terms retain those terms.

[PolyForm Noncommercial 1.0.0](LICENSE.md) — free for non-commercial use. See [LICENSE.md](LICENSE.md) for details.

ROM files are NOT included — you must provide your own legally obtained copy.

---

<p align="center">
  <sub><b>R.A.I.D. — Retro AI Development</b> · a Discord for AI-assisted retro reverse-engineering, decomp &amp; recomp</sub>
</p>

<p align="center">
  <a href="https://discord.gg/Ad9BwSzctP"><img src=".github/raid-discord.png" alt="Join the Retro AI Development (R.A.I.D.) Discord" width="200"></a>
</p>

## QA

[TheGabeMaster29](https://github.com/TheGabeMaster29) — QA testing.
