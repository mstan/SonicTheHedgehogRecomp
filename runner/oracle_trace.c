/*
 * oracle_trace.c — Tier-3 pre-instruction capture ring.
 *
 * Hot path: t3_pre_insn(pc) is called by clown68000 before every
 * instruction. It snapshots (pc, D, A, SR, frame) into the ring when
 * a configured PC range matches, then chains to whatever pre-insn
 * fn was installed before us (typically hybrid_pre_insn). Chaining
 * means --mem-write-log and Tier 2's hybrid dispatch continue to work
 * while Tier 3 is recording.
 *
 * Only compiled on SONIC_REVERSE_DEBUG=ON oracle builds. On native,
 * the interpreter isn't running (stub replaces DoCycles) so there's
 * nothing to hook.
 */

#if SONIC_REVERSE_DEBUG && defined(SONIC_ORACLE_BUILD)

#include "oracle_trace.h"
#include "cmd_server.h"
#include "clownmdemu.h"
#include "clown68000.h"
#include <stdio.h>
#include <string.h>

extern ClownMDEmu g_clownmdemu;   /* defined in main.c */

/* Hook slot on the interpreter side. Declared in runner/hybrid_global.c
 * and dereferenced from inside clown68000.c's per-instruction loop. */
extern void (*g_hybrid_pre_insn_fn)(cc_u32l pc);

#define T3_LOG_SIZE    (1u << 20)   /* 1,048,576 entries ≈ 80 MB */
#define T3_MAX_RANGES  8

typedef struct {
    uint32_t frame;
    uint32_t pc;
    uint32_t d[8];
    uint32_t a[8];
    uint16_t sr;
    uint16_t _pad;
} T3Entry;

static struct {
    int      active;
    int      nranges;
    struct { uint32_t lo, hi; } ranges[T3_MAX_RANGES];
    uint32_t write_idx;
    uint32_t count;
    uint32_t snap_start;
    uint32_t snap_count;
    int      snap_active;
    int      hook_installed;
    void   (*prev_fn)(cc_u32l pc);
    T3Entry  log[T3_LOG_SIZE];
} s_t3 = {0};

/* ---- Hot path ---- */

static int t3_range_hit(uint32_t pc)
{
    if (s_t3.nranges == 0) return 1;   /* no filter == log all */
    for (int i = 0; i < s_t3.nranges; i++) {
        if (pc >= s_t3.ranges[i].lo && pc <= s_t3.ranges[i].hi) return 1;
    }
    return 0;
}

/* Stage C: unconditional per-instruction counter on oracle. Ticked
 * from t3_pre_insn which fires once per 68K instruction in the
 * clown68000 interpreter's main loop. Compared against native's
 * g_native_insn_count (ticked from generated C) to measure whether
 * the cap-mode tempo slowdown is from per-instruction cost inflation
 * or from extra instructions per game-frame. */
uint64_t g_oracle_insn_count = 0;

static void t3_pre_insn(cc_u32l pc)
{
    g_oracle_insn_count++;
    if (s_t3.active && t3_range_hit((uint32_t)pc)) {
        const Clown68000_State *st = &g_clownmdemu.m68k;
        uint32_t idx = s_t3.write_idx % T3_LOG_SIZE;
        T3Entry *e = &s_t3.log[idx];
        e->pc    = (uint32_t)pc;
        e->sr    = (uint16_t)st->status_register;
        e->frame = cmd_server_current_frame();
        for (int i = 0; i < 8; i++) {
            e->d[i] = (uint32_t)st->data_registers[i];
            e->a[i] = (uint32_t)st->address_registers[i];
        }
        s_t3.write_idx++;
        if (s_t3.count < T3_LOG_SIZE) s_t3.count++;
    }
    /* Chain: forward to whatever pre-insn fn was installed before us
     * (hybrid_pre_insn for native dispatch, mostly a no-op on oracle
     * because the hybrid table is empty there). */
    if (s_t3.prev_fn) s_t3.prev_fn(pc);
}

/* ---- Install / uninstall ---- */

void t3_install_hook(void)
{
    if (s_t3.hook_installed) return;
    s_t3.prev_fn = g_hybrid_pre_insn_fn;
    g_hybrid_pre_insn_fn = t3_pre_insn;
    s_t3.hook_installed = 1;
}

void t3_uninstall_hook(void)
{
    if (!s_t3.hook_installed) return;
    /* Only restore if we're still the head of the chain; otherwise
     * leave whoever installed on top of us in place. */
    if (g_hybrid_pre_insn_fn == t3_pre_insn)
        g_hybrid_pre_insn_fn = s_t3.prev_fn;
    s_t3.prev_fn = NULL;
    s_t3.hook_installed = 0;
}

/* ---- Control API ---- */

int t3_add_range(uint32_t lo, uint32_t hi)
{
    if (s_t3.nranges >= T3_MAX_RANGES) return 0;
    if (hi < lo) { uint32_t t = lo; lo = hi; hi = t; }
    s_t3.ranges[s_t3.nranges].lo = lo & 0xFFFFFFu;
    s_t3.ranges[s_t3.nranges].hi = hi & 0xFFFFFFu;
    s_t3.nranges++;
    s_t3.active = 1;
    return 1;
}

void t3_reset(void)
{
    s_t3.active = 0;
    s_t3.nranges = 0;
    s_t3.write_idx = 0;
    s_t3.count = 0;
    s_t3.snap_start = 0;
    s_t3.snap_count = 0;
    s_t3.snap_active = 0;
}

int t3_range_count(void) { return s_t3.nranges; }

int t3_range_get(int i, uint32_t *lo_out, uint32_t *hi_out)
{
    if (i < 0 || i >= s_t3.nranges) return 0;
    if (lo_out) *lo_out = s_t3.ranges[i].lo;
    if (hi_out) *hi_out = s_t3.ranges[i].hi;
    return 1;
}

void t3_snapshot_begin(void)
{
    if (s_t3.count < T3_LOG_SIZE)
        s_t3.snap_start = 0;
    else
        s_t3.snap_start = s_t3.write_idx % T3_LOG_SIZE;
    s_t3.snap_count  = s_t3.count;
    s_t3.snap_active = 1;
}

void t3_snapshot_end(void)   { s_t3.snap_active = 0; }
uint32_t t3_snapshot_count(void)
{
    return s_t3.snap_active ? s_t3.snap_count : 0;
}

int t3_format_entry(uint32_t i, char *buf, size_t buflen)
{
    if (!s_t3.snap_active || i >= s_t3.snap_count) return -1;
    uint32_t idx = (s_t3.snap_start + i) % T3_LOG_SIZE;
    const T3Entry *e = &s_t3.log[idx];
    int n = snprintf(buf, buflen,
        "{\"f\":%u,\"pc\":\"0x%06X\",\"sr\":\"0x%04X\","
         "\"D\":[%u,%u,%u,%u,%u,%u,%u,%u],"
         "\"A\":[%u,%u,%u,%u,%u,%u,%u,%u]}",
        (unsigned)e->frame,
        (unsigned)(e->pc & 0xFFFFFFu),
        (unsigned)e->sr,
        (unsigned)e->d[0], (unsigned)e->d[1],
        (unsigned)e->d[2], (unsigned)e->d[3],
        (unsigned)e->d[4], (unsigned)e->d[5],
        (unsigned)e->d[6], (unsigned)e->d[7],
        (unsigned)e->a[0], (unsigned)e->a[1],
        (unsigned)e->a[2], (unsigned)e->a[3],
        (unsigned)e->a[4], (unsigned)e->a[5],
        (unsigned)e->a[6], (unsigned)e->a[7]);
    return (n < 0 || (size_t)n >= buflen) ? -1 : n;
}

#endif /* SONIC_REVERSE_DEBUG && SONIC_ORACLE_BUILD */
