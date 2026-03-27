# Sonic 1 Static Recompiler — Claude Instructions

## RULE 0: Never Modify Generated Output

**sonic_full.c, sonic_dispatch.c, and ALL generated files are READ-ONLY.**

- Codegen bugs → fix in the recompiler (`F:/Projects/segagenesisrecomp/genesisrecomp/`)
- Game-specific patterns (VBlank yield) → game.cfg or runtime override in the runner
- If you think you need to edit generated output, STOP. Find the right layer.

## RULE 1: Always Regenerate After Recompiler Changes

Every recompiler change: **change → build recompiler → regenerate → diff → rebuild game → test.**
Never commit recompiler changes without regenerated + tested output.

## Active Mode: Step 2 Standalone

Branch `step2-standalone`. All 68K execution is native (no interpreter).

### Build (Step 2)

```
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 -DENABLE_RECOMPILED_CODE=ON -DHYBRID_RECOMPILED_CODE=OFF
cmake --build build --config Release
```

Test: `build\Release\SonicTheHedgehogRecomp.exe sonic.bin`

### Build (Interpreter baseline for comparison)

```
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 -DHYBRID_RECOMPILED_CODE=ON -DENABLE_RECOMPILED_CODE=OFF -DCLOWNMDEMU_SKIP_CLOWN68000_INTERPRETER=OFF
```
Then set `g_hybrid_table_size = 0` in `runner/hybrid_table.c`, build, run for ~15s.
Produces `framelog_hybrid.txt`. Restore table size after.

**Never create or use build_step0, build_hybrid, or any other build directory.**

## Debug Methodology: Iterative Fix Loop

**This is the core workflow. Follow it exactly.**

### 1. Run Step 2, capture failure point
```
timeout 30 build/Release/SonicTheHedgehogRecomp.exe sonic.bin 2>&1 | tee step2_run.txt
```
Look for: dispatch misses, crashes, visual glitches, hangs. Note the frame number.

### 2. Run interpreter baseline at ±25 frames around failure
Build interpreter baseline (see above), run, get `framelog_hybrid.txt`.
```
python3 compare_frames.py --context 25
```
The comparison aligns by frame number. For deeper analysis, align by `fcnt` field
(game frame counter) since interpreter init takes ~59 extra VDP frames.

### 3. Identify the divergence
- **Dispatch miss**: Add the address to `game.cfg` as `extra_func`, regenerate
- **Wrong game state**: Diff the fields (mode, fcnt, scrl, obj0) to find which
  function is computing wrong values. Check if it's a codegen bug.
- **Crash/garbage addresses**: Usually stack corruption from missing fall-through
  or tail duplication. Check the last good function in the call chain.

### 4. Fix in the right layer
- **Codegen bug** → fix in recompiler (`genesisrecomp/recompiler/src/code_generator.c`)
- **Missing function** → add `extra_func` to `game.cfg`
- **Runtime bug** → fix in `runner/glue.c`, `runner/main.c`, etc.

### 5. Regenerate → rebuild → test
```bash
# Build recompiler
cd F:/Projects/segagenesisrecomp/genesisrecomp && cmake --build build/recompiler --config Release

# Regenerate
cd F:/Projects/segagenesisrecomp/sonicthehedgehog
../genesisrecomp/build/recompiler/Release/GenesisRecomp.exe \
  "F:/Projects/segagenesisrecomp-v2/sonicthehedgehog/sonic.bin" --game game.cfg

# Rebuild game
cd F:/Projects/segagenesisrecomp-v2/sonicthehedgehog
cmake --build build --config Release

# Test
timeout 30 build/Release/SonicTheHedgehogRecomp.exe sonic.bin
```

### 6. Commit and repeat
Commit recompiler fix, regenerated output, and game.cfg changes separately.
Then go back to step 1.

## Recompiler Paths

| What | Path |
|------|------|
| Recompiler source | `F:/Projects/segagenesisrecomp/genesisrecomp/recompiler/src/` |
| Recompiler binary | `F:/Projects/segagenesisrecomp/genesisrecomp/build/recompiler/Release/GenesisRecomp.exe` |
| Game config | `F:/Projects/segagenesisrecomp/sonicthehedgehog/game.cfg` |
| Annotations | `F:/Projects/segagenesisrecomp/sonicthehedgehog/annotations_from_disasm.csv` |
| Generated output | `F:/Projects/segagenesisrecomp/sonicthehedgehog/generated/` |
| ROM | `F:/Projects/segagenesisrecomp-v2/sonicthehedgehog/sonic.bin` |

## Key Files (v2 runner)

| File | Purpose |
|------|---------|
| `runner/main.c` | SDL2 host, 60 Hz fiber loop |
| `runner/glue.c` | m68k_read/write → clownmdemu bus; fiber yield; VBlank service |
| `runner/stub_clown68000.c` | Replaces clown68000 interpreter for Step 2 |
| `runner/hybrid_table.c` | Hybrid dispatch table (used for interpreter baseline) |
| `compare_frames.py` | Frame log comparison tool |

## Architecture (Step 2)

Single-threaded fiber model:
1. `glue_run_game_frame()` → SwitchToFiber(game) → native 68K code runs
2. Game calls `glue_yield_for_vblank()` → SwitchToFiber(main) → returns
3. `ClownMDEmu_Iterate()` → VDP renders frame, generates audio
4. `glue_service_vblank()` → checks imask, calls VBlank/HBlank handlers
5. Back to step 1

337 generated functions. All JMP table targets in game.cfg as extra_func.
Zero dispatch misses as of 2026-03-21.

## FIXED BUGS (2026-03-23, branch experiment-vblank-reorder)

1. **Dark palettes** — VBlank handler MOVEM pop clobbered D0-D7/A0-A6.
   Fix: save/restore full M68KState around handler calls.
2. **PLC softlock** — NemDec tile processing needed game fiber context.
   Fix: call func_001642 from glue_yield_for_vblank with register save/restore.
3. **Jump → restart loop** — Stack overflow writes corrupted $FE02 timer.
   Fix: block write32 at $FFFE00 + clamp A7 at yield point.

## OPEN BUG: Jump has no height (2026-03-24)

**ROOT CAUSE IDENTIFIED: VBlank must interrupt between Sonic_Jump and Sonic_Move.**

In the interpreter, 68K execution interleaves with VDP scanlines. VBlank fires
as an interrupt BETWEEN Sonic_Jump (which sets yvel=$F98D) and Sonic_Move (which
overwrites yvel=0). The interrupt runs the VBlank handler, then the NEXT frame
starts. The next frame dispatches to MdJump2 (which preserves yvel).

In native Step 2, the game runs atomically: Sonic_Jump → Sonic_Move both execute
in one uninterrupted block. Sonic_Move overwrites yvel=0 because:
- ground_speed ($D014) is 0 (Sonic_Jump doesn't modify it)
- yvel = cos(angle) × 0 >> 8 = ALWAYS 0
- No path in Sonic_Move skips this write ($F7CA=0 in GHZ)

The fix requires VBlank to fire INSIDE game code at the right scanline, between
Sonic_Jump and Sonic_Move. The scanline-interleave branch's DoCycles gives game
code ~488 cycles per chunk. But running the handler inside Iterate (via
Clown68000_Interrupt) caused a black screen due to dual cycle counters:
- Iterate uses its own internal CPUCallbackUserData (local variable)
- Game code uses s_cpu_data (global in glue.c)
- Handler bus writes use s_cpu_data but Iterate expects its local state

**The solution: make Clown68000_Interrupt properly fire the handler during Iterate
without conflicting with Iterate's cycle accounting.**

### Failed approaches (all documented, do not retry):
1. Cycle increment (8/10/20 per bus access) — no effect
2. No sync reset — no effect
3. Handlers on game fiber — no effect on drift
4. Natural MOVEM (no C save/restore) — broke palette fades
5. DoCycles advancing Z80/FM/PSG — no effect
6. Iterate-first ordering — no effect
7. Handler inside Iterate via Clown68000_Interrupt — black screen (dual counters)
8. Game fiber resume in service_vblank — 2x speed
9. $FFFE00 write protection removal — restart loop returned
