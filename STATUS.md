# Status: Sonic the Hedgehog Recompiled Runner

## What Works

- SEGA logo animation renders correctly
- Title screen, menus, and demo mode render correctly
- Green Hill Zone loads and plays
- Sonic runs, rolls, interacts with terrain (slopes, loops, springs)
- Rings, enemies, item monitors functional
- HUD displays correctly (score, lives, rings, timer)
- Palette fades work (FIXED: register save/restore around VBlank handler)
- All 337 generated functions dispatch without misses
- Music plays (garbled — see Known Bugs)

## Known Bugs

### 1. Jump has no height
Sonic can't jump. Root cause identified: joypad edge-detection timing. The interpreter detects the A button press one frame earlier than native code. In native, Sonic_Move runs on the jump frame and overwrites yvel=$0000 (because ground_speed=0 and cos(angle)*0=0). In the interpreter, the game is already in MdJump2 which skips Sonic_Move, preserving yvel=$F98D from Sonic_Jump. This is a **joypad timing issue**, not a codegen bug. The contextual cycle tracking infrastructure was built to address this but doesn't fix it yet.

### 2. Audio garbled
Partial FM synthesis, alternates between real audio and noise. Likely caused by Z80/FM not advancing during game code execution — the Z80 only runs during ClownMDEmu_Iterate(), not while game code executes between DoCycles calls.

### 3. Flower sprites use wrong art
VDP/VRAM timing issue. Not investigated.

### 4. Scene transitions too fast
Scene transitions process too quickly. Game logic runs without cycle-accurate timing — a full frame's worth of logic executes in one burst rather than interleaved with VDP scanlines.

## Architecture Limitation

The 68K game code runs atomically between VBlanks. Real hardware interleaves 68K execution with VDP scanline rendering (~488 cycles per scanline, 262 scanlines per frame). Our model runs all game logic in one burst per DoCycles call, then renders. This causes:

- VBlank interrupts can't fire mid-frame at the exact right cycle
- Z80 and FM synthesis don't advance during game code execution
- Joypad edge detection timing differs by one frame
- Scene transitions process too many game frames per real-time frame

## Runtime Workarounds

These are real, necessary workarounds that ship in the runner. Each is annotated in `runner/glue.c`.

| # | Mechanism | Why It Exists | Hack? |
|---|-----------|---------------|-------|
| 1 | M68KState save/restore around VBlank handler | Handler's MOVEM pop clobbers D0-D7/A0-A6. RTE doesn't restore registers because handlers return to C, not via 68K RTE. Discovered when palette fade counter D4 got corrupted. | YES |
| 2 | PLC processing in glue_yield_for_vblank | NemDec tile decompression (func_001642) must run from game fiber context. Without this, PLC queue never drains and game softlocks. | YES |
| 3 | $FFFE00 write32 protection | Stack overflow at $FFFE00 corrupts game timer at $FE02, causing restart loop after jump. | YES |
| 4 | A7 clamp at yield | Prevents stack pointer drift above initial SSP ($FFFE00) across frames. Related to #3. | YES |
| 5 | Handler stack at $FFD000 + RAM save/restore | VBlank handler's MOVEM push would corrupt collision response buffer at $FFCFC4+. Redirect handler stack and save/restore 128 words. | YES |
| 6 | Contextual cycle tracking (g_cycle_accumulator) | Generated code increments after every instruction. At 109312 cycles, fires glue_check_vblank(). Foundation for future interrupt timing fix. | NO (infrastructure) |

## Development History

This section documents the investigation arc so future work doesn't re-tread dead ends.

### Fruitful branches

**master (hybrid interpreter mode)** — Functions replaced one-at-a-time inside the running interpreter. Got to 112 verified functions. Proved codegen was correct for leaf functions.

**step2-standalone (fully native)** — 337 functions, no interpreter. Three bugs found and fixed: dark palettes (register clobber), PLC softlock (fiber context), jump-restart loop (stack/RAM collision). Got us to playable state.

**contextual-recompiler (current best)** — Recompiler emits g_cycle_accumulator += N after every instruction. glue_check_vblank() fires at cycle 109312. VBlank handler executes mid-frame. Infrastructure works correctly, but doesn't fix the jump bug — joypad timing offset persists. Foundation for future fix.

**dual-execution, stack-verify, verify-system** — Verification harnesses that proved generated code was correct by running interpreter and native side-by-side and comparing state after each function call. Fruitful for diagnosis.

### Dead-end branches

**experiment-vblank-reorder** — Tried reordering VBlank-before-HBlank. Black screen from palette DMA timing.

**scanline-interleave** — DoCycles stub runs game code in ~488-cycle chunks. Game ran 2x speed because service_vblank resumed game fiber. Partially reverted. Informed the contextual approach.

**cycle-timing** — Various cycle increment values (8/10/20 per bus access). No observable effect.

**vblank-interrupt** — Tried firing handler inside Iterate via Clown68000_Interrupt. Black screen from dual cycle counters (Iterate's local vs glue's global). Do not retry this approach without resolving the dual-counter problem.

## Effective Debug Methodology

The iterative fix loop that successfully found and fixed 3 bugs:

1. **Run Step 2, capture failure point** — Note the frame number and symptom.
2. **Run interpreter baseline at the same frames** — Build with interpreter, capture framelog.
3. **Compare frame logs** — Align by frame number (interpreter init takes ~59 extra VDP frames). Diff game state fields (mode, fcnt, scrl, obj0, yvel).
4. **Identify the divergence** — Dispatch miss? Wrong game state? Crash?
5. **Fix in the right layer** — Codegen bug → recompiler. Missing function → game.cfg. Runtime bug → glue.c.
6. **Regenerate, rebuild, test** — Never commit recompiler changes without regenerated + tested output.

Key insight: always build an interpreter baseline for comparison. Most bugs are visible as divergences in frame state between interpreter and native at the same game frame count.
