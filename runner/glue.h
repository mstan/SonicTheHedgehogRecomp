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

/* Frame loop functions (called from main.c each frame) */
void glue_reset_frame_sync(void);
void glue_run_game_frame(void);
void glue_service_vblank(void);

#endif /* GLUE_H */
