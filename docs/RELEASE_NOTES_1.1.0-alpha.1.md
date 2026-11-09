# Sonic the Hedgehog v1.1.0-alpha.1

This Windows x64 pre-release adds an experimental **Clone Driver + Mega PCM**
sound mod and two Scrap Brain Act 2 corrections.

## Clone Driver preview

- Enable **Mods → Audio → Clone Driver + Mega PCM**, then press Play.
  The mod is disabled by default; disabling it uses the original sound driver.
  Restart the application after changing drivers.
- Music, sound effects, instruments, PSG envelopes and PCM samples are prepared
  automatically from the player's verified Sonic 1 JUE REV00 ROM. Players need
  no separate sound BIN, assembler, Python installation or network connection.
- Developer builds fetch pinned, hash-checked driver software and assembler
  inputs. No upstream Sonic music or sample pack is downloaded or bundled.
- The replacement 68000 driver uses generated native code; Mega PCM's uploaded
  Z80 program uses the standard Z80 interpreter. The shared audio scheduler
  preserves the FM register latch when merging Z80 PCM writes and 68000 updates.
- Clone Driver is currently for local play. Disable it and restart before
  netplay. Other Sonic games and the optional Z80 AOT backend are unqualified.

See [sound setup and build details](SOUND_DRIVER_MODS.md) and the included
[driver notices](CLONE_DRIVER_NOTICES.txt). These notices do not establish
rights to underlying third-party game material.

## Corrections

- Debug flight no longer becomes captured by an SBZ teleporter. Entering debug
  flight during a transporter ride releases that object's control lock, so
  leaving flight restores normal movement. Ordinary transporter rides retain
  their original capture and release behavior.
- During an outgoing level fade, widescreen pairs Sonic's captured animation
  pose with its matching ROM graphics. This avoids displaying a new pose using
  the previous pose's dynamic graphics in the screen margins. The correction
  also applies to interpolated presentation frames.

## Verification and limits

Windows validation covers the complete music/effect conversion, filtered and
unfiltered audio, launcher activation and persistence, gameplay, pause/resume,
current-build save/load, rewind and rollback-state checks. Renderer regressions
cover outgoing fades, interpolated poses and guest RAM/VRAM/CPU immutability.
Transporter checks cover ordinary rides and entering/leaving debug flight.
These are targeted checks, not a complete new playthrough or platform matrix.

Fresh installations use adaptive **Fit to screen**. Existing saved aspect,
audio and mod choices are preserved. **32:9 remains experimental**. Supply
your own Sonic 1 JUE REV00 ROM; no ROM is included. REV01 remains unqualified.
Save states are build-specific; preserve native saves and settings when
updating. This production package has no development TCP server or trace rings.
