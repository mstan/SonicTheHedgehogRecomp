/*
 * glue.c — bridges genesis_runtime.h with clownmdemu-core.
 *
 * Step 1 (ENABLE_RECOMPILED_CODE not set):
 *   Provides all symbols required by genesis_runtime.h so the generated code
 *   links.  Memory functions call through clownmdemu's bus layer.  No game
 *   thread is started; the interpreter still drives 68K execution.
 *
 * Step 2 (ENABLE_RECOMPILED_CODE defined):
 *   Starts the game thread that calls func_000206() continuously.
 *   m68k_read/write go through clownmdemu's M68kReadCallback / M68kWriteCallback.
 *   VBlank is cooperative: main thread sets g_vblank_pending, game thread checks
 *   it at every memory access and calls service_vblank() when it fires.
 */

#include "glue.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <inttypes.h>

/* genesis_runtime.h interface */
#include "genesis_runtime.h"

/* clownmdemu bus layer */
#include "bus-main-m68k.h"
#include "bus-common.h"

/* clowncommon types */
#include "clowncommon.h"

#if HYBRID_RECOMPILED_CODE
#include "hybrid.h"
#include "verify.h"
#endif

/* =========================================================================
 * Global state required by genesis_runtime.h
 * ========================================================================= */

M68KState g_cpu;
uint8_t   g_rom[0x400000];   /* 4 MB ROM shadow — ROM bytes (big-endian, byte-addressed) */
uint8_t   g_ram[0x10000];    /* 64 KB work RAM shadow (not authoritative in Step 2) */

uint64_t  g_frame_count       = 0;
uint8_t   g_controller1_buttons = 0;
uint8_t   g_controller2_buttons = 0;

/* Contextual recompiler cycle tracking */
uint32_t  g_cycle_accumulator  = 0;
uint32_t  g_vblank_threshold   = 109312;  /* scanline 224 × 488 cycles */
/* NTSC wall-frame cycle budget — used in CYCLE_ACCURATE mode to cap
 * game-fiber work at hardware rate. 262 scanlines × 488 cycles each. */
#define NTSC_CYCLES_PER_WALL_FRAME 127856u
static int s_vblank_fired_this_frame = 0;

/* Pacing mode (see glue.h). Default stays FIBER_FULL because
 * CYCLE_ACCURATE measurement (Stage B, this branch) shows the cap
 * eliminates multi-fire (1.074 → 1.000 fires/wall) but also halves
 * native's FM-write count (~50% tempo slowdown). The cap mechanism
 * itself is correct; the slowdown means the cycle-cost signal still
 * has a bias somewhere. CYCLE_ACCURATE remains opt-in via
 * --pacing=accurate while we hunt the bias. */
GluePacingMode g_pacing_mode = GLUE_PACING_FIBER_FULL;

uint32_t  g_miss_count_any    = 0;
int       g_step2_active      = 0;  /* set to 1 in Step 2 mode */
uint32_t  g_miss_last_addr    = 0;
uint64_t  g_miss_last_frame   = 0;
uint32_t  g_miss_unique_addrs[MAX_MISS_UNIQUE];
int       g_miss_unique_count  = 0;

/* g_rte_pending via pointer indirection (see genesis_runtime.h).
 * During VBlank service, we redirect to s_rte_dummy so RTE propagation
 * inside the handler chain is suppressed — the handler's stack management
 * is handled by force-restoring A7. */
static int s_rte_real  = 0;
static int s_rte_dummy = 0;
int *g_rte_pending_ptr = &s_rte_real;

int       g_early_return      = 0;

int       g_dbg_b64_count     = 0;
int       g_dbg_b5e_count     = 0;
int       g_dbg_b88_count     = 0;

/* =========================================================================
 * Bus access watchdog
 *
 * Counts bus accesses between yields.  If the game fiber does > N million
 * accesses without calling glue_yield_for_vblank(), something is stuck in
 * an infinite loop.  We log full CPU state and exit cleanly instead of
 * hanging forever.
 * ========================================================================= */

#define WATCHDOG_LIMIT  10000000u  /* 10M bus ops ≈ way too many for one frame */
static uint32_t s_watchdog_counter = 0;

static void watchdog_check(uint32_t addr, int is_write, uint32_t val)
{
    if (++s_watchdog_counter != WATCHDOG_LIMIT)
        return;

    fprintf(stderr,
        "\n=== WATCHDOG: %u bus accesses without yield (frame %"PRIu64") ===\n"
        "  Last access: %s $%06X val=$%04X\n"
        "  D0=$%08X D1=$%08X D2=$%08X D3=$%08X\n"
        "  D4=$%08X D5=$%08X D6=$%08X D7=$%08X\n"
        "  A0=$%08X A1=$%08X A2=$%08X A3=$%08X\n"
        "  A4=$%08X A5=$%08X A6=$%08X A7=$%08X\n"
        "  SR=$%04X  rte_pending=%d\n"
        "=== Exiting to prevent hang ===\n",
        s_watchdog_counter, g_frame_count,
        is_write ? "WRITE" : "READ", addr, val,
        g_cpu.D[0], g_cpu.D[1], g_cpu.D[2], g_cpu.D[3],
        g_cpu.D[4], g_cpu.D[5], g_cpu.D[6], g_cpu.D[7],
        g_cpu.A[0], g_cpu.A[1], g_cpu.A[2], g_cpu.A[3],
        g_cpu.A[4], g_cpu.A[5], g_cpu.A[6], g_cpu.A[7],
        g_cpu.SR, g_rte_pending);

    /* Also dump the 68K stack (top 16 words) */
    {
        extern uint16_t m68k_read16(uint32_t);
        uint32_t sp = g_cpu.A[7] & 0xFFFFFFu;
        fprintf(stderr, "  Stack @$%06X:", sp);
        for (int i = 0; i < 16 && sp + i*2 < 0x1000000u; i++)
            fprintf(stderr, " %04X", m68k_read16(sp + i*2));
        fprintf(stderr, "\n");
    }

    exit(2);  /* clean exit with error code */
}

/* =========================================================================
 * Internal glue state
 * ========================================================================= */

static ClownMDEmu        *s_emu      = NULL;
static CPUCallbackUserData s_cpu_data;   /* passed to M68kReadCallback / M68kWriteCallback */

/* Bus cycle counter — declared in generated code or glue, used by clownmdemu sync */
extern cc_u32f g_hybrid_cycle_counter;

/* Reset bus sync state to frame start. Called at frame boundaries
 * so that cycle-based VDP/Z80/FM/PSG sync stays within one frame. */
void glue_reset_frame_sync(void)
{
    g_hybrid_cycle_counter = 0;
    s_cpu_data.sync.m68k.current_cycle = 0;
    s_cpu_data.sync.m68k.base_cycle = 0;
    /* Reset audio and IO sync states to match the cycle counter reset.
     * Without this, SyncCommon computes (0/divisor - old_value) = huge
     * negative delta (wraps unsigned), causing audio overgeneration. */
    s_cpu_data.sync.fm.current_cycle = 0;
    s_cpu_data.sync.psg.current_cycle = 0;
    s_cpu_data.sync.pcm.current_cycle = 0;
    s_cpu_data.sync.io_ports[0].current_cycle = 0;
    s_cpu_data.sync.io_ports[1].current_cycle = 0;
    s_cpu_data.sync.io_ports[2].current_cycle = 0;
}

/* Hybrid verifier sync snapshot/restore — saves s_cpu_data sync state */
#if HYBRID_RECOMPILED_CODE
static CPUCallbackUserData s_sync_snapshot;

void glue_snapshot_sync(void)
{
    memcpy(&s_sync_snapshot, &s_cpu_data, sizeof(s_cpu_data));
}

void glue_restore_sync(void)
{
    memcpy(&s_cpu_data, &s_sync_snapshot, sizeof(s_cpu_data));
}
#endif

/* =========================================================================
 * VBlank / single-threaded fiber sync (Step 2)
 *
 * Game code and VDP rendering alternate on the same thread using Windows
 * Fibers.  func_0029A8 (WaitForVBlank) yields to the main fiber; the main
 * loop runs Iterate + VBlank handlers, then resumes the game fiber.
 * No threads, no semaphores, no races.
 * ========================================================================= */

/* Re-entrancy guard (used by glue_check_vblank, must be in global scope) */
static int s_in_vblank_service = 0;

#if ENABLE_RECOMPILED_CODE

#include <windows.h>

/* func_000206 is the recompiled entry point declared in the generated headers */
void func_000206(void);
/* VBlank / HBlank interrupt handlers */
void func_000B10(void);   /* VBlank IRQ6 */
void func_001126(void);   /* HBlank IRQ4 */

void glue_log_frame_state(uint64_t frame);  /* defined below */

static LPVOID s_main_fiber = NULL;
static LPVOID s_game_fiber = NULL;
static int    s_game_running = 0;   /* 1 once the game fiber has started */

/* Game fiber entry point. */
static void CALLBACK game_fiber_func(LPVOID param)
{
    (void)param;
    g_cpu.A[7] =   ((uint32_t)g_rom[0] << 24)
                  | ((uint32_t)g_rom[1] << 16)
                  | ((uint32_t)g_rom[2] <<  8)
                  |  (uint32_t)g_rom[3];
    g_cpu.SR  = 0x2700u;

    func_000206();

    /* func_000206 should never return — it contains the main game loop.
     * If it does, just yield back to main forever. */
    fprintf(stderr, "[GAME] func_000206 returned unexpectedly!\n");
    for (;;) SwitchToFiber(s_main_fiber);
}

/* Scanline interleave state */
static int32_t s_cycle_budget = 0;
static int     s_game_yielded_vblank = 0;
#if SONIC_REVERSE_DEBUG
/* Tier-2 reverse debugger: set by glue_yield_for_break when the game
 * fiber parks at a block-entry hook. Main loop reads via
 * glue_game_yielded_for_break() after each SwitchToFiber return and
 * drains cmd_server until a resume command clears it. */
static int     s_game_yielded_break = 0;
#endif
static int     s_interleave_active = 0;

/* Called from DoCycles (inside Iterate). Runs game code for a chunk. */
static cc_u32f s_chunk_cycles = 0;  /* budget for current chunk */

void glue_run_game_chunk(cc_u32f cycles)
{
    if (!s_game_running || !s_game_fiber)
        return;
    if (s_game_yielded_vblank)
        return;
#if SONIC_REVERSE_DEBUG
    /* Tier 2: game fiber has parked at a breakpoint. Keep DoCycles
     * no-opping until Iterate returns to main.c, where rdb_park_drain
     * polls cmd_server until a resume command arrives. Without this
     * gate the yield is effectively a no-op — DoCycles would switch
     * right back into the game fiber on the next chunk. */
    if (s_game_yielded_break)
        return;
#endif

    s_chunk_cycles = cycles;
    s_cycle_budget = (int32_t)cycles;
    s_interleave_active = 1;
    SwitchToFiber(s_game_fiber);
    s_interleave_active = 0;
}

/* Called from bus access macro to check budget */
#define BUDGET_COST_PER_ACCESS 10
static void check_cycle_budget(void)
{
    if (s_interleave_active && !s_in_vblank_service) {
        s_cycle_budget -= BUDGET_COST_PER_ACCESS;
        if (s_cycle_budget <= 0) {
            /* Top up g_hybrid_cycle_counter to match the full chunk budget.
             * Bus accesses advanced it by N*CYCLES_PER_BUS_ACCESS, but
             * the budget was s_chunk_cycles.  Add the remainder. */
            g_hybrid_cycle_counter += (cc_u32f)(-s_cycle_budget);
            SwitchToFiber(s_main_fiber);
        }
    }
}

/* Called from Clown68000_Interrupt during Iterate when VBlank/HBlank fires. */
void glue_handle_interrupt(cc_u16f level)
{
    if (!s_game_running || !s_game_yielded_vblank)
        return;

    int imask = (g_cpu.SR >> 8) & 7;

    if (level == 6 && imask < 6) {
        /* VBlank interrupt — run handler with register save/restore */
        M68KState saved = g_cpu;

        #define INTR_STACK 0x00FFD000u
        #define INTR_SAVE 128
        cc_u16l intr_ram[INTR_SAVE];
        uint32_t base = ((INTR_STACK - INTR_SAVE * 2) & 0xFFFF) / 2;
        for (int i = 0; i < INTR_SAVE; i++)
            intr_ram[i] = s_emu->state.m68k.ram[base + i];

        g_cpu.A[7] = INTR_STACK;
        s_in_vblank_service = 1;
        g_rte_pending = 0;
        g_rte_pending_ptr = &s_rte_dummy;
        func_000B10();
        g_rte_pending_ptr = &s_rte_real;
        g_rte_pending = 0;
        s_in_vblank_service = 0;

        for (int i = 0; i < INTR_SAVE; i++)
            s_emu->state.m68k.ram[base + i] = intr_ram[i];

        g_cpu = saved;

        /* Wake game — it can now continue past WaitForVBlank */
        s_game_yielded_vblank = 0;
    }
    if (level == 4 && imask < 4) {
        /* HBlank — run with save/restore */
        M68KState saved = g_cpu;

        uint32_t base = ((INTR_STACK - INTR_SAVE * 2) & 0xFFFF) / 2;
        cc_u16l hbl_ram[INTR_SAVE];
        for (int i = 0; i < INTR_SAVE; i++)
            hbl_ram[i] = s_emu->state.m68k.ram[base + i];

        g_cpu.A[7] = INTR_STACK;
        s_in_vblank_service = 1;
        g_rte_pending = 0;
        g_rte_pending_ptr = &s_rte_dummy;
        func_001126();
        g_rte_pending_ptr = &s_rte_real;
        g_rte_pending = 0;
        s_in_vblank_service = 0;

        for (int i = 0; i < INTR_SAVE; i++)
            s_emu->state.m68k.ram[base + i] = hbl_ram[i];

        g_cpu = saved;
    }
}

/* Yield-site cycle-accumulator log.  Each line records the state of
 * g_cycle_accumulator at the moment the game fiber yields for VBlank.
 * This captures native's belief about how many 68K cycles it spent
 * executing since the last frame reset — including any sub-VBla work
 * before the game's own WaitForVBlank call.  Paired oracle run writes
 * equivalent data (interpreter master_cycle counter at same yield).
 * Column layout:
 *   frame cycle_acc v_vblank_count vbla_routine
 */
FILE *g_yield_log_file = NULL;
extern uint16_t m68k_read16(uint32_t);
extern uint8_t  m68k_read8(uint32_t);
extern uint32_t m68k_read32(uint32_t);

/* Called from func_0029A8: yield to main loop for one frame. */
void glue_yield_for_vblank(void)
{
    if (s_in_vblank_service)
        return;
    s_watchdog_counter = 0;
    if (g_yield_log_file) {
        uint32_t vbc = m68k_read32(0xFE0C);
        uint8_t  vr  = m68k_read8(0xF62A);
        fprintf(g_yield_log_file,
                "%llu %u %u %u\n",
                (unsigned long long)g_frame_count,
                g_cycle_accumulator,
                vbc,
                vr);
    }
    /* At yield, the game is inside WaitForVBlank (func_0029A8), which was
     * called via JSR from the game loop.  The expected A7 at yield is
     * initial_SSP - 4 (one return address pushed for the JSR to func_0029A8)
     * minus 4 more for the dispatch JSR = initial_SSP - 8 = $FFFDF8.
     * But A7 can drift above $FFFE00 if GM_Level's internal restart (goto
     * label_0037A2) bypasses the mode dispatch's A7 pop, accumulating +4
     * per restart.  Clamp to prevent stack/variable collision. */
    if (g_cpu.A[7] > 0x00FFFE00u)
        g_cpu.A[7] = 0x00FFFE00u;
    s_game_yielded_vblank = 1;
    SwitchToFiber(s_main_fiber);
    /* Resumed here when next frame's DoCycles calls glue_run_game_chunk.
     *
     * Simulate WaitForVBlank polling overhead: the real 68K spins in a
     * tst.b/bne.s loop (~18 cycles per iteration) until VBlank fires.
     * This consumes ~10,000-20,000 cycles of the frame budget.  Without
     * this penalty, the game gets extra cycles → runs too fast →
     * transitions too quick → more BSRs per frame than real hardware.
     *
     * Set accumulator to simulate that VBlank fired at scanline 224
     * and the game wasted cycles polling until the handler cleared $F62A. */
    /* If VBlank hasn't fired yet this frame (game yielded early, e.g. during
     * init when frames are very short), fire it now.  This matches the
     * interpreter where WaitForVBlank polls until VBlank fires — the game
     * doesn't continue until the handler has run.
     *
     * glue_check_vblank now requires accumulator >= threshold to fire, so
     * bump the accumulator just past threshold to force one fire here. */
    if (!s_vblank_fired_this_frame) {
        if (g_cycle_accumulator < g_vblank_threshold)
            g_cycle_accumulator = g_vblank_threshold;
        glue_check_vblank();
        s_vblank_fired_this_frame = 1;  /* ensure it's marked */
    }

    /* In FIBER_FULL: reset accumulator + fired flag per game-frame boundary.
     * In CYCLE_ACCURATE: accumulator tracks wall-frame budget (not game-
     * frame budget) — don't reset. s_vblank_fired_this_frame is a
     * per-wall-frame latch reset by glue_end_of_wall_frame. */
    if (g_pacing_mode == GLUE_PACING_FIBER_FULL) {
        g_cycle_accumulator = 0;
        s_vblank_fired_this_frame = 0;
    }

    /* PLC tile processing */
    {
        extern uint16_t m68k_read16(uint32_t);
        extern void func_001642(void);
        if (m68k_read16(0xFFF6F8) != 0) {
            M68KState plc_save = g_cpu;
            func_001642();
            g_cpu = plc_save;
        }
    }
}

/* Called from main loop: start the game frame. With interleave mode,
 * the game runs in small chunks during Iterate's DoCycles calls.
 * Without interleave, it runs until WaitForVBlank as before. */
void glue_run_game_frame(void)
{
    s_game_yielded_vblank = 0;
    /* Don't switch to game fiber here — DoCycles will do it during Iterate.
     * But we need to handle the first frame and any code that runs before
     * the first DoCycles call. */
}

/* Service VBlank: called from main loop AFTER Iterate.
 * Resumes game fiber so handlers + PLC run, then does joypad + bookkeeping. */
void glue_service_vblank(void)
{
    /* Handlers now fire from glue_check_vblank (contextual recompiler)
     * at the exact cycle count. No handler here — just bookkeeping. */

    s_game_yielded_vblank = 0;

    glue_reset_frame_sync();

    /* Joypad copy REMOVED — the VBlank handler's ReadJoypads handles
     * $F602/$F603 natively. Our manual copy was overwriting $F603
     * (pressed-this-frame) after the handler set it, causing a
     * one-frame delay in button edge detection. */

    glue_log_frame_state(g_frame_count);
    g_frame_count++;
}

#if SONIC_REVERSE_DEBUG
void glue_yield_for_break(void)
{
    /* Block-entry hook in the game fiber decided to park. We're at a
     * label boundary, so g_cpu and g_ram are consistent. Yield to main
     * fiber — main loop will drain cmd_server until a resume command
     * arrives, then SwitchToFiber(s_game_fiber) re-enters here and we
     * continue the interrupted function. Clear the flag on return so
     * the next yield can be detected. */
    s_game_yielded_break = 1;
    SwitchToFiber(s_main_fiber);
    s_game_yielded_break = 0;
}

int glue_game_yielded_for_break(void)
{
    return s_game_yielded_break;
}

void glue_resume_from_break(void)
{
    if (!s_game_running || !s_game_fiber) return;
    SwitchToFiber(s_game_fiber);
    /* Returns here when the game fiber yields again (any reason). */
}
#endif

#endif /* ENABLE_RECOMPILED_CODE */

/* In hybrid mode, VBlank is handled by the interpreter — yield is a no-op. */
#if !ENABLE_RECOMPILED_CODE
void glue_yield_for_vblank(void) { /* stub */ }
#if SONIC_REVERSE_DEBUG
/* Tier 2 is native-only. Oracle never enters recompiled C so block-entry
 * hooks don't fire, but the symbols must exist for reverse_debug.c to
 * link into both builds. */
void glue_yield_for_break(void) { /* native-only; unreachable from oracle */ }
int  glue_game_yielded_for_break(void) { return 0; }
void glue_resume_from_break(void) { /* native-only */ }
#endif
#endif

/* Hybrid dispatch is now handled via the pre-instruction hook in
 * clown68000.c.  See hybrid.c / HybridInit(). */

/* =========================================================================
 * glue_init / glue_signal_* / glue_wait_vblank_done / glue_shutdown
 * ========================================================================= */

void glue_init(ClownMDEmu *emu, const cc_u8l *rom_bytes, cc_u32l rom_byte_len)
{
    s_emu = emu;

    /* Build the CPUCallbackUserData clownmdemu expects */
    memset(&s_cpu_data, 0, sizeof(s_cpu_data));
    s_cpu_data.clownmdemu = emu;

    /* Wire up cycle_countdown pointers so Z80/DMA sync doesn't deref NULL */
    s_cpu_data.sync.z80.cycle_countdown          = &emu->state.z80.cycle_countdown;
    s_cpu_data.sync.vdp_dma_transfer.cycle_countdown =
        &emu->state.vdp_dma_transfer_countdown;

    /* Copy ROM bytes into g_rom so recompiled code can inspect ROM data
     * directly (e.g. tables copied from ROM to RAM at startup). */
    if (rom_bytes && rom_byte_len) {
        cc_u32l copy_len = rom_byte_len < sizeof(g_rom) ? rom_byte_len : sizeof(g_rom);
        memcpy(g_rom, rom_bytes, copy_len);
    }

#if HYBRID_RECOMPILED_CODE
    /* Install the pre-instruction dispatch hook. */
    HybridInit(emu);
    /* JMP table interpreter fallback. */
    {
        extern void hybrid_jmp_init(ClownMDEmu *emu, CPUCallbackUserData *cpu_data);
        hybrid_jmp_init(emu, &s_cpu_data);
    }
    VerifyInit(emu, &s_cpu_data);
#endif

#if ENABLE_RECOMPILED_CODE
    g_step2_active = 1;
    s_main_fiber = ConvertThreadToFiber(NULL);
    if (!s_main_fiber) {
        fprintf(stderr, "glue: ConvertThreadToFiber failed\n");
        return;
    }
    s_game_fiber = CreateFiber(0, game_fiber_func, NULL);
    if (!s_game_fiber) {
        fprintf(stderr, "glue: CreateFiber failed\n");
        return;
    }
    s_game_running = 1;
#endif
}

void glue_signal_vblank(void)
{
    /* In single-threaded Step 2, VBlank is delivered explicitly
     * by the main loop — this function is no longer needed. */
}

void glue_signal_hblank(void)
{
    /* HBlank is handled via the interrupt mask check inside service_vblank().
     * No additional signalling needed here. */
    (void)0;
}

/* Contextual recompiler: called from generated code when cycle accumulator
 * crosses the VBlank threshold.  Fires VBlank handler between instructions
 * on the game fiber — matching the interpreter's interrupt behavior.
 *
 * Previously gated by s_vblank_fired_this_frame, causing native to fire at
 * most ONE VBla per wall frame.  Measured consequence: heavy boot / init
 * blocks that execute N × threshold cycles of 68K work in a single wall
 * frame generated 1 VBla fire on native vs N fires on hardware/oracle,
 * making native's game_state-per-VBla-count overshoot.  ISSUE-003 round 6.
 *
 * New behavior: consume threshold-worth of cycles per fire.  Re-fire while
 * accumulator still has threshold-worth available.  s_in_vblank_service
 * prevents recursion from handler-internal accumulator crosses. */
uint64_t g_cvblank_fires_total = 0;

/* Fire the VBla handler once. Caller manages g_cycle_accumulator per
 * mode (FIBER_FULL subtracts threshold per fire; CYCLE_ACCURATE leaves
 * the accumulator running until the wall-frame cap is hit). The
 * Stage-A instrumentation hook records the fire for telemetry. */
static void fire_vblank_handler_once(void)
{
    s_vblank_fired_this_frame = 1;
    g_cvblank_fires_total++;
    { static int s_cv = 0; if (s_cv < 50) { s_cv++;
      fprintf(stderr, "[CVBLANK] fired at cycle %u (frame %"PRIu64") [#%llu]\n",
              g_cycle_accumulator, g_frame_count,
              (unsigned long long)g_cvblank_fires_total); } }

    int imask = (g_cpu.SR >> 8) & 7;
#if SONIC_REVERSE_DEBUG
    {
        extern void rdb_record_vbla_fire(uint32_t, uint64_t, int);
        rdb_record_vbla_fire(g_cycle_accumulator, g_frame_count,
            imask >= 6 ? 1 /*SUPPRESSED*/ : 0 /*THRESHOLD*/);
    }
#endif
    if (imask >= 6)
        return;  /* interrupts masked — cycles consumed, handler suppressed */

    M68KState saved = g_cpu;

    #define VBLK_STACK 0x00FFD000u
    #define VBLK_SAVE  128
    cc_u16l vblk_ram[VBLK_SAVE];
    uint32_t vbase = ((VBLK_STACK - VBLK_SAVE * 2) & 0xFFFF) / 2;
    for (int i = 0; i < VBLK_SAVE; i++)
        vblk_ram[i] = s_emu->state.m68k.ram[vbase + i];

    g_cpu.A[7] = VBLK_STACK;
    s_in_vblank_service = 1;
    g_rte_pending = 0;
    g_rte_pending_ptr = &s_rte_dummy;
    uint32_t acc_saved = g_cycle_accumulator;
    func_000B10();
    g_cycle_accumulator = acc_saved;
    g_rte_pending_ptr = &s_rte_real;
    g_rte_pending = 0;
    s_in_vblank_service = 0;

    for (int i = 0; i < VBLK_SAVE; i++)
        s_emu->state.m68k.ram[vbase + i] = vblk_ram[i];

    g_cpu = saved;
}

void glue_check_vblank(void)
{
    if (s_in_vblank_service)
        return;  /* already servicing — don't re-enter from handler's own accumulator */

    if (g_pacing_mode == GLUE_PACING_CYCLE_ACCURATE) {
        /* Accurate mode: fire once per wall frame at threshold crossing,
         * then cap game-fiber execution at NTSC_CYCLES_PER_WALL_FRAME —
         * yield to main at that point, matching hardware's wall-clock-
         * paced 68K execution. */
        if (!s_vblank_fired_this_frame &&
            g_cycle_accumulator >= g_vblank_threshold) {
            fire_vblank_handler_once();
        }
        if (g_cycle_accumulator >= NTSC_CYCLES_PER_WALL_FRAME) {
#if ENABLE_RECOMPILED_CODE
            /* Game has consumed a full NTSC frame's worth of cycles —
             * yield the fiber. Carry the excess over to next wall frame.
             * Native-only: oracle doesn't have a game fiber. */
            g_cycle_accumulator -= NTSC_CYCLES_PER_WALL_FRAME;
            s_game_yielded_vblank = 1;
            SwitchToFiber(s_main_fiber);
#endif
        }
        return;
    }

    /* FIBER_FULL mode: multi-fire while accumulator still over threshold. */
    while (g_cycle_accumulator >= g_vblank_threshold) {
        g_cycle_accumulator -= g_vblank_threshold;
        fire_vblank_handler_once();
    }
}

void glue_end_of_wall_frame(void)
{
    if (g_pacing_mode == GLUE_PACING_CYCLE_ACCURATE) {
        /* If nothing fired this wall frame (game didn't cross threshold
         * AND didn't call WaitForVBlank — e.g., boot ROM copy), force
         * one fire. Hardware fires VBla every wall frame regardless of
         * what the 68K is doing. */
        if (!s_vblank_fired_this_frame) {
            if (g_cycle_accumulator < g_vblank_threshold)
                g_cycle_accumulator = g_vblank_threshold;
            glue_check_vblank();
        }
    }
    /* Reset per-wall-frame latch. In FIBER_FULL this is also reset by
     * glue_yield_for_vblank; resetting here is idempotent and covers
     * non-yielding wall frames. */
    s_vblank_fired_this_frame = 0;
}

void glue_set_callbacks(const void *callbacks)
{
    /* The callbacks pointer that clownmdemu passed to Clown68000_DoCycles.
     * In Step 2 we use M68kReadCallback / M68kWriteCallback directly, so we
     * don't need these.  Stored for completeness. */
    (void)callbacks;
}

void glue_wait_vblank_done(void)
{
    /* In single-threaded Step 2, not needed — main loop drives everything. */
}

void glue_shutdown(void)
{
#if ENABLE_RECOMPILED_CODE
    if (s_game_fiber) {
        DeleteFiber(s_game_fiber);
        s_game_fiber = NULL;
    }
    /* s_main_fiber is the thread itself — ConvertFiberToThread to clean up. */
    if (s_main_fiber) {
        ConvertFiberToThread();
        s_main_fiber = NULL;
    }
    s_game_running = 0;
#endif
}

/* =========================================================================
 * Save state helpers — called from main.c F6/F7 handlers.
 * Saves/restores recompiled game state that lives outside ClownMDEmu.
 * ========================================================================= */

void glue_save_state(FILE *sf)
{
    fwrite(&g_cpu, 1, sizeof(g_cpu), sf);
    fwrite(&g_frame_count, 1, sizeof(g_frame_count), sf);
    fwrite(&g_cycle_accumulator, 1, sizeof(g_cycle_accumulator), sf);
    fwrite(&g_vblank_threshold, 1, sizeof(g_vblank_threshold), sf);
}

void glue_load_state(FILE *sf)
{
    fread(&g_cpu, 1, sizeof(g_cpu), sf);
    fread(&g_frame_count, 1, sizeof(g_frame_count), sf);
    fread(&g_cycle_accumulator, 1, sizeof(g_cycle_accumulator), sf);
    fread(&g_vblank_threshold, 1, sizeof(g_vblank_threshold), sf);
}

/* =========================================================================
 * Memory access — route through clownmdemu's bus layer (M68kReadCallback /
 * M68kWriteCallback), which handles ROM, work RAM, VDP, IO, Z80 bus, etc.
 * ========================================================================= */

/* Cycle counter for bus timing.
 *
 * clownmdemu's M68kRead/WriteCallback receives current_cycle (68K cycles)
 * and computes target_cycle = base_cycle + current_cycle * 7 (master cycles).
 * This drives VDP/Z80/FM/PSG sync — realistic timing is critical for:
 *   - DMA completion (collision data, art loading)
 *   - Z80 sound driver advancement (audio quality)
 *   - FM/PSG sample generation (audio timing)
 *
 * The 68K runs at ~7.67 MHz (master / 7).  A typical instruction takes
 * 4-20 cycles with ~1.5 bus accesses.  Average cycles per bus access ≈ 8.
 * We reset to 0 at frame boundaries so cycle values stay within one frame. */
/* Cycle tracking for clownmdemu sync timing.
 *
 * Iterate calls DoCycles(N) per scanline (~488 68K cycles).  We distribute
 * these cycles across bus accesses proportionally: each access advances
 * g_hybrid_cycle_counter by (budget / expected_accesses_per_chunk).
 *
 * With ~48 bus accesses per 488-cycle chunk (68K averages ~10 cycles per
 * access including non-bus instructions), we use budget/48 ≈ 10 per access.
 * This keeps g_hybrid_cycle_counter aligned with Iterate's scanline timing. */
#define CYCLES_PER_BUS_ACCESS 10u
#if ENABLE_RECOMPILED_CODE
#define HYBRID_BUMP_CYCLES() do { g_hybrid_cycle_counter += CYCLES_PER_BUS_ACCESS; check_cycle_budget(); } while(0)
#else
#define HYBRID_BUMP_CYCLES() do { g_hybrid_cycle_counter += CYCLES_PER_BUS_ACCESS; } while(0)
#endif

uint16_t m68k_read16(uint32_t byte_addr)
{
    byte_addr &= 0xFFFFFFu;
#if ENABLE_RECOMPILED_CODE
    watchdog_check(byte_addr, 0, 0);
#endif
    HYBRID_BUMP_CYCLES();
    return (uint16_t)M68kReadCallback(&s_cpu_data,
                                       byte_addr >> 1,
                                       cc_true, cc_true,
                                       g_hybrid_cycle_counter);
}

/* IO port access logging for joypad debugging */
int s_io_log_enabled = 0;  /* set via TCP command */
int s_io_log_count   = 0;

uint8_t m68k_read8(uint32_t byte_addr)
{
    byte_addr &= 0xFFFFFFu;
#if ENABLE_RECOMPILED_CODE
    watchdog_check(byte_addr, 0, 0);
    /* Z80 sync polls — the SMPS sound driver and Z80 bus arbiter both
     * require the Z80 to actually advance before responding. In native
     * mode the game fiber can run thousands of instructions without
     * yielding, so Z80 never gets cycles inside a polling loop.
     *
     * Old behavior: return constant 0 → 68K loop exits immediately,
     * but never actually waits for Z80 → SMPS commands queue up faster
     * than Z80 can drain them → notes drop ("squelching").
     *
     * New behavior: yield game fiber → Iterate's next DoCycles advances
     * Z80 by one scanline → resume → re-read the real Z80 RAM / bus
     * register. Loop self-paces against actual Z80 throughput.
     *
     * Bounded fallback: if a single read polls > 256 times without
     * resolving (corrupted Z80 state, dead driver), return 0 so we
     * don't hang. Counter resets across distinct read addresses. */
    static uint32_t last_poll_addr  = 0;
    static int      poll_streak     = 0;
    if (byte_addr == 0xA01FFDu || byte_addr == 0xA01FFFu || byte_addr == 0xA11100u) {
        if (s_interleave_active && !s_in_vblank_service) {
            if (byte_addr == last_poll_addr) {
                if (++poll_streak > 256) return 0x00u;
            } else {
                last_poll_addr = byte_addr;
                poll_streak    = 1;
            }
            SwitchToFiber(s_main_fiber);
            /* fall through to real read */
        } else {
            return 0x00u;  /* outside interleave: keep old shortcut */
        }
    } else {
        last_poll_addr = 0;
        poll_streak    = 0;
    }
#endif
    HYBRID_BUMP_CYCLES();
    cc_bool hi = (byte_addr & 1) == 0;
    cc_bool lo = !hi;
    cc_u16f word = M68kReadCallback(&s_cpu_data,
                                     byte_addr >> 1,
                                     hi, lo,
                                     g_hybrid_cycle_counter);
    uint8_t result = hi ? (uint8_t)(word >> 8) : (uint8_t)(word & 0xFF);
    if (s_io_log_enabled && byte_addr >= 0xA10000u && byte_addr <= 0xA1001Fu) {
        if (s_io_log_count < 200) {
            fprintf(stderr, "[IO-R] $%06X => 0x%02X (vblk=%d frame=%"PRIu64")\n",
                    byte_addr, result, s_in_vblank_service, g_frame_count);
            s_io_log_count++;
        }
    }
    return result;
}

uint32_t m68k_read32(uint32_t byte_addr)
{
    byte_addr &= 0xFFFFFFu;
#if ENABLE_RECOMPILED_CODE
    watchdog_check(byte_addr, 0, 0);
#endif
    /* Bump once for the whole 32-bit op — both halves at same cycle.
     * Prevents VDP/Z80 sync between the two 16-bit reads. */
    HYBRID_BUMP_CYCLES();
    uint16_t hi = (uint16_t)M68kReadCallback(&s_cpu_data,
                                              byte_addr >> 1,
                                              cc_true, cc_true,
                                              g_hybrid_cycle_counter);
    uint16_t lo = (uint16_t)M68kReadCallback(&s_cpu_data,
                                              (byte_addr + 2) >> 1,
                                              cc_true, cc_true,
                                              g_hybrid_cycle_counter);
    return ((uint32_t)hi << 16) | (uint32_t)lo;
}

void m68k_write16(uint32_t byte_addr, uint16_t val)
{
    byte_addr &= 0xFFFFFFu;
#if ENABLE_RECOMPILED_CODE
    watchdog_check(byte_addr, 1, val);
#endif
    HYBRID_BUMP_CYCLES();
    M68kWriteCallback(&s_cpu_data,
                       byte_addr >> 1,
                       cc_true, cc_true,
                       g_hybrid_cycle_counter, (cc_u16f)val);
}

void m68k_write8(uint32_t byte_addr, uint8_t val)
{
    byte_addr &= 0xFFFFFFu;
#if ENABLE_RECOMPILED_CODE
    watchdog_check(byte_addr, 1, val);
#endif
    if (s_io_log_enabled && byte_addr >= 0xA10000u && byte_addr <= 0xA1001Fu) {
        if (s_io_log_count < 200) {
            fprintf(stderr, "[IO-W] $%06X <= 0x%02X (vblk=%d frame=%"PRIu64")\n",
                    byte_addr, val, s_in_vblank_service, g_frame_count);
            s_io_log_count++;
        }
    }
    HYBRID_BUMP_CYCLES();
    cc_bool hi = (byte_addr & 1) == 0;
    cc_bool lo = !hi;
    /* Replicate the byte on both halves of the word */
    cc_u16f word = (cc_u16f)val | ((cc_u16f)val << 8);
    M68kWriteCallback(&s_cpu_data,
                       byte_addr >> 1,
                       hi, lo,
                       g_hybrid_cycle_counter, word);
}

void m68k_write32(uint32_t byte_addr, uint32_t val)
{
    byte_addr &= 0xFFFFFFu;
#if ENABLE_RECOMPILED_CODE
    watchdog_check(byte_addr, 1, (uint32_t)(val >> 16));
    /* Protect game variables at $FFFE00+: when the mode dispatch JSR
     * pushes a return address with A7 above the initial SSP ($FFFE00),
     * the low word lands at $FFFE02 (timer variable), causing false
     * level restarts.  Block writes to $FFFE00-$FFFE06. */
    if (byte_addr == 0xFFFE00u)
        return;  /* block JSR push that corrupts $FE02 timer */
#endif
    /* Bump once for the whole 32-bit op — both halves at same cycle.
     * Critical for VDP control port: the VDP latches a 32-bit command
     * from two consecutive 16-bit writes. If VDP sync runs between
     * them, the half-written command corrupts VDP state. */
    HYBRID_BUMP_CYCLES();
    M68kWriteCallback(&s_cpu_data,
                       byte_addr >> 1,
                       cc_true, cc_true,
                       g_hybrid_cycle_counter, (cc_u16f)(val >> 16));
    M68kWriteCallback(&s_cpu_data,
                       (byte_addr + 2) >> 1,
                       cc_true, cc_true,
                       g_hybrid_cycle_counter, (cc_u16f)(val & 0xFFFF));
}

/* =========================================================================
 * Dispatch
 * ========================================================================= */

#if ENABLE_RECOMPILED_CODE
static void log_true_miss(uint32_t target_pc);  /* forward decl — defined below */
#else
static void log_true_miss(uint32_t target_pc) { (void)target_pc; }
#endif

/* Check if addr falls inside an existing compiled function's range.
 * Uses the dispatch table exported by game_dispatch_get_table(). */
static int is_interior_label(uint32_t addr)
{
    /* game_dispatch_get_table returns a NULL-terminated array of
     * {addr, fn} pairs sorted by address.  Check if addr falls
     * between two consecutive entries. */
    extern int game_dispatch_table_size(void);
    extern uint32_t game_dispatch_table_addr(int i);

    int count = game_dispatch_table_size();
    if (count == 0) return 0;

    /* Binary search for the largest entry <= addr */
    int lo = 0, hi = count - 1;
    while (lo < hi) {
        int mid = (lo + hi + 1) / 2;
        if (game_dispatch_table_addr(mid) <= addr)
            lo = mid;
        else
            hi = mid - 1;
    }

    uint32_t func_start = game_dispatch_table_addr(lo);
    if (func_start == addr)
        return 0;  /* exact match = it IS a function, not interior */
    if (func_start < addr) {
        /* addr is between func_start and the next function — interior label */
        return 1;
    }
    return 0;
}

/* Is the 68K instruction at `addr` a `bra.w` trampoline?  bra.w is encoded
 * as 0x6000 <disp16>; it has zero prerequisite state and zero fall-through,
 * so seeding its address as an extra_func always produces a correct
 * single-tail-call function body.  This distinguishes real jmp-table
 * trampolines (which MUST be callable) from true interior labels (loop
 * tops, conditional-branch joins) that are never valid JSR targets. */
static int is_bra_w_trampoline(uint32_t addr)
{
    /* m68k_read16 goes through the bus callback so this works for ROM
     * (< $800000) as well as RAM. */
    uint16_t opcode = m68k_read16(addr);
    return opcode == 0x6000u;
}

void genesis_log_dispatch_miss(uint32_t addr)
{
    g_miss_count_any++;
    g_miss_last_addr  = addr;
    g_miss_last_frame = g_frame_count;

    /* Skip TRUE interior labels — these are valid JMP targets inside
     * existing functions, NOT missing function entry points.  EXCEPT:
     * if the bytes there are a bra.w trampoline, we almost certainly
     * hit a jmp-table entry that silently failed dispatch (ISSUE-003
     * class).  Fall through to log as a regular miss so the user can
     * seed it via extra_func.  Non-bra.w interior labels remain silent
     * — they would trigger the boundary-splitter and may need
     * hand-blacklisting, so humans decide. */
    if (is_interior_label(addr) && !is_bra_w_trampoline(addr))
        return;

    /* Skip out-of-ROM addresses */
    if (addr > 0x80000) return;

    /* Only process each unique address once */
    for (int i = 0; i < g_miss_unique_count; i++)
        if (g_miss_unique_addrs[i] == addr)
            return;  /* already reported */

    fprintf(stderr, "dispatch miss: $%06X (frame %" PRIu64 ")\n",
            addr, g_frame_count);

    if (g_miss_unique_count < MAX_MISS_UNIQUE)
        g_miss_unique_addrs[g_miss_unique_count++] = addr;

    /* Append to dispatch_misses.log (one address per line).
     * This file can be fed back to the recompiler via game.cfg extra_func. */
    extern const char *exe_relative(const char *);
    FILE *mf = fopen(exe_relative("dispatch_misses.log"), "a");
    if (mf) {
        fprintf(mf, "extra_func 0x%06X\n", addr);
        fclose(mf);
    }

    /* Also log to interp_fallbacks.log (same format, for convergence tools) */
    log_true_miss(addr);
}

/* NOTE: call_by_address() is implemented by sonic_dispatch.c (generated).
 * Do not define it here; it would conflict with the generated implementation. */

/* =========================================================================
 * VDP helpers (not called by generated code, provided for completeness)
 * ========================================================================= */

void     vdp_write_data(uint16_t val)   { m68k_write16(0xC00000, val); }
void     vdp_write_ctrl(uint16_t val)   { m68k_write16(0xC00004, val); }
uint16_t vdp_read_data(void)            { return m68k_read16(0xC00000); }
uint16_t vdp_read_status(void)          { return m68k_read16(0xC00004); }
void     vdp_render_frame(uint32_t *fb) { (void)fb; /* rendering via clownmdemu callbacks */ }

/* =========================================================================
 * Runtime init / VBlank request (old runner interface; not used by main.c)
 * ========================================================================= */

/* =========================================================================
 * Frame state logger — dumps key game state at each VBlank for comparison
 * between hybrid and Step 2 modes.
 * ========================================================================= */

static FILE *s_framelog = NULL;

void glue_log_frame_state(uint64_t frame)
{
    if (!s_framelog) {
#if ENABLE_RECOMPILED_CODE
        s_framelog = fopen("framelog_step2.txt", "w");
#else
        s_framelog = fopen("framelog_hybrid.txt", "w");
#endif
        if (!s_framelog) return;
    }
    if (frame > 9999) return;  /* cap framelog at 10000 frames */

    /* Read directly from clownmdemu's RAM (word-addressed, big-endian).
     * This avoids triggering SyncM68k in hybrid mode. */
    #define EMU_RAM_BYTE(addr) \
        ((uint8_t)(s_emu->state.m68k.ram[((addr) & 0xFFFF) / 2] >> \
                   (((addr) & 1) ? 0 : 8)))
    #define EMU_RAM_WORD(addr) \
        ((uint16_t)(s_emu->state.m68k.ram[((addr) & 0xFFFF) / 2]))
    #define EMU_RAM_LONG(addr) \
        (((uint32_t)EMU_RAM_WORD(addr) << 16) | EMU_RAM_WORD((addr)+2))

    uint8_t  game_mode = EMU_RAM_BYTE(0xF600);
    uint8_t  vbl_flag  = EMU_RAM_BYTE(0xF62A);
    uint16_t vbl_count = EMU_RAM_WORD(0xF628);
    uint16_t scroll_x  = EMU_RAM_WORD(0xF700);
    uint16_t plc_ptr   = EMU_RAM_WORD(0xF680);
    uint32_t frame_cnt = EMU_RAM_LONG(0xFE0C);
    uint8_t  obj0_id   = EMU_RAM_BYTE(0xD000);
    uint8_t  obj0_rt   = EMU_RAM_BYTE(0xD001);

    fprintf(s_framelog,
            "F%03llu mode=%02X vbl=%02X cnt=%04X scrl=%04X plc=%04X "
            "fcnt=%08X obj0=%02X/%02X\n",
            (unsigned long long)frame,
            game_mode, vbl_flag, vbl_count, scroll_x, plc_ptr,
            frame_cnt, obj0_id, obj0_rt);
    fflush(s_framelog);
}

void runtime_init(void)             { /* nothing; glue_init() serves this role */ }
void runtime_request_vblank(void)   { glue_signal_vblank(); }

/* =========================================================================
 * Logger helper
 * ========================================================================= */

void log_on_change(const char *label, uint32_t value)
{
    static uint32_t prev = ~0u;
    static const char *prev_label = NULL;
    if (prev_label != label || prev != value) {
        fprintf(stderr, "LOG %s = $%08X\n", label, value);
        prev_label = label;
        prev = value;
    }
}

/* =========================================================================
 * Step 2: hybrid_jmp_interpret / hybrid_call_interpret → call_by_address
 *
 * In hybrid mode these run the interpreter as a fallback.  In Step 2 there
 * is no interpreter — redirect to call_by_address() which has every
 * generated function in its dispatch table.
 * ========================================================================= */

#if ENABLE_RECOMPILED_CODE

extern void call_by_address(uint32_t addr);

/* Track indirect dispatch calls.
 * These go through hybrid_jmp/call_interpret → call_by_address.
 * We only log addresses that FAIL dispatch (true misses that need
 * new extra_func entries).  Addresses that dispatch successfully
 * are interior labels of existing functions — logging them would
 * cause the recompiler to split functions incorrectly. */
#define MAX_INTERP_SEEN 1024
static uint32_t s_interp_seen[MAX_INTERP_SEEN];
static int      s_interp_seen_count = 0;
int             g_interp_total_calls = 0;

/* Called from genesis_log_dispatch_miss — these are REAL misses */
static void log_true_miss(uint32_t target_pc)
{
    for (int i = 0; i < s_interp_seen_count; i++)
        if (s_interp_seen[i] == target_pc) return;
    if (s_interp_seen_count < MAX_INTERP_SEEN)
        s_interp_seen[s_interp_seen_count++] = target_pc;
    extern const char *exe_relative(const char *);
    FILE *f = fopen(exe_relative("interp_fallbacks.log"), "a");
    if (f) { fprintf(f, "extra_func 0x%06X\n", target_pc); fclose(f); }
}

int glue_interp_seen_count(void) { return s_interp_seen_count; }
int glue_interp_total_calls(void) { return g_interp_total_calls; }
uint64_t glue_miss_count_any(void) { return (uint64_t)g_miss_count_any; }
uint32_t glue_interp_seen_addr(int i) {
    return (i >= 0 && i < s_interp_seen_count) ? s_interp_seen[i] : 0;
}

void hybrid_jmp_interpret(uint32_t target_pc)
{
    g_interp_total_calls++;
    call_by_address(target_pc);
    /* If call_by_address didn't find it, genesis_log_dispatch_miss
     * was called, which logs it as a true miss via log_true_miss. */
}

void hybrid_call_interpret(uint32_t target_pc)
{
    g_interp_total_calls++;
    call_by_address(target_pc);
}

#endif /* ENABLE_RECOMPILED_CODE */
