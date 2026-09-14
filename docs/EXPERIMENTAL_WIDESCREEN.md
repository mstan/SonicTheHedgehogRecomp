# Sonic 1 custom widescreen experiment

The original renderer remains the default. Enable **Mods → Widescreen** in the
recomp-ui launcher or in-game Escape menu, then choose **16:9**, **21:9**,
**32:9**, or **Adaptive**. All four choices use the custom renderer; selecting
an aspect without enabling the mod does not opt in. Resize/maximize the window,
or press F11 for fullscreen. The built-in provider follows Super Metroid's
recomp-ui mod interface; no external mod archive is needed.

Settings persist in `settings.ini` under `[mods.widescreen]` (`enabled = 0|1`,
`aspect = fit|16:9|21:9|32:9`). A command-line `--widescreen` overrides these
settings for that launch. Adaptive is the initial aspect, but the mod itself
starts disabled. The custom renderer is disabled in netplay sessions.

```powershell
.\build\Release\SonicTheHedgehogRecomp.exe .\segagenesisrecomp\sonicthehedgehog\sonic.bin --no-launcher --widescreen fit
```

Modes:

- `fit`: follows the drawable window aspect, with a minimum native 320px view.
- `32:9`, `64:9`, or another positive `W:H`: fixed aspect, not stretched pixels.
- `stage`: shows the full foreground layout width at the current camera height.
- `off`: original native view, also overriding persisted legacy widescreen.

There is no 32:9 ceiling. Host framebuffer storage and the SDL texture grow with
the requested width, subject to available memory and the GPU's texture limit.
Green Hill Act 1's full layout is 10,240 pixels wide. Full-stage mode is an
overview: fitting it into an ordinary window makes the game very small.

## Rendering approach

Like the adjacent NES SMB/SMB2 custom renderers, this reads the world layout
instead of stretching or repeating the hardware's streamed foreground plane.
Sonic's RAM layout, 256px chunks, 16px blocks, live VRAM patterns and CRAM palettes
supply the foreground. Camera alignment uses the frame's VDP scroll, unwrapped
against the copied game camera, so tile streaming and presentation agree.

The enhanced path reconstructs foreground **and background** from the layout.
It uses the live per-line parallax and palettes without repeating the hardware's
small, partially stale background tile buffer. Green Hill's perspective water
also receives full-layout data where the native view reads wrapped stale cells.
Green Hill's sky, cliffs and perspective water are reprojected through the wide
camera's stage-edge clamp, preventing background drift while the expanded
foreground view remains stationary near spawn.

Game-owned hooks capture the sprite queues before native `BuildSprites`, then
match the resulting native sprite-table upload to the displayed frame. Host
sprite pieces have unrestricted horizontal coordinates and do not inherit the
VDP's per-line sprite limit. The HUD is anchored at the output's left inset;
the world view remains anchored at stage boundaries and otherwise centered
around the original camera.

Object activation and culling use the expanded viewport. Ordinary placed rings
live in the host scene, retaining the game's collected-bit state and calling
its ring reward/SFX routine. Lost rings retain their original physics. Actors
still execute their original game routines. This is deliberately an enhanced
mode, not a promise of identical native RAM or hardware limitations.

Loading and culling share the same 128px-rounded activation cells. The initial
prototype loaded some objects a cell too early, culled them immediately, then
remembered them as already loaded. This removed real platforms (including
Marble's first grassy platform) and made pits impassable. Both boundaries now
come from the same calculation; this is not an alteration of stage geometry.

Title scenery extends across the window while its logo remains centered.
Special stages reconstruct the complete rotating layout from live block
descriptors. Fades retain the full-width canvas; full-stage mode retains its
last level width through scene changes. Level select keeps its centered text
composition without resurrecting stale title scenery.

The original renderer/instruction behavior remains the launch default. Sonic's
old bounded VDP toggle is replaced by the custom Widescreen mod; other Genesis
games retain their existing view controls. No generated C is hand-edited:
audited instruction-hook sites live in Sonic's `game.toml`, routed through a
generic `GameSpec` callback.

## Prototype limits

- This is experimental. Later-zone playtesting exposed an activation/culling
  regression after initial Green Hill approval; the corrected production build
  passed renewed owner playtesting. Long-session coverage is not exhaustive.
- Non-ring actors still use the existing dynamic-object storage. At extreme
  widths it can fill; spawning prioritizes actors closest to Sonic and exposes
  `pool_pressure` in `custom_video`. Host-owned placed rings do not consume that
  storage. This remains an implementation limit, not a fidelity requirement.
- Mid-level mode changes, save-state rewinds, bosses and later-zone scripted
  activation rules need additional validation.
- Water effects, unusual raster effects and later-zone dynamic terrain need
  further game-specific validation. Full-stage rendering costs more CPU/GPU time.
- Labyrinth has a separate Burrobot interior-label dispatch issue at `00AD1A`,
  also reproduced in the older native build. This release does not fix it.

## Validation

`segagenesisrecomp/tests/runtime/run_sonic1_custom_video.py` runs native, 32:9,
64:9, full-stage, live-resize and special-stage cases with finite, free-running
inputs. Special-stage entry uses the title's real level-select cheat. It
requires Pillow/numpy, the owner's ROM, and `debug.ini` beside the executable
(contents can be `port=4438`). The debug server is never used to pause/step or
patch the game. Run against an idle test executable, not a player's instance.

```powershell
python segagenesisrecomp/tests/runtime/run_sonic1_custom_video.py --exe build/Release/SonicTheHedgehogRecomp.exe --rom segagenesisrecomp/sonicthehedgehog/sonic.bin --out build/video-validation
```

The enhanced checks cover HUD anchoring, full-width title margins, special-stage
sprite publication, constant fixed-aspect dimensions through transitions,
foreground/background layout mapping, zero dispatch misses, and resizing
through logical widths 398, 796, 1792 and 320. Background comparisons exclude
cells outside the native streamed strip, separately counted as
`background_unstreamed`; those stale native cells are intentionally replaced.
The BG mapping diagnostic samples the original camera independently from the
displayed wide-camera parallax correction. Native and enhanced routes differ
because earlier enemy activation changes encounters; these are presentation
and smoke checks, not a proof of identical gameplay trajectories.
Pass `--native-reference <first-pass-native-capture-directory>` to additionally
verify the opt-out's RAM, VRAM and PNG captures against an untouched baseline.

The CTest `sonic1_video` checks synthetic terrain, screen-anchored HUD, wide
sprites, ring collection state, culling, special-stage blocks, title/menu
separation and immutable displayed sprite lists. Its optional
`--replay <ram.bin> <vram.bin>` checks GHZ layout reconstruction offline; both
earlier start/scroll captures pass with zero foreground or streamed-BG errors.
This replay does not validate live sprite timing or raster interrupts.
The `video_mod` CTest verifies native defaults, all four aspect choices,
provider identity/option validation, persistence and controller-binding
preservation. A striped-background test covers GHZ's clamped-camera drift.

`run_sonic1_zone_video.py` compares input-only Spring Yard and Marble routes
with the mod off, at native logical width, and at 32:9. It checks the surviving
Marble platform, foreground mapping and independent ROM data reconstruction.
`check_sonic1_stage_data.py` independently decodes Kosinski chunks, Enigma blocks
and level-layout rows to compare against captured RAM. The missing-platform
case also has a CTest reproducer that failed before the shared-boundary fix.
Keep before/after output directories distinct; the reference is not regenerated
to disguise a mismatch. These routes do not establish full-stage completion.

`tests/tools/test_game_hooks.py` executes six real-generated-C cases covering
disabled, continuing and replacing hooks through direct and tail-split calls.
`tests/tools/test_stack_skip.py` separately covers the jump-SFX unwind fix.
Both require `--recompiler <GenesisRecomp.exe> --cc <GCC-compatible compiler>`.
