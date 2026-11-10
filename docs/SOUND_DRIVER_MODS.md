# Sonic 1 sound driver mods

**Clone Driver + Mega PCM** is an experimental Sonic 1 mod, disabled by default.
It runs the linked port's 68000 sound program as generated native code and
uses the engine's existing Z80, YM2612, PSG and audio scheduling. Turning it
off retains the original driver and its existing correctness fixes.

The game repository owns the adapter, launcher package and generation inputs.
The shared engine supplies the generic Z80/audio execution and the FM latch
correction described below. The launcher uses recomp-ui's existing mod
interface. Sonic 2 and Sonic 3 are outside this prototype's scope.

Each adapter owns its instruction sites. Widescreen culling explicitly lists
its object-range sites and leaves audio/debug operands unchanged, even when
the corresponding optional feature is disabled.

## Enable the mod

Select your Sonic 1 JUE REV00 ROM, enable **Mods → Audio → Clone Driver + Mega
PCM**, and press Play. The game prepares the sound assets from that ROM
automatically. No separate BIN, assembler, Python installation or network
connection is needed to play. The mod remains disabled by default.

Music, effects, FM instruments, PSG envelopes and PCM samples come from the
player's ROM. Conversion happens once at boot in memory, outside the original
ROM bank. The ROM file and its original 524,288 bytes remain unchanged. An
unsupported ROM is rejected when the driver is enabled.

Driver selection is fixed for the running process. Restart the application
after changing it; guest reset, save/load and rewind cannot apply a pending
selection. Sound choices persist in `sonic1-sound.ini`
beside the engine's `settings.ini`. Common audio, controller and widescreen
settings retain their existing storage and behavior.

The prototype supports local play with the standard SuperZazu Z80 backend,
which is the engine's default. A build opting into the original fixed-image
Z80 AOT backend refuses Clone Driver. Netplay with Clone Driver is gated until
its deterministic compatibility is qualified. The new **68000** driver and
Mega PCM library code run through the native dispatch table; the uploaded
**Z80** program uses the existing interpreter.

The stable application release keeps this alternate driver opt-in. Its pinned
software is the Sonic 1 integration's Clone Driver 2.8.0.1; later Clone Driver
versions and their sound/fade changes are separate integrations.

## Original game quirks

**Mods > Gameplay > Base game bug fixes > Debug transporter escape** is a
separate optional enhancement. It prevents debug-flight capture and releases an
active transporter before flying away. It is disabled by default, preserving
the original debug transporter behavior. Normal transporter rides are unchanged.
Restart after changing the choice. It is stored in `sonic1-bug-fixes.ini` and
included in the running configuration and state identity.

The ROM converter follows the linked Sonic 1 port's music and SFX conversion.
For example, its Star Light arrangement uses DAC percussion. Enabling it may
change the sound as well as the driver implementation. It preserves the stock
REV00 gameplay ROM; the complete upstream game, which defaults to REV01 and
other gameplay changes, is not substituted.

## Build support

Use the normal Sonic 1 build dependencies, Python 3.10+, the JUE REV00 ROM at
`game/sonic.bin`, and the pinned engine/UI dependencies. On Windows x64 and
Linux x86-64, the normal configure step automatically fetches and builds the
driver software. The first developer build needs internet access; verified
cached inputs work offline afterward. Players do not run these tools.

```powershell
cmake -S . -B build-clone -G "Visual Studio 17 2022" -A x64 `
  -DSONIC_CLONE_DRIVER_SUPPORT=ON `
  -DGENESIS_Z80_RECOMP=OFF
cmake --build build-clone --config Release --target SonicTheHedgehogRecomp --parallel 1
```

`SONIC_CLONE_DRIVER_SUPPORT=AUTO` is the default. `OFF` builds the normal game
without replacement-driver support. On other build hosts, `AUTO` keeps the
normal game build available; supply native `SONIC_CLONE_DRIVER_ASSEMBLER` and
`SONIC_CLONE_DRIVER_P2BIN` paths with `ON` to include the mod. Runtime execution
has been qualified on Windows x64 only.

The fetcher uses an explicit allowlist of fourteen driver/configuration/macro
files at a pinned commit. Each source file and bundled native build tool has
an exact size and SHA-256 in `tools/clone_driver_probe/sources.lock.json`.
It does not download music, samples, artwork, levels, or the complete game
repository. A separate include tree contains only those software files and
empty asset tables, preventing accidental assembly of upstream game assets.

The generated program contains only driver software, sample descriptors and
a 60-byte metadata header. It is embedded in the executable.
`prepare_native.py` checks its identity and creates an augmented private image
under the build directory, keeping all 524,288 original ROM bytes intact.
Sound code starts in the unused cartridge bank at `$080000`. Discovery reuses
the engine's source-verified label and jump-table extractors. Generated C,
ROMs and build products remain local and are not checked in. Developer-only
`SONIC_CLONE_DRIVER_PROBE_DIR` can use an already prepared software-only input,
for example when the assembler runs on another host. Full music/sample packs
are rejected and have no runtime loading path.

Current software program: 14,984 bytes; SHA-256
`b13bba8aeb037aebdba2ac679eefed576b0964a066eca92adaeb1301555eaa45`.
It uses 923 bytes of the existing `$FFF000-$FFF3FF` sound RAM reservation.
The build stages [driver and Mega PCM notices](CLONE_DRIVER_NOTICES.txt) beside
the executable. These notices do not grant rights to game music or samples.

## Adapter behavior

Hooks replace the stock sound initialization, three queue entry points and
`UpdateMusic` at its real interrupt scheduling point. Pause writes go to the
replacement layout while the rest of the original pause loop continues.
The driver's own interrupt-mask and reentrancy macro wraps its update routine.
Presentation frames do not drive sound ticks.

The SEGA screen waits for Mega PCM's actual sample completion before starting
the original logo hold. Debug pause+A stops the replacement driver while muted,
so returning to the title does not briefly resume the previous music. Ordinary
pause/resume and slow motion keep their original behavior. With Clone enabled,
the sound-test display uses `$00-$4F`; the original driver's `$80-$CF` display,
selection range and queued playback IDs remain unchanged when disabled.

The shared 68000 generator also accounts for immediate ADDA/SUBA adjustments
to SP when unwinding native calls. The 1-UP restoration discards three parser
return frames this way; executing those discarded continuations would overwrite
the restored DAC track with the old jingle pointer.

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
An unsupported ROM and a file state from the other driver are rejected.

With the mod disabled, a 600-frame audio capture is byte-identical to v1.0.9,
and every sampled framebuffer hash matches. Filtered and unfiltered stock
captures also remain byte-identical across the FM latch correction. A matching
Clone Driver regression capture applies PCM writes to the intended DAC register;
all register write counts match their issued destinations. Enabled boot and gameplay record
no native dispatch misses or Tier-3 floor execution.

An actual 100-ring pickup verifies the extra-life trigger and subsequent DAC
restoration. The resumed track stays in its original song bank and continues
playing percussion after the fade-in. Generated-C regression checks cover
three-frame stack skips, signed word/long adjustments, split tails and mixed
stack data. A full SBZ1-to-SBZ2 transition also verifies that selecting the next
act cannot discard the outgoing scene's widescreen runoff position during its fade.

Driver-level queue injections separately exercise Star Light, 1-UP restoration,
speed-up/slow-down, fade, Green Hill and stop. The 1-UP finishes and restores
all five active FM pointers into the Star Light sequence; speed flags toggle
and stop clears every music track. These injections bypass gameplay triggers.
They supplement the real gameplay check and the adapter contract tests.

To build the contract test, configure with `-DSONIC1_SOUND_DRIVER_TESTS=ON`,
build `sonic1_sound_driver_test`, then run it with the original ROM path and
`<new-scratch-directory>/settings.ini`. Create that empty scratch directory
first. It tests all 256 IDs in all three queues, condition flags, pause RAM
isolation, provider composition, settings persistence, frame-0 identity and
rejection of a different driver's state. `sonic1_sound_assets_test <ROM>` checks
exact ROM identity, rejected inputs, output bounds, immutable ROM bytes,
aliased output and repeated preparation. A source-derived reference comparison
also checks all nineteen tracks, forty-nine effects, nine PSG envelopes and
four PCM buffers. The four PCM buffers match the earlier local pack exactly.

This is a first-consumer prototype, with broader stage and platform coverage
still open. It does not qualify other Sonic games
or the optional Z80 AOT backend.

## Upstream sources

The tested source is [TheBlad768's Sonic 1 integration at the pinned commit](https://github.com/TheBlad768/s1disasm-clone-driver/tree/9c6e99358c510fa640868ce21b5fd6aea6cd01f5),
containing assembly Clone Driver v2.8.0.1 and Mega PCM 2.1. Its README still
names Mega PCM 2.0; the actual driver identifies 2.1.
[Clownacy's later driver snapshot](https://github.com/Clownacy/Sonic-2-Clone-Driver-v2/tree/865e850af106f0f69189c6ac129428de0e754bb8)
identifies its engine as v2.9 and uses a [C++ core compiled for the 68000](https://github.com/Clownacy/Clone-Driver-v2-CPP).
That is a separate future integration rather than part of this port.
