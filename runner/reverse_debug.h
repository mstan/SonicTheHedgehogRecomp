/*
 * reverse_debug.h — Tier-1 reverse-debugger: ring buffer + bus tap.
 *
 * Design (simpler than the snesrecomp equivalent because 68K has a bus):
 *   - Generator emits `g_rdb_current_func = 0xXXXXXXu;` at the top of
 *     every func_XXXXXX body when --reverse-debug is on.
 *   - reverse_debug.c installs itself as g_mem_write_trace_fn (the
 *     shared bus-write callback that clownmdemu-core fires on every
 *     M68kWriteCallback).
 *   - Every bus write is recorded with (frame, adr, val, width, func,
 *     caller_return_addr), filtered by up to 8 configurable ranges.
 *
 * This works in BOTH the native build (recompiled C drives the bus) and
 * the oracle build (clown68000 interpreter drives the bus). In the
 * oracle build g_rdb_current_func stays 0 because no recompiled function
 * bodies run; the caller column (ret0 from A7) still gives useful
 * attribution against SMWDisX-equivalent disassembly.
 *
 * When SONIC_REVERSE_DEBUG is not defined, this header is empty and
 * nothing is linked in.
 */
#pragma once

#include <stdint.h>
#include <stddef.h>

#if SONIC_REVERSE_DEBUG

/* Updated at the start of every recompiled func_XXXXXX body (generator
 * writes `g_rdb_current_func = 0xAABBCCu;` as the first line of each
 * function). Read by the bus-tap callback at record time to attribute
 * each write to its source 68K function. Stays 0 in oracle builds. */
extern uint32_t g_rdb_current_func;

/* ---- Control API (called from cmd_server.c dispatcher) ---- */

/* Register the Tier-1 bus tap with the shared write-trace hook slot in
 * bus-main-m68k.c. Chains any previously-installed callback so
 * --mem-write-log and rdb can coexist. Idempotent; safe to call more
 * than once. */
void rdb_install_bus_tap(void);

/* Remove the bus tap and restore the previous g_mem_write_trace_fn
 * (if any). Called from rdb_reset. */
void rdb_uninstall_bus_tap(void);

/* Add an address-range filter [lo,hi] (inclusive, 24-bit 68K bus addr).
 * Installs the bus tap on first call. Returns 0 if full (max 8). */
int  rdb_add_range(uint32_t lo, uint32_t hi);

/* Clear all ranges + ring + uninstall bus tap. */
void rdb_reset(void);

/* Snapshot API for paged dumps. During a snapshot the visible count is
 * frozen; new writes still append to the ring but aren't visible. */
void     rdb_snapshot_begin(void);
void     rdb_snapshot_end  (void);
uint32_t rdb_snapshot_count(void);

int rdb_range_count(void);
int rdb_range_get(int i, uint32_t *lo_out, uint32_t *hi_out);

/* Format the snapshot entry at index `i` as a JSON object into buf.
 * Returns bytes written or -1 on bad index / buffer overflow. */
int rdb_format_entry(uint32_t i, char *buf, size_t buflen);

#endif /* SONIC_REVERSE_DEBUG */
