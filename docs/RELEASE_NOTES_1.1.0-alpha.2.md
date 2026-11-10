# Sonic the Hedgehog v1.1.0-alpha.2

This Windows x64 pre-release corrects Clone Driver playback and an act-clear
widescreen transition. Clone Driver remains experimental and disabled by default.

## Corrections

- The SEGA presentation waits for the complete Mega PCM voice sample before
  starting its logo hold.
- Percussion resumes after a ring-earned 1-UP. The shared native compiler now
  honors the stack adjustment that ends the jingle parser, preserving the
  restored music track.
- Debug pause+A returns to the title without briefly resuming the previous
  music. Ordinary pause/resume and slow motion retain their existing behavior.
- Clone Driver's sound-test display uses zero-based IDs. The original driver's
  numbering and the menu's playback selection range are preserved.
- Sonic stays offscreen during SBZ1's fade into SBZ2. The outgoing scene keeps
  its own runoff position even after the game selects the next act.

## Setup and scope

Enable **Mods > Audio > Clone Driver + Mega PCM**, then press Play. Restart
after changing drivers. Music, effects, instruments and PCM are prepared locally
from the player's verified Sonic 1 JUE REV00 ROM; no separate BIN, assembler,
Python installation or network connection is needed to play. Developer builds
fetch pinned, verified driver software. No ROM or upstream game assets are bundled.

Fresh profiles use adaptive **Fit to screen**. Existing aspect, audio and mod
choices are preserved. **32:9 remains experimental.** Clone Driver is qualified
for local play with the standard Z80 interpreter; disable it and restart before
netplay. REV01, other platforms and the optional Z80 AOT backend remain unqualified.
Save states are build-specific; preserve native saves and settings when updating.

See [sound setup and build details](SOUND_DRIVER_MODS.md) and the included
[driver notices](CLONE_DRIVER_NOTICES.txt). Software notices do not establish
rights to third-party game music or samples.
