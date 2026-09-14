SonicTheHedgehogRecomp — native static recompilation of Sonic the Hedgehog
===========================================================================

A native Windows port produced by statically recompiling the Sega Genesis
68000 code to C. No emulator core: recompiled CPU code, clean-room
VDP/bus/scheduler, ymfm FM synthesis, clean-room SN76489 PSG.

BRING YOUR OWN ROM
------------------
This package contains NO game data. Place your own legally obtained
Sonic the Hedgehog (World) ROM next to the exe, named:

    sonic.bin

then run SonicTheHedgehogRecomp.exe.

CONTROLS
--------
Arrow keys = D-pad, Z/X/C = A/B/C, Enter = Start. Gamepads supported (SDL2).
Save states: Shift+F1..F9 save, F1..F9 load.
Escape = runtime settings. F11 = fullscreen.

WIDESCREEN MOD (OPT-IN)
----------------------
In the recomp-ui launcher or Escape menu, open Mods and enable Widescreen.
Choose 16:9, 21:9, 32:9, or Adaptive. Adaptive follows the entire window,
without a 32:9 limit. Settings are saved beside the executable.
Disabled uses the original renderer. No ROM patch or external mod file is needed.

The custom renderer expands scenery, ring/object activation, title and special
stages; anchors the HUD to the screen; and preserves a full-width fade canvas.
Green Hill's background parallax follows the expanded camera near stage edges.
The jump-SFX extra "boop" correction applies in both native and wide modes.

KNOWN ISSUES
------------
- Widescreen is experimental: extreme widths can exhaust dynamic actor storage,
  and earlier activation changes enemy timing. Later zones, bosses and
  save-state rewinds need further validation. The mod is disabled in netplay.
- A brief audio/video hitch can occur on the SEGA logo screen.
- Labyrinth has an existing Burrobot control-flow issue, also reproduced in an
  older native build. It is not fixed in this release.

LICENSE
-------
This software: PolyForm Noncommercial 1.0.0 — see LICENSE.
Third-party components (ymfm BSD-3-Clause, superzazu Z80 MIT, clowncommon
ISC, SDL2 zlib): see THIRD-PARTY-LICENSES.md.

Sonic the Hedgehog is a trademark of SEGA. This project is not affiliated
with or endorsed by SEGA. No SEGA assets are distributed.

SOURCE
------
https://github.com/mstan/SonicTheHedgehogRecomp
