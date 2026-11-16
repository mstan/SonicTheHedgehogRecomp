Sonic the Hedgehog v1.2.1 - Windows x64 stable release

Run SonicTheHedgehogRecomp.exe and select your own Sonic 1 JUE REV00 ROM.
No ROM is included. REV01 and other revisions are unqualified.
Project license: PolyForm Noncommercial 1.0.0; see LICENSE and PERMISSIONS.md.
Source: https://github.com/mstan/SonicTheHedgehogRecomp/tree/v1.2.1

Fresh profiles use adaptive Widescreen / Fit to screen. Existing saved aspect,
audio and mod choices are preserved. 32:9 remains experimental.

Clone Driver + Mega PCM is an optional, disabled-by-default sound mod.
Enable it in Mods > Audio, then press Play. Restart after changing drivers.
Music and samples are generated locally from your ROM. No separate sound BIN,
assembler, Python or network connection is required to play. Clone Driver is for local play. Software permissions
and limitations are in CLONE_DRIVER_NOTICES.txt and docs/SOUND_DRIVER_MODS.md.

Keyboard: arrows move, Z/X/C are A/B/C, Enter is Start. Configure controllers
in the launcher. Escape opens settings; F11 toggles fullscreen.
Shift+F1..F9 saves. Hold F1..F6 or F9 for 1.5 seconds to load.
Hold Ctrl+F7 or Ctrl+F8 for slots 7 and 8. In the 3-button controller
layout, LB saves slot 1 and holding RB loads it. A progress bar shows
the hold. Releasing early cancels the load. Save states are build-specific.
Preserve native saves and settings when updating.

Optional Base game bug fixes > Debug transporter escape preserves the earlier
escape enhancement. It is disabled by default; original debug rides remain.

Optional Mods > Gameplay > Easy level select and debug offers independent
shortcuts. Easy level select opens the original menu with Start at the title.
Easy debug enables the original B toggle without the code or held A at entry.
A changes items; C places them. Both are off by default; restart after changing.
Special Stage item placement retains the original game's limitations.
See docs/EASY_ACCESS.md.

This production build has no development TCP server or trace rings.
See docs/RELEASE_NOTES_1.2.0.md for changes and validation limits.
