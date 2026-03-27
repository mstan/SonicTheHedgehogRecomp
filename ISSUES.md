# Sonic 1 Recompiler — Open Issues

## ISSUE-001: Jump Has No Height (Step 2 Native) — FIXED 2025-03-25

**Status:** FIXED
**Root cause:** Recompiler didn't handle `addq.l #4,sp` + `rts` early-exit pattern
**Fix:** Recompiler commit `8ddc234`, runner commit `4aa5ac5`

### The Symptom

In the Step 2 native build, pressing A (jump) caused Sonic to briefly go airborne
but with no height — yvel was near 0 instead of the expected -1651 ($F98D). The
interpreter build had no issue: full 99px jumps, 60 frames airborne.

### Investigation Timeline

**Phase 1: Build the right tooling**

Previous debugging relied on stdout frame logs and single-trace memory diffs.
These could show THAT the jump failed but not WHY — there was no way to query
live game state, traverse Sonic's state over time, or inject inputs scriptably.

We built a TCP debug server (port 4378) modeled on the sibling projects
(snesrecomp-v2, gbarecomp). This gave us:
- `sonic_state` / `sonic_history`: snapshot and time-series of Sonic's position,
  velocity, routine, status, angle, and input — per frame
- `set_input` + `run_frames`: scriptable, reproducible input injection
- `read_joypad_port`: manual hardware port read for comparing VBlank vs poll context
- `io_log`: per-access logging of $A10003 reads/writes with VBlank flag
- `read_ram` / `read_memory`: arbitrary memory inspection
- `find_jumps.py`: automated scan of 36,000-frame ring buffer for jump events
- `frame_info` / `frame_range`: frame history traversal

**Phase 2: Confirm the interpreter works**

Launched the interpreter build, user jumped 5 times. `find_jumps.py` confirmed:
```
Jump 1: frames 1759-1819, peak yvel=-1651, height=99px
Jump 2: frames 1827-1886, peak yvel=-1651, height=98px
(5 jumps total, all correct)
```

**Phase 3: Eliminate the IO port hypothesis**

Initial suspicion was that ReadJoypads couldn't read the A button from the
hardware port in native mode. IO logging showed $A10003 returning different
values inside VBlank vs poll context:

```
VBlank:  [IO-R] $A10003 => 0x33  (A not pressed)
Poll:    phase0_raw: 0x23         (A pressed)
```

This turned out to be a **test harness bug**: `dbg.py` uses separate TCP
connections per command, causing timing gaps between `set_input` and `run_frames`.
With a persistent connection (`test_jump.py`), the IO port works correctly:

```
[INPUT-CB] A queried: keys=0x48 result=1
[IO-R] $A10003 => 0x23 (vblk=1)  ← correct in VBlank handler
```

**Phase 4: Observe the actual failure**

With working IO and scriptable input, we observed Sonic during a jump attempt:
```
Frame +1: yvel=110  status=2 (airborne, but falling — no upward velocity)
Frame +2: yvel=-14  status=0 (back on ground)
```

The jump velocity (-1651) was being set by Sonic_Jump but immediately overwritten
by Sonic_Move's ground velocity calculation (≈0 on flat ground).

**Phase 5: Discover the real root cause**

Reading the Sonic 1 disassembly (`s1disasm/_incObj/01 Sonic.asm`) revealed that
Sonic_Jump uses a 68K idiom to skip the rest of MdNormal's subroutine chain:

```asm
Sonic_Jump:
    ; ... check A button, set yvel=-1651, set airborne flag ...
    addq.l  #4,sp           ; Discard MdNormal's return address
    ; ... play sound, set animation ...
    rts                     ; Returns to MdNormal's CALLER, skipping Sonic_Move
```

MdNormal calls subroutines sequentially:
```asm
Sonic_MdNormal:
    bsr.w   Sonic_Jump       ; ← if jump triggers, skips everything below
    bsr.w   Sonic_SlopeResist
    bsr.w   Sonic_Move       ; ← THIS overwrites yvel on flat ground
    bsr.w   Sonic_Roll
    jsr     SpeedToPos
    ...
```

On real 68K hardware, `addq.l #4,sp` pops the return address from the stack.
The subsequent `rts` reads the NEXT address — returning to MdNormal's caller.
Sonic_Move never executes on the jump frame.

In the recompiled C code:
```c
g_cpu.A[7] += 4u;    // Adjusts 68K stack pointer...
// ... play sound, set flags ...
return;               // ...but C 'return' goes back to MdNormal, NOT its caller!
```

The C `return` always returns to the immediate caller (MdNormal), which then
calls Sonic_Move, which overwrites yvel.

### The Fix

**Recompiler change** (`code_generator.c`):

1. **Pre-scan each function** for `addq.l #N,A7` (N multiple of 4) instructions
2. For functions that have them, emit a **local** `int _sp_popped = 0;`
3. At `addq.l #N,A7`, emit `_sp_popped += N/4;`
4. At `rts` sites within the same function, emit:
   ```c
   if (_sp_popped > 0) { _sp_popped--; g_rte_pending = 1; }
   return;
   ```
5. The caller's existing post-JSR check (`if (g_rte_pending) { ... return; }`)
   catches the propagation and returns from MdNormal without calling Sonic_Move.

**Why function-local?** An earlier attempt used a global `g_early_return` flag,
but intermediate JSR calls (like PlaySound inside Sonic_Jump) consumed the flag
at their own RTS before Sonic_Jump's RTS could use it. Making it function-local
ensures only the function that did the `addq.l #4,sp` checks the flag at its
own RTS sites.

**Why not use `g_rte_pending` directly at the `addq` site?** Because there are
JSR calls between `addq.l #4,sp` and the eventual `rts` (PlaySound at $013458).
These calls have `if (g_rte_pending) { g_rte_pending = 0; return; }` checks that
would consume the flag prematurely.

### Verification

```
Interpreter:  yvel=-1651, 61 frames airborne, height=99px
Native (fix): yvel=-1651, 60 frames airborne, height=99px  ✓
```

### Game-Agnostic Impact

The `addq.l #4,sp` + `rts` pattern is a standard 68K idiom used across many
games. This fix applies to ALL recompiled Genesis games, not just Sonic 1.
Any function that uses this stack manipulation to skip remaining subroutine
calls in a caller will now work correctly.

---

## ISSUE-002: Garbled Purple Flowers in GHZ (Step 2 Native)

**Status:** Root cause identified, fix pending
**Severity:** Cosmetic
**Date:** 2025-03-26

### Summary

The purple decorative flowers in Green Hill Zone appear garbled/corrupted in
native Step 2. All other level art renders correctly.

### Root Cause

**PLC (Pattern Load Cue) timing desynchronization.** VRAM tile comparison shows
33 tiles differ in the 0x6B80-0x6FC0 region. The data is the same art but
**nibble-shifted** — pixels are offset, indicating tiles loaded at wrong VRAM
addresses.

PLC state comparison at the same gameplay point (x=362 in GHZ Act 1):

| Metric | Native | Interpreter |
|--------|--------|------------|
| PLC patterns left (start) | 0x09B0 (2480) | 0x02D8 (728) |
| Tile 876 | Has data | All zeros (not yet loaded) |

The native build processes PLC tiles **faster** than the interpreter. It's ahead
in the decompression queue, causing tiles from future PLC loads to overwrite
flower tile slots before the flowers are properly loaded.

### Mechanism

1. `ProcessDPLC_3Tiles` (called from VBla_08) decompresses 3 tiles per frame
2. `ProcessDPLC_9Tiles` (called from VBla_04/0C) decompresses 9 tiles per frame
3. The VBlank handler's cycle timing differs between native and interpreter
4. Native fires VBlank at slightly different points, causing PLC to process
   more/fewer tiles per frame, accumulating a phase offset
5. By GHZ gameplay, the PLC queue is out of sync — tiles land in wrong VRAM slots

### Fix Applied: VDP Access State Save/Restore

`glue_check_vblank()` now saves and restores the VDP access state (write
address, code register, auto-increment, selected buffer, write pending flag)
around the VBlank handler call. This prevents the handler's VDP writes from
corrupting the auto-increment pointer when VBlank fires mid-tile-write.

VRAM comparison shows 16 of 33 differing tiles were fixed. Remaining 17 diffs
may be animation phase differences or PLC timing offsets.

### Alternative Fix Strategies (for reference)

The PLC timing depends on how many VBlank frames occur during level loading.
If native runs more VBlanks during loading (because game code runs faster than
expected), more PLC tiles get processed before gameplay starts.

Possible fixes:
1. **Align VBlank count** during level loading to match interpreter
2. **Process all PLC tiles at once** during level init (bypass incremental loading)
3. **Fix cycle accounting** so VBlank fires at the exact right cadence

This is likely related to the audio timing issue (same cycle counter drift).

### Files Changed

| File | Change |
|------|--------|
| `genesisrecomp/recompiler/src/code_generator.c` | Pre-scan for sp adjust, emit _sp_popped tracking |
| `genesisrecomp/runner/include/genesis_runtime.h` | Add g_early_return declaration (retained for future use) |
| `sonicthehedgehog/generated/sonic_full.c` | Regenerated with _sp_popped in 11 functions |
| `sonicthehedgehog/runner/glue.c` | g_early_return definition, IO port logging |
| `sonicthehedgehog/runner/main.c` | set_input wiring, input callback logging |
| `sonicthehedgehog/runner/cmd_server.c/h` | TCP debug server (new) |
| `sonicthehedgehog/tools/*.py` | Debug client and test scripts (new) |
