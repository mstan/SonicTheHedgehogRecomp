/*
 * oracle_trace.h — Tier-3 reverse debugger: per-instruction CPU state
 * capture on the oracle (clown68000 interpreter) side.
 *
 * Design: chain into the existing g_hybrid_pre_insn_fn hook. Before
 * each 68K instruction the interpreter executes, snapshot
 * (pc, D[8], A[8], SR, frame) into a ring buffer, filtered by a
 * TCP-configurable set of PC ranges. Combined with Tier 2's native
 * block snapshots, this lets a Python tool pair (pc, nth-hit) entries
 * across targets and find the first 68K instruction where native and
 * oracle diverge during live gameplay.
 *
 * Double-gated: built only when SONIC_REVERSE_DEBUG is on AND we're in
 * the oracle build (SONIC_ORACLE_BUILD). The native build has no
 * interpreter (stub_clown68000.c stubs DoCycles), so a pre-instruction
 * hook has nothing to hook. All t3_* TCP handlers still compile on
 * native but reply with "oracle only" errors for a uniform surface.
 */
#pragma once

#include <stdint.h>
#include <stddef.h>

#if SONIC_REVERSE_DEBUG && defined(SONIC_ORACLE_BUILD)

/* Install / uninstall the pre-insn chain. install() must run AFTER the
 * existing hybrid_pre_insn installation in HybridInit — we save the
 * previous fn pointer and forward to it on every call. Idempotent. */
void t3_install_hook(void);
void t3_uninstall_hook(void);

/* Add a PC-range filter (inclusive). Activates capture on first call.
 * Returns 0 if the range table is full (max 8). */
int  t3_add_range(uint32_t lo, uint32_t hi);

/* Clear ring + ranges + deactivate. */
void t3_reset(void);

int      t3_range_count(void);
int      t3_range_get(int i, uint32_t *lo_out, uint32_t *hi_out);

/* Snapshot API for paged JSON dumps. During a snapshot, live captures
 * still enter the ring but aren't visible through format_entry, so
 * the paged dump is consistent across multiple TCP calls. */
void     t3_snapshot_begin(void);
void     t3_snapshot_end  (void);
uint32_t t3_snapshot_count(void);

/* Format entry at snapshot-index i as a JSON object into buf. Returns
 * bytes written or -1 on bad index / buffer overflow. Entry shape:
 *   {"f":N,"pc":"0xXXXXXX","sr":"0xXXXX",
 *    "D":[d0,...,d7],"A":[a0,...,a7]}
 * All register values are 32-bit unsigned, SR is 16-bit. */
int t3_format_entry(uint32_t i, char *buf, size_t buflen);

#endif /* SONIC_REVERSE_DEBUG && SONIC_ORACLE_BUILD */
