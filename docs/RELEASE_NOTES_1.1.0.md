# Sonic the Hedgehog v1.1.0

This stable minor release adds the optional **Clone Driver + Mega PCM** sound
driver, preserves original debug transporter behavior by default, and improves
widescreen effects and act transitions.

## Sound driver mod

- Enable **Mods > Audio > Clone Driver + Mega PCM**, then press Play. Restart
  after changing drivers. The original sound driver remains the default.
- Music, effects, instruments, envelopes and samples are prepared in memory
  from the player's verified Sonic 1 JUE REV00 ROM. No separate sound BIN,
  assembler, Python installation or network connection is required to play.
- Developer builds fetch and assemble pinned, verified driver software. No game
  music, samples or ROM are downloaded or included in the release.
- The replacement 68000 program uses native generated code. Its uploaded Mega
  PCM Z80 program uses the standard Z80 interpreter. The audio scheduler retains
  the FM register latch across timestamped PCM writes.
- The SEGA screen waits for its complete voice sample. Real 1-UP restoration
  preserves the resumed percussion track, and debug pause+A exits silently.
  Clone's sound test uses zero-based numbering; the original display and
  playback range remain intact when the driver is disabled.

## Gameplay and presentation

- Original debug transporter capture is restored by default. The optional
  **Base game bug fixes > Debug transporter escape** mod retains the alternative
  escape behavior. It prevents debug-flight capture and the stuck control lock
  after flying away. Ordinary rides follow the original code in either mode.
- Widescreen explosion and hidden-score effects use their complete ROM artwork,
  avoiding borrowed title-card or partially loaded VRAM patterns. Native rendering,
  graphics loading and object simulation retain their original behavior.
- A stationary Sonic stays in his standing pose during the SBZ2 control handoff.
  Movement, inertia, animation changes and returning input still use the real
  player. The trap sequence retains its original timing.
- Outgoing fades retain the correct Sonic pose artwork and outgoing scene's
  runoff position, including SBZ1's transition into SBZ2.

## Setup and limits

Fresh installations use adaptive **Fit to screen**. Existing aspect, audio and
mod choices are preserved. **32:9 remains experimental.** Supply your own Sonic
1 JUE REV00 ROM; no ROM is included. REV01 remains unqualified. Save states are
build-specific; preserve native saves and settings when updating.

Clone Driver is an optional local-play preview within this stable application.
It uses the pinned Sonic 1 port of Clone Driver 2.8.0.1 and Mega PCM 2.1.
Disable it and restart before netplay. Other platforms, later driver versions
and the optional fixed-image Z80 AOT backend remain unqualified.

See [sound and build details](SOUND_DRIVER_MODS.md) and the included
[software notices](CLONE_DRIVER_NOTICES.txt). Software permission notices do not
grant rights to third-party game material.
