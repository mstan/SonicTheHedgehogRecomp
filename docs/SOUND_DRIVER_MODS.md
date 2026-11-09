# Sonic 1 sound driver mods

Start with a game-owned, default-off **Clone Driver v2 with Mega PCM** mod.
Sonic 1 owns its sound commands, driver data, native-code generation and launcher
registration. The engine continues to supply the shared CPU scheduling,
YM2612/PSG synthesis and audio event queue. Add shared engine interfaces only
when a concrete integration needs them; a general driver plugin loader is not
needed for this first consumer.

The development probe assembles a separate sound image successfully. It does
not install a mod or change the game's current audio behavior. Native execution,
gameplay audio and state restoration remain implementation work.

## Source versions

The linked projects belong to one driver family, with different integrations:

| Source | Relevant distinction |
| --- | --- |
| [Clownacy's driver](https://github.com/Clownacy/Sonic-2-Clone-Driver-v2/tree/865e850af106f0f69189c6ac129428de0e754bb8) | This snapshot's engine identifies itself as v2.9 and embeds a C++ core compiled for the 68000. Its README still names v2.8.0.1. |
| [Sonic 1 integration](https://github.com/TheBlad768/s1disasm-clone-driver/tree/9c6e99358c510fa640868ce21b5fd6aea6cd01f5) | The tested source: assembly Clone Driver v2.8.0.1 with Mega PCM 2.1. Its README still says Mega PCM 2.0; the actual driver files identify 2.1. |
| [Sonic 2 integration](https://github.com/TheBlad768/s2disasm-clone-driver) and [Sonic and Knuckles integration](https://github.com/TheBlad768/skdisasm-clone-driver) | Later consumer references. Neither is part of the Sonic 1 probe or its qualification. |

The [C++ core source](https://github.com/Clownacy/Clone-Driver-v2-CPP) is another
possible implementation route. It depends on ClownMDSDK and its hardware access
interfaces; its existence does not establish a ready-to-link desktop driver.
The first prototype uses the Sonic 1 integration's matching driver and data.
Mixing the latest core with the older port would introduce a second porting
task before the initial integration works.

Use the sound portion of the upstream project. Its complete game defaults to
REV01, bug fixes and optimizations; replacing the gameplay ROM with that build
would also replace unrelated game behavior and invalidate the REV00 hooks.

## Repository ownership

| Location | Responsibility |
| --- | --- |
| This game's `game/` | Driver selection, original-to-driver command translation, initialization, tick and pause hooks, asset validation, driver state and configuration identity. |
| This game's `tools/` | Pinned sound-image assembly, discovery inputs and native-code generation. Keep generated code and game data local. |
| Shared engine | Existing bus, scheduler, chips, audio timestamps and `GameSpec` lifecycle/mod/state interfaces. Extract reusable driver support here after its contract is demonstrated. |

`GameSpec.mods` can compose another package with the existing video mods, and
`load_settings` can read game-owned settings. `instruction_hook` already allows
an audited instruction site to replace a routine. These are candidate integration
points; the probe adds no engine API or runtime hook.

## Mod behavior

The proposed package ID is `sonic1.audio.clone-driver`. It is disabled on a
fresh profile. When disabled, execute the existing original driver path,
including its established correctness fixes, without loading external driver
data or changing the sound RAM/Z80 program.

Enabling the package selects Clone Driver for the whole cold-boot session.
Changing drivers requires a restart because initialization, RAM layouts and
the Z80 program differ. Future sound-driver packages must be mutually exclusive.
Persist explicit player choices. If an enabled package is missing or invalid,
report a launch error instead of presenting the original driver as the selected
replacement. Keep ordinary volume and audio-output settings independent.

The runtime should verify the normal REV00 ROM first, then map the validated
sound payload into its unused cartridge space only for the enabled session.
The proposed load address is `$080000`, immediately after the 512 KiB input
ROM and inside the engine's existing 4 MiB cartridge map. The build can use a
private augmented generation image to emit the additional native functions
into the game's existing dispatch table. This arrangement still needs runtime
qualification; the probe only establishes assembly and address layout.

## Sonic 1 adapter requirements

The stock annotations identify `DACDriverLoad` at `$001352`, `QueueSound1/2/3`
at `$001396/$00139C/$0013A2`, `PauseGame` at `$0013A8` and `UpdateMusic` at
`$071B4C`. Audit their instruction bytes and register/stack contracts before
adding hooks in `game.toml`. Compose the audio hook with the current video hook.

The replacement must consume commands once and suppress the corresponding
original sound operation. The upstream command IDs differ:

| Original REV00 command | Sonic 1 Clone Driver command |
| --- | --- |
| Music `$81-$93` | `$01-$13` |
| Ordinary SFX `$A0-$CF` | `$20-$4F` |
| Waterfall `$D0` | `$50` |
| Fade, SEGA, speed up, slow down, stop `$E0-$E4` | `$FB-$FF` |

Handle silence/empty commands explicitly rather than subtracting `$80` from
every byte. Preserve the distinction between music and SFX queues, priority,
pause/unpause and jingle restoration.

Replace the original sound tick at its actual interrupt scheduling point.
Do not tick from a presentation-frame callback: lag frames, pause loops and
interrupt masking are part of the sound contract. The upstream
`SMPS_UpdateSoundDriver` macro also supplies a reentrancy guard and interrupt
mask handling around `SMPS_UpdateDriver`; calling the bare update entry is not
the complete contract.

Mega PCM supplies both 68000 library code and a different Z80 DAC program.
Discover and recompile its executable 68000 blob, then qualify the uploaded
Z80 image, sample-table writes, bank switching and any code mutation. A build
using Sonic 1's original generated Z80 stepper cannot assume that stepper also
executes Mega PCM. Preserve shared chip synthesis and cycle-stamped writes.

Include driver identity and payload hash in snapshots, rewind/rollback state,
replay configuration and netplay compatibility. Reject snapshots from a different
driver. Old save-state compatibility requires no migration. Preserve native
game saves and current-build restoration correctness.

## Reproduce the assembly probe

Requires Python 3.10+, Git, Macroassembler AS and Clownacy's p2bin. The pinned
upstream checkout includes native tools for Windows, Linux and other hosts.
The script runs the explicitly supplied tool executables, reads the upstream
source and writes into a new output directory outside that checkout. It does
not download dependencies, accept a retail ROM or launch the game.

```powershell
git clone https://github.com/TheBlad768/s1disasm-clone-driver.git C:/Temp/s1-clone-source
git -C C:/Temp/s1-clone-source checkout 9c6e99358c510fa640868ce21b5fd6aea6cd01f5
python tools/clone_driver_probe/build.py `
  --source C:/Temp/s1-clone-source `
  --assembler C:/Temp/s1-clone-source/build_tools/Windows-x86_64/asw.exe `
  --p2bin C:/Temp/s1-clone-source/build_tools/Windows-x86_64/p2bin.exe `
  --output C:/Temp/s1-clone-probe
```

On Linux x86-64, supply `build_tools/Linux-x86_64/asl` and `p2bin`. Linux
execution has not been qualified by this probe.

Outputs include `clone-driver.bin`, a padded `driver-image.bin`, the assembler
listing, logs and `probe.json` with entry points and source/tool/payload hashes.
The leading 56-byte probe header describes the image; it is not an entry point.
Keep these generated files local. The sound driver carries its permission notice
in its assembly header and Mega PCM carries an MIT notice; retain applicable
notices when incorporating their code. The probe also contains upstream game
music and samples, which are not committed here.

## Assembly result and next milestone

Two Windows builds from separate output directories produced the same payload:

| Property | Result |
| --- | --- |
| Payload extent, including probe header | `$080000-$09308D`, 77,966 bytes |
| Driver RAM | 923 bytes (`$39B`), fitting the proposed `$FFF000-$FFF3FF` reservation |
| Queue offset / pause offset | `$370` / `$398` |
| Payload SHA-256 | `6cddc3bfb59ad46de77eed675f583ace00f02b30aa0c7d109b65a3e7d1069d3f` |

This establishes that sound code/data can be assembled independently of the
upstream game. It does not establish playback or native-code coverage.

The next milestone is a native sound test using this image and the existing
chip/scheduler path: SEGA sample, title/GHZ music, jump/ring SFX, pause, speed
shoes, fade/stop and 1-UP restoration. Once that passes, wire the default-off
package into gameplay and compare a disabled build with the current original
path. Complete current-build save/load, rewind and driver-mismatch rejection
before exposing it as a usable mod; qualify netplay or explicitly gate the
experimental driver there until its deterministic contract passes.
