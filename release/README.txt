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
Arrow keys = D-pad, A/S/D = A/B/C, Enter = Start. Gamepads supported (SDL2).
Save states: Shift+F1..F9 save, F1..F9 load.

KNOWN ISSUES
------------
- The jump sound effect can sound slightly off ("boop") compared to
  original hardware; under investigation.
- A brief audio/video hitch can occur on the SEGA logo screen.

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
