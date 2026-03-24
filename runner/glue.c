/*
 * glue.c — Fiber-based bridge between recompiled 68K code and clownmdemu.
 *
 * This file connects the statically recompiled Sonic 1 code (which calls
 * m68k_read/write for bus access) to clownmdemu's emulator core (which
 * provides VDP rendering, Z80/FM/PSG audio, and I/O).
 *
 * Architecture: single-threaded Windows Fibers. The game code runs on a
 * game fiber; the main loop runs on the main fiber. DoCycles (called from
 * ClownMDEmu_Iterate) switches to the game fiber for each scanline's
 * worth of cycles. The game fiber yields back when it calls WaitForVBlank
 * (func_0029A8 -> glue_yield_for_vblank).
 *
 * RUNTIME WORKAROUNDS (6):
 *
 * 1. M68KState save/restore around VBlank/HBlank handlers
 *    HACK: The handler's MOVEM pop clobbers D0-D7/A0-A6. On real hardware,
 *    RTE restores the SR and PC from the stack, and the handler manages its
 *    own register saves. In our model, handlers are called as C functions
 *    that return normally — RTE propagation is suppressed, so registers
 *    aren't restored. We save/restore the full M68KState around each call.
 *    Discovered when palette fade counter D4 got corrupted ($15->$A881->$FFFF).
 *
 * 2. PLC tile processing in glue_yield_for_vblank
 *    HACK: Nemesis tile decompression (func_001642) must run from the game
 *    fiber context. The PLC queue is serviced during WaitForVBlank's yield.
 *    Without this, the PLC queue never drains and the game softlocks waiting
 *    for tile art to load.
 *
 * 3. $FFFE00 write32 protection
 *    HACK: When A7 is at the initial SSP ($FFFE00), a JSR pushes its return
 *    address to $FFFE00-$FFFE03, corrupting the game timer at $FE02. This
 *    causes a false level-restart loop after jumping. We block write32 at
 *    $FFFE00 to prevent the corruption.
 *
 * 4. A7 clamp at yield
 *    HACK: The game mode dispatch (GM_Level's internal restart via goto
 *    label_0037A2) bypasses the mode dispatch's A7 pop, causing A7 to drift
 *    above $FFFE00 by +4 per restart. We clamp A7 to $FFFE00 at each yield
 *    to prevent stack/variable collision.
 *
 * 5. Handler stack at $FFD000 + RAM save/restore
 *    HACK: The VBlank handler uses MOVEM to push registers to the stack.
 *    If the handler runs with A7 in the game's stack area, it overwrites
 *    the collision response buffer at $FFCFC4+. We redirect the handler's
 *    A7 to $FFD000 and save/restore 128 words of RAM around the call.
 *
 * 6. Contextual cycle tracking (g_cycle_accumulator / g_vblank_threshold)
 *    NOT A HACK — prod infrastructure. The recompiler emits cycle accumulation
 *    after every instruction (12,076 call sites in sonic_full.c). When the
 *    accumulator crosses the VBlank threshold (109312 = scanline 224 * 488),
 *    glue_check_vblank() fires the VBlank handler between instructions.
 *    This is the foundation for proper interrupt timing but does not yet
 *    resolve the jump-height bug. The mechanism fires correctly; the timing
 *    offset persists.
 */

#include "glue.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <inttypes.h>
#include <windows.h>

/* genesis_runtime.h interface */
#include "genesis_runtime.h"

/* clownmdemu bus layer */
#include "bus-main-m68k.h"
#include "bus-common.h"

/* clowncommon types */
#include "clowncommon.h"

/* =========================================================================
 * Global state required by genesis_runtime.h
 * ========================================================================= */

M68KState g_cpu;
uint8_t   g_rom[0x400000];   /* 4 MB ROM shadow (big-endian, byte-addressed) */
uint8_t   g_ram[0x10000];    /* 64 KB work RAM shadow (not authoritative — clownmdemu owns RAM) */

uint64_t  g_frame_count       = 0;
uint8_t   g_controller1_buttons = 0;
uint8_t   g_controller2_buttons = 0;

/* Contextual cycle tracking (see workaround #6 above) */
uint32_t  g_cycle_accumulator  = 0;
uint32_t  g_vblank_threshold   = 109312;  /* scanline 224 * 488 cycles */
static int s_vblank_fired_this_frame = 0;

/* g_rte_pending via pointer indirection (see genesis_runtime.h).
 * During VBlank service, we redirect to s_rte_dummy so RTE propagation
 * inside the handler chain is suppressed — the handler's stack management
 * is handled by force-restoring A7. */
static int s_rte_real  = 0;
static int s_rte_dummy = 0;
int *g_rte_pending_ptr = &s_rte_real;

/* Referenced by generated code (sonic_full.c) */
int       g_dbg_b64_count     = 0;
int       g_dbg_b5e_count     = 0;
int       g_dbg_b88_count     = 0;

/* Bus-access cycle counter (legacy name from hybrid mode).
 * Used by M68kRead/WriteCallback for VDP/Z80/FM/PSG sync timing. */
cc_u32f g_hybrid_cycle_counter = 0;

/* =========================================================================
 * Bus access watchdog
 *
 * Counts bus accesses between yields.  If the game fiber does > N million
 * accesses without calling glue_yield_for_vblank(), something is stuck in
 * an infinite loop.  We log full CPU state and exit cleanly instead of
 * hanging forever.
 * ========================================================================= */

#define WATCHDOG_LIMIT  10000000u  /* 10M bus ops ~ way too many for one frame */
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

    /* Dump the 68K stack (top 16 words) */
    {
        extern uint16_t m68k_read16(uint32_t);
        uint32_t sp = g_cpu.A[7] & 0xFFFFFFu;
        fprintf(stderr, "  Stack @$%06X:", sp);
        for (int i = 0; i < 16 && sp + i*2 < 0x1000000u; i++)
            fprintf(stderr, " %04X", m68k_read16(sp + i*2));
        fprintf(stderr, "\n");
    }

    exit(2);
}

/* =========================================================================
 * Internal glue state
 * ========================================================================= */

static ClownMDEmu        *s_emu      = NULL;
static CPUCallbackUserData s_cpu_data;

/* Reset bus sync state to frame start. Called at frame boundaries
 * so that cycle-based VDP/Z80/FM/PSG sync stays within one frame. */
void glue_reset_frame_sync(void)
{
    g_hybrid_cycle_counter = 0;
    s_cpu_data.sync.m68k.current_cycle = 0;
    s_cpu_data.sync.m68k.base_cycle = 0;
}

/* =========================================================================
 * Single-threaded fiber sync
 *
 * Game code and VDP rendering alternate on the same thread using Windows
 * Fibers.  func_0029A8 (WaitForVBlank) yields to the main fiber; the main
 * loop runs Iterate + VBlank handlers, then resumes the game fiber.
 * No threads, no semaphores, no races.
 * ========================================================================= */

/* func_000206 is the recompiled entry point declared in the generated headers */
void func_000206(void);
/* VBlank / HBlank interrupt handlers */
void func_000B10(void);   /* VBlank IRQ6 */
void func_001126(void);   /* HBlank IRQ4 */

static LPVOID s_main_fiber = NULL;
static LPVOID s_game_fiber = NULL;
static int    s_game_running = 0;

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

/* Re-entrancy guard */
static int s_in_vblank_service = 0;

/* Scanline interleave state */
static int32_t s_cycle_budget = 0;
static int     s_game_yielded_vblank = 0;
static int     s_interleave_active = 0;

/* Called from DoCycles (inside Iterate). Runs game code for a chunk. */
void glue_run_game_chunk(cc_u32f cycles)
{
    if (!s_game_running || !s_game_fiber)
        return;
    if (s_game_yielded_vblank)
        return;

    s_cycle_budget = (int32_t)cycles;
    s_interleave_active = 1;
    SwitchToFiber(s_game_fiber);
    s_interleave_active = 0;
}

/* Called from bus access macro to check budget */
static void check_cycle_budget(void)
{
    if (s_interleave_active && !s_in_vblank_service) {
        s_cycle_budget -= 10;  /* matches CYCLES_PER_BUS_ACCESS */
        if (s_cycle_budget <= 0) {
            SwitchToFiber(s_main_fiber);
        }
    }
}

/* --- Workaround #1 + #5: handler with register save/restore + redirected stack --- */

#define HANDLER_STACK 0x00FFD000u
#define HANDLER_SAVE  128  /* words of RAM to save/restore around handler */

static void run_handler_with_save(void (*handler)(void))
{
    M68KState saved = g_cpu;

    uint32_t base = ((HANDLER_STACK - HANDLER_SAVE * 2) & 0xFFFF) / 2;
    cc_u16l handler_ram[HANDLER_SAVE];
    for (int i = 0; i < HANDLER_SAVE; i++)
        handler_ram[i] = s_emu->state.m68k.ram[base + i];

    g_cpu.A[7] = HANDLER_STACK;
    s_in_vblank_service = 1;
    g_rte_pending = 0;
    g_rte_pending_ptr = &s_rte_dummy;

    handler();

    g_rte_pending_ptr = &s_rte_real;
    g_rte_pending = 0;
    s_in_vblank_service = 0;

    for (int i = 0; i < HANDLER_SAVE; i++)
        s_emu->state.m68k.ram[base + i] = handler_ram[i];

    g_cpu = saved;
}

/* Called from Clown68000_Interrupt during Iterate when VBlank/HBlank fires. */
void glue_handle_interrupt(cc_u16f level)
{
    if (!s_game_running || !s_game_yielded_vblank)
        return;

    int imask = (g_cpu.SR >> 8) & 7;

    if (level == 6 && imask < 6) {
        run_handler_with_save(func_000B10);
        s_game_yielded_vblank = 0;
    }
    if (level == 4 && imask < 4) {
        run_handler_with_save(func_001126);
    }
}

/* --- Workaround #2 + #4: yield with PLC processing + A7 clamp --- */

/* Called from func_0029A8: yield to main loop for one frame. */
void glue_yield_for_vblank(void)
{
    if (s_in_vblank_service)
        return;
    s_watchdog_counter = 0;

    /* Workaround #4: A7 clamp.
     * A7 can drift above $FFFE00 if GM_Level's internal restart bypasses
     * the mode dispatch's A7 pop, accumulating +4 per restart. */
    if (g_cpu.A[7] > 0x00FFFE00u)
        g_cpu.A[7] = 0x00FFFE00u;

    s_game_yielded_vblank = 1;
    SwitchToFiber(s_main_fiber);

    /* Resumed here when next frame's DoCycles calls glue_run_game_chunk. */

    /* If VBlank hasn't fired yet this frame (game yielded early, e.g. during
     * init when frames are very short), fire it now. */
    if (!s_vblank_fired_this_frame) {
        glue_check_vblank();
        s_vblank_fired_this_frame = 1;
    }

    /* Reset for next game frame's VBlank cycle check */
    g_cycle_accumulator = 0;
    s_vblank_fired_this_frame = 0;

    /* Workaround #2: PLC tile processing.
     * NemDec (func_001642) must run from game fiber context. */
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

/* Called from main loop: prepare for the game frame. With interleave mode,
 * the game runs in small chunks during Iterate's DoCycles calls. */
void glue_run_game_frame(void)
{
    s_game_yielded_vblank = 0;
}

/* Service VBlank: called from main loop AFTER Iterate. Bookkeeping only —
 * handlers now fire from glue_check_vblank (contextual recompiler). */
void glue_service_vblank(void)
{
    s_game_yielded_vblank = 0;
    glue_reset_frame_sync();
    g_frame_count++;
}

/* =========================================================================
 * Contextual cycle-based VBlank (workaround #6)
 *
 * Called from generated code when g_cycle_accumulator crosses
 * g_vblank_threshold. Fires VBlank handler between instructions on
 * the game fiber — matching the interpreter's interrupt behavior.
 * ========================================================================= */

void glue_check_vblank(void)
{
    if (s_vblank_fired_this_frame)
        return;
    s_vblank_fired_this_frame = 1;

    int imask = (g_cpu.SR >> 8) & 7;
    if (imask >= 6)
        return;  /* interrupts masked — VBlank suppressed */

    run_handler_with_save(func_000B10);
}

/* =========================================================================
 * glue_init / glue_signal_* / glue_shutdown
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
}

void glue_signal_vblank(void)
{
    /* In single-threaded fiber model, VBlank is delivered explicitly
     * by the main loop — this function is not needed. */
}

void glue_signal_hblank(void)
{
    /* HBlank is handled via the interrupt mask check in handlers.
     * No additional signalling needed. */
}

void glue_set_callbacks(const void *callbacks)
{
    (void)callbacks;
}

void glue_wait_vblank_done(void)
{
    /* In single-threaded model, not needed — main loop drives everything. */
}

void glue_shutdown(void)
{
    if (s_game_fiber) {
        DeleteFiber(s_game_fiber);
        s_game_fiber = NULL;
    }
    if (s_main_fiber) {
        ConvertFiberToThread();
        s_main_fiber = NULL;
    }
    s_game_running = 0;
}

/* =========================================================================
 * Memory access — route through clownmdemu's bus layer
 *
 * clownmdemu's M68kRead/WriteCallback receives current_cycle and computes
 * target_cycle = base_cycle + current_cycle * 7 (master cycles). This
 * drives VDP/Z80/FM/PSG sync.
 * ========================================================================= */

#define CYCLES_PER_BUS_ACCESS 10u
#define BUMP_CYCLES() do { g_hybrid_cycle_counter += CYCLES_PER_BUS_ACCESS; check_cycle_budget(); } while(0)

uint16_t m68k_read16(uint32_t byte_addr)
{
    byte_addr &= 0xFFFFFFu;
    watchdog_check(byte_addr, 0, 0);
    BUMP_CYCLES();
    return (uint16_t)M68kReadCallback(&s_cpu_data,
                                       byte_addr >> 1,
                                       cc_true, cc_true,
                                       g_hybrid_cycle_counter);
}

uint8_t m68k_read8(uint32_t byte_addr)
{
    byte_addr &= 0xFFFFFFu;
    watchdog_check(byte_addr, 0, 0);

    /* Z80 sound driver "ready" flag: the game polls Z80 RAM addresses
     * waiting for command acknowledgment. In native mode, the Z80 only
     * runs during Iterate(). Return 0 for known polling targets. */
    if (byte_addr == 0xA01FFDu || byte_addr == 0xA01FFFu)
        return 0x00u;
    /* Z80 bus grant shortcut: after requesting the bus ($A11100 write),
     * the polling loop reads $A11100 until bit 0 is clear (granted).
     * Return "granted" immediately. */
    if (byte_addr == 0xA11100u)
        return 0x00u;

    BUMP_CYCLES();
    cc_bool hi = (byte_addr & 1) == 0;
    cc_bool lo = !hi;
    cc_u16f word = M68kReadCallback(&s_cpu_data,
                                     byte_addr >> 1,
                                     hi, lo,
                                     g_hybrid_cycle_counter);
    return hi ? (uint8_t)(word >> 8) : (uint8_t)(word & 0xFF);
}

uint32_t m68k_read32(uint32_t byte_addr)
{
    byte_addr &= 0xFFFFFFu;
    watchdog_check(byte_addr, 0, 0);
    BUMP_CYCLES();
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
    watchdog_check(byte_addr, 1, val);
    BUMP_CYCLES();
    M68kWriteCallback(&s_cpu_data,
                       byte_addr >> 1,
                       cc_true, cc_true,
                       g_hybrid_cycle_counter, (cc_u16f)val);
}

void m68k_write8(uint32_t byte_addr, uint8_t val)
{
    byte_addr &= 0xFFFFFFu;
    watchdog_check(byte_addr, 1, val);
    BUMP_CYCLES();
    cc_bool hi = (byte_addr & 1) == 0;
    cc_bool lo = !hi;
    cc_u16f word = (cc_u16f)val | ((cc_u16f)val << 8);
    M68kWriteCallback(&s_cpu_data,
                       byte_addr >> 1,
                       hi, lo,
                       g_hybrid_cycle_counter, word);
}

void m68k_write32(uint32_t byte_addr, uint32_t val)
{
    byte_addr &= 0xFFFFFFu;
    watchdog_check(byte_addr, 1, (uint32_t)(val >> 16));

    /* Workaround #3: $FFFE00 write32 protection.
     * When A7 is at the initial SSP ($FFFE00), a JSR pushes its return
     * address to $FFFE00-$FFFE03, corrupting the game timer at $FE02. */
    if (byte_addr == 0xFFFE00u)
        return;

    BUMP_CYCLES();
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

void genesis_log_dispatch_miss(uint32_t addr)
{
    fprintf(stderr, "dispatch miss: $%06X (frame %" PRIu64 ")\n", addr, g_frame_count);
}

/* NOTE: call_by_address() is implemented by sonic_dispatch.c (generated). */

/* =========================================================================
 * VDP helpers
 * ========================================================================= */

void     vdp_write_data(uint16_t val)   { m68k_write16(0xC00000, val); }
void     vdp_write_ctrl(uint16_t val)   { m68k_write16(0xC00004, val); }
uint16_t vdp_read_data(void)            { return m68k_read16(0xC00000); }
uint16_t vdp_read_status(void)          { return m68k_read16(0xC00004); }
void     vdp_render_frame(uint32_t *fb) { (void)fb; }

/* =========================================================================
 * Stubs for genesis_runtime.h interface
 * ========================================================================= */

void runtime_init(void)             { /* glue_init serves this role */ }
void runtime_request_vblank(void)   { glue_signal_vblank(); }

/* In Step 2 there is no interpreter — redirect to call_by_address. */
extern void call_by_address(uint32_t addr);

void hybrid_jmp_interpret(uint32_t target_pc)
{
    call_by_address(target_pc);
}

void hybrid_call_interpret(uint32_t target_pc)
{
    call_by_address(target_pc);
}
