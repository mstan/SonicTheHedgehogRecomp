/*
 * stub_clown68000.c — replaces the clown68000 interpreter for native execution.
 *
 * When CLOWNMDEMU_SKIP_CLOWN68000_INTERPRETER is set, clownmdemu's Iterate()
 * still calls the Clown68000_* entry points for timing coordination. This
 * stub provides those symbols:
 *
 * - DoCycles: dispatches to glue_run_game_chunk(), which switches to the
 *   game fiber for each scanline's worth of cycles. This interleaves
 *   recompiled game code with VDP scanline rendering.
 *
 * - Interrupt: no-op. VBlank/HBlank handlers run at yield points via
 *   glue_check_vblank() and glue_handle_interrupt(), not inside Iterate.
 *   Running handlers inside Iterate caused a black screen due to dual
 *   cycle counters (Iterate's local vs glue's global).
 */

#include <stdio.h>
#include "clown68000.h"
#include "bus-main-m68k.h"
#include "glue.h"

void Clown68000_SetErrorCallback(
    void (*error_callback)(void *user_data, const char *format, va_list arg),
    const void *user_data)
{
    (void)error_callback;
    (void)user_data;
}

void Clown68000_Reset(Clown68000_State *state,
                      const Clown68000_ReadWriteCallbacks *callbacks)
{
    (void)state;
    (void)callbacks;
}

void Clown68000_DoCycles(Clown68000_State *state,
                         const Clown68000_ReadWriteCallbacks *callbacks,
                         cc_u32f cycles_to_do)
{
    CPUCallbackUserData *ud = (CPUCallbackUserData *)callbacks->user_data;
    ud->clownmdemu->state.m68k.frozen_by_dma_transfer = cc_false;
    ud->clownmdemu->state.z80.frozen_by_dma_transfer  = cc_false;

    glue_set_callbacks(callbacks);

    if (cycles_to_do > 0) {
        extern void glue_run_game_chunk(cc_u32f cycles);
        glue_run_game_chunk(cycles_to_do);
    }

    (void)state;
}

void Clown68000_Interrupt(Clown68000_State *state, cc_u16f level)
{
    (void)state;
    (void)level;
}
