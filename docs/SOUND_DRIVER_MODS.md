# Sonic 1 sound driver mods

**Clone Driver + Mega PCM** is an experimental Sonic 1 mod, disabled by default.
It runs the linked port's 68000 sound program as generated native code and
uses the engine's existing Z80, YM2612, PSG and audio scheduling. Turning it
off retains the original driver and its existing correctness fixes.

The game repository owns the adapter, launcher package and generation inputs.
The engine and recomp-ui require no source changes. Sonic 2 and Sonic 3 are
outside this prototype's scope.

## Enable the mod

Use a build prepared with the sound probe below. In **Mods → Audio → Clone
Driver + Mega PCM**, select that build's `clone-driver.bin`, then enable the
feature and press Play. The file must match the build's exact size and SHA-256.
An invalid enabled pack prevents launch. A disabled feature does not read its
pack, including when a previously selected file has been removed.

Driver selection is fixed for the running process. Restart the application
after changing it; guest reset, save/load and rewind cannot apply a pending
selection. Sound choices and the selected path persist in `sonic1-sound.ini`
beside the engine's `settings.ini`. Common audio, controller and widescreen
settings retain their existing storage and behavior.

The prototype supports local play with the standard SuperZazu Z80 backend,
which is the engine's default. A build opting into the original fixed-image
Z80 AOT backend refuses Clone Driver. Netplay with Clone Driver is gated until
its deterministic compatibility is qualified. The new **68000** driver and
Mega PCM library code run through the native dispatch table; the uploaded
**Z80** program uses the existing interpreter.

The sound pack uses the linked Sonic 1 port's converted music and SFX data.
For example, its Star Light arrangement uses DAC percussion. Enabling it may
change the sound as well as the driver implementation. It preserves the stock
REV00 gameplay ROM; the complete upstream game, which defaults to REV01 and
other gameplay changes, is not substituted.

## Build the prototype

Requires Python 3.10+, Git, Macroassembler AS, Clownacy's p2bin and the normal
Sonic 1 build dependencies. Supply the normal JUE REV00 ROM as
`game/sonic.bin` and initialize the project's pinned engine/UI dependencies.

```powershell
git clone https://github.com/TheBlad768/s1disasm-clone-driver.git C:/Temp/s1-clone-source
git -C C:/Temp/s1-clone-source checkout 9c6e99358c510fa640868ce21b5fd6aea6cd01f5
python tools/clone_driver_probe/build.py `
  --source C:/Temp/s1-clone-source `
  --assembler C:/Temp/s1-clone-source/build_tools/Windows-x86_64/asw.exe `
  --p2bin C:/Temp/s1-clone-source/build_tools/Windows-x86_64/p2bin.exe `
  --output C:/Temp/s1-clone-probe
cmake -S . -B build-clone -G "Visual Studio 17 2022" -A x64 `
  -DSONIC_CLONE_DRIVER_PROBE_DIR=C:/Temp/s1-clone-probe `
  -DGENESIS_Z80_RECOMP=OFF
cmake --build build-clone --config Release --target SonicTheHedgehogRecomp --parallel 1
```

Leave `SONIC_CLONE_DRIVER_PROBE_DIR` empty for a normal build without the
replacement code. Such a build keeps the package unavailable and refuses an
enabled saved selection rather than silently changing the requested driver.
On Linux x86-64 the upstream native tools are
`build_tools/Linux-x86_64/asl` and `p2bin`; this prototype's execution has been
qualified on Windows x64 only.

The probe contains only sound code/data and a 60-byte metadata header.
`prepare_native.py` checks its identity and creates an augmented private image
under the build directory, keeping all 524,288 original ROM bytes intact.
Sound code starts in the unused cartridge bank at `$080000`. Discovery reuses
the engine's source-verified label and jump-table extractors. Generated C,
ROMs and music/sample packs remain local and are not checked in.

Current payload: 77,994 bytes; SHA-256
`686f441bf5f3ef14ffa3b4c5e385784249329675d428ebb2d28a5bee78f7c336`.
It uses 923 bytes of the existing `$FFF000-$FFF3FF` sound RAM reservation.
The build stages [driver and Mega PCM notices](CLONE_DRIVER_NOTICES.txt) beside
the executable. These notices do not grant rights to game music or samples.

## Adapter behavior

Hooks replace the stock sound initialization, three queue entry points and
`UpdateMusic` at its real interrupt scheduling point. Pause writes go to the
replacement layout while the rest of the original pause loop continues.
The driver's own interrupt-mask and reentrancy macro wraps its update routine.
Presentation frames do not drive sound ticks.

The pinned shared engine preserves the guest FM address latch on Z80 data
writes before merging the audio timestamps. Mega PCM retains register `$2A`
across PCM writes. Sorting those raw data ports alongside 68000 FM updates
could otherwise redirect sample bytes into note and instrument registers.
The engine materializes an address/data pair at each Z80 data write's timestamp,
without adding guest bus accesses or changing driver timing.

| Original REV00 command | Clone Driver command |
| --- | --- |
| Music `$81-$93` | `$01-$13` |
| Ordinary SFX `$A0-$CF` | `$20-$4F` |
| Waterfall `$D0` | `$50` |
| Fade, SEGA, speed up, slow down, stop `$E0-$E4` | `$FB-$FF` |

Empty/silence and unsupported IDs do not become arbitrary replacement commands.
Queue hooks retain the original D0 value and MOVE.B condition flags. File
states and rewind/rollback snapshots include the active driver and pack
identity, including the frame-0 snapshot. Mismatched-driver loads are rejected.
Old save-state migration is not required; native game saves are unaffected.

## Validation

Windows checks cover enabled boot/audio, actual Green Hill gameplay and jump
input, pause/resume, warm and cold save/load, and the engine's real rewind and
rollback-state self-tests. Restored screenshots and sound RAM match exactly.
An enabled invalid pack and a file state from the other driver are rejected.

With the mod disabled, a 600-frame audio capture is byte-identical to v1.0.9,
and every sampled framebuffer hash matches. Filtered and unfiltered stock
captures also remain byte-identical across the FM latch correction. A matching
Clone Driver capture applies all 81,629 PCM writes to the intended DAC register;
all register write counts match their issued destinations. Enabled boot and gameplay record
no native dispatch misses or Tier-3 floor execution.

Driver-level queue injections separately exercise Star Light, 1-UP restoration,
speed-up/slow-down, fade, Green Hill and stop. The 1-UP finishes and restores
all five active FM pointers into the Star Light sequence; speed flags toggle
and stop clears every music track. These injections bypass gameplay triggers.
They supplement the real gameplay check and the adapter contract tests.

To build the contract test, configure with `-DSONIC1_SOUND_DRIVER_TESTS=ON`,
build `sonic1_sound_driver_test`, then run it with the matching pack path and
`<new-scratch-directory>/settings.ini`. Create that empty scratch directory
first. It tests all 256 IDs in all three queues, condition flags, pause RAM
isolation, provider composition, settings persistence, frame-0 identity and
rejection of a different driver's state.

This is a first-consumer prototype, with listening review and broader song,
stage and platform coverage still open. It does not qualify other Sonic games
or the optional Z80 AOT backend.

## Upstream sources

The tested source is [TheBlad768's Sonic 1 integration at the pinned commit](https://github.com/TheBlad768/s1disasm-clone-driver/tree/9c6e99358c510fa640868ce21b5fd6aea6cd01f5),
containing assembly Clone Driver v2.8.0.1 and Mega PCM 2.1. Its README still
names Mega PCM 2.0; the actual driver identifies 2.1.
[Clownacy's later driver snapshot](https://github.com/Clownacy/Sonic-2-Clone-Driver-v2/tree/865e850af106f0f69189c6ac129428de0e754bb8)
identifies its engine as v2.9 and uses a [C++ core compiled for the 68000](https://github.com/Clownacy/Clone-Driver-v2-CPP).
That is a separate future integration rather than part of this port.
