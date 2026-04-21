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

#if SONIC_REVERSE_DEBUG
/* Tier-2 reverse debugger: yield the game fiber for a breakpoint /
 * step. Called from rdb_on_block_slow when a block-entry hook decides
 * to park. Mechanism mirrors glue_yield_for_vblank — we SwitchToFiber
 * back to the main fiber, the main loop drains cmd_server until a
 * resume command arrives, then switches back and execution continues
 * from the same block-entry point. Single-threaded cooperative; no
 * locking, no deadlock risk. Native-only; not defined in oracle /
 * hybrid-only builds. */
void glue_yield_for_break(void);

/* True if the most recent SwitchToFiber-return was triggered by a
 * block-entry break (not by a VBlank yield). Checked by main.c after
 * ClownMDEmu_Iterate so it knows to run the park-drain loop instead
 * of servicing VBlank. Cleared when the game fiber resumes. */
int  glue_game_yielded_for_break(void);

/* Called from main.c's park-drain loop when a rdb_step/rdb_continue
 * TCP command arrives. Switches to the game fiber so it can continue
 * executing from the yield point in rdb_on_block_slow. Returns when
 * the fiber yields again (break, vblank, or cycle budget) or exits.
 * Native-only; oracle build has a stub. */
void glue_resume_from_break(void);
#endif

#endif /* GLUE_H */
