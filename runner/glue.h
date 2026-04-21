#ifndef GLUE_H
#define GLUE_H

#include "clownmdemu.h"

/* Call after ClownMDEmu_Initialise() and ROM load.
 * Stores the emulator pointer for m68k_read/write routing, and
 * (when ENABLE_RECOMPILED_CODE is set) starts the game thread. */
void glue_init(ClownMDEmu *emu, const cc_u8l *rom_bytes, cc_u32l rom_byte_len);

/* Called by stub_clown68000 when clownmdemu raises the VBlank interrupt
 * (level 6).  In Step 2, this signals the game thread to service VBlank. */
void glue_signal_vblank(void);

/* Called by stub_clown68000 when clownmdemu raises the HBlank interrupt
 * (level 4).  Currently a hint only; game thread polls g_vblank_pending. */
void glue_signal_hblank(void);

/* Called by stub_clown68000 to hand us the read/write callbacks that
 * clownmdemu set up for the current Clown68000_DoCycles call. */
void glue_set_callbacks(const void *callbacks);

/* Block until the game thread has finished servicing VBlank.
 * Called from the main loop after ClownMDEmu_Iterate(). */
void glue_wait_vblank_done(void);

/* Shutdown: signal game thread to stop (if running). */
void glue_shutdown(void);

/* Pacing mode selector.
 *   FIBER_FULL      — default; game fiber runs to WaitForVBlank per wall
 *                     frame at full host speed.  Heavy game frames get
 *                     multi-fire VBla; no hardware slowdown.
 *   CYCLE_ACCURATE  — EXPERIMENTAL: game fiber capped at NTSC wall-frame
 *                     cycle budget.  Exactly 1 VBla handler per wall
 *                     frame.  Currently produces noticeable tempo
 *                     slowdown because estimate_cycles in code_generator.c
 *                     is imprecise — not viable for playback yet.  Kept
 *                     as a research / regression-test substrate.
 *                     Opt-in via --pacing=accurate or debug.ini
 *                     pacing=accurate. */
typedef enum {
    GLUE_PACING_FIBER_FULL     = 0,
    GLUE_PACING_CYCLE_ACCURATE = 1,
} GluePacingMode;

extern GluePacingMode g_pacing_mode;

/* Per-wall-frame fire guarantee (CYCLE_ACCURATE mode only).  Called from
 * main.c AFTER glue_service_vblank each wall frame.  If the game fiber
 * didn't cross the VBla threshold and didn't call WaitForVBlank this
 * wall frame (e.g., boot ROM copy), force a fire so hardware's
 * 1-VBla-per-wall-frame invariant holds.  Also resets the per-wall-frame
 * fired latch either way. */
void glue_end_of_wall_frame(void);

#endif /* GLUE_H */
