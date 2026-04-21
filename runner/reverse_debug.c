/*
 * reverse_debug.c — Tier-1 reverse-debugger implementation.
 *
 * Only compiled when SONIC_REVERSE_DEBUG=1. The generator emits a
 * `g_rdb_current_func = 0xXXXXXXu;` assignment at the entry of every
 * recompiled function body; this file installs itself as the shared
 * bus-write callback (g_mem_write_trace_fn, defined in
 * clownmdemu-core/bus-main-m68k.c) and records every bus write with
 * (frame, adr, val, width, func, caller) attribution.
 *
 * Native + oracle share the same bus-callback path, so Tier 1 observes
 * both. Range filter is applied before recording so 1M-entry ring never
 * drowns on unfiltered traffic.
 */

#if SONIC_REVERSE_DEBUG

#include "reverse_debug.h"
#include "cmd_server.h"
#include <stdio.h>
#include <string.h>
#include <stdint.h>

/* Shared hook slot in clownmdemu-core/source/bus-main-m68k.c. It is
 * defined unconditionally there; null when nobody has installed a tap. */
extern void (*g_mem_write_trace_fn)(uint32_t byte_address, uint8_t value, uint32_t target_cycle);

/* ---- Ring + ranges ---- */

#define RDB_LOG_SIZE     (1u << 20)   /* 1,048,576 entries */
#define RDB_MAX_RANGES   8

uint32_t g_rdb_current_func = 0;

typedef struct {
    uint32_t frame;
    uint32_t adr;
    uint32_t val;     /* byte writes zero-extend into low 8 */
    uint32_t func;
    uint32_t caller;
    uint8_t  width;   /* bus tap fires per byte → width is always 1 today */
} RdbEntry;

static struct {
    int      active;
    int      nranges;
    struct { uint32_t lo, hi; } ranges[RDB_MAX_RANGES];
    uint32_t write_idx;
    uint32_t count;
    uint32_t snap_start;
    uint32_t snap_count;
    int      snap_active;
    int      tap_installed;
    /* Previous callback, so --mem-write-log and rdb can coexist. */
    void (*prev_trace_fn)(uint32_t, uint8_t, uint32_t);
    RdbEntry log[RDB_LOG_SIZE];
} s_rdb = {0};

/* ---- Hot path ---- */

static int rdb_range_hit(uint32_t adr)
{
    if (s_rdb.nranges == 0) return 1;
    for (int i = 0; i < s_rdb.nranges; i++) {
        if (adr >= s_rdb.ranges[i].lo && adr <= s_rdb.ranges[i].hi) return 1;
    }
    return 0;
}

static void rdb_bus_tap(uint32_t byte_address, uint8_t value, uint32_t target_cycle)
{
    /* Forward to previously-installed tracer (e.g. --mem-write-log) first
     * so we don't starve other instrumentation while Tier 1 is armed. */
    if (s_rdb.prev_trace_fn) s_rdb.prev_trace_fn(byte_address, value, target_cycle);

    if (!s_rdb.active) return;
    uint32_t adr = byte_address & 0xFFFFFFu;
    if (!rdb_range_hit(adr)) return;

    uint32_t idx = s_rdb.write_idx % RDB_LOG_SIZE;
    uint32_t a7  = cmd_server_current_a7();

    s_rdb.log[idx].frame  = cmd_server_current_frame();
    s_rdb.log[idx].adr    = adr;
    s_rdb.log[idx].val    = value;
    s_rdb.log[idx].width  = 1;
    s_rdb.log[idx].func   = g_rdb_current_func;
    s_rdb.log[idx].caller = cmd_server_stack_read32(a7);

    s_rdb.write_idx++;
    if (s_rdb.count < RDB_LOG_SIZE) s_rdb.count++;
}

/* ---- Bus-tap (de)install ---- */

void rdb_install_bus_tap(void)
{
    if (s_rdb.tap_installed) return;
    s_rdb.prev_trace_fn = g_mem_write_trace_fn;
    g_mem_write_trace_fn = rdb_bus_tap;
    s_rdb.tap_installed = 1;
}

void rdb_uninstall_bus_tap(void)
{
    if (!s_rdb.tap_installed) return;
    /* Only restore if we're still the current callback; if someone else
     * installed on top of us, leave the chain alone. */
    if (g_mem_write_trace_fn == rdb_bus_tap)
        g_mem_write_trace_fn = s_rdb.prev_trace_fn;
    s_rdb.prev_trace_fn = NULL;
    s_rdb.tap_installed = 0;
}

/* ---- Control API ---- */

int rdb_add_range(uint32_t lo, uint32_t hi)
{
    if (s_rdb.nranges >= RDB_MAX_RANGES) return 0;
    if (hi < lo) { uint32_t t = lo; lo = hi; hi = t; }
    s_rdb.ranges[s_rdb.nranges].lo = lo & 0xFFFFFFu;
    s_rdb.ranges[s_rdb.nranges].hi = hi & 0xFFFFFFu;
    s_rdb.nranges++;
    s_rdb.active = 1;
    rdb_install_bus_tap();
    return 1;
}

void rdb_reset(void)
{
    s_rdb.active = 0;
    s_rdb.nranges = 0;
    s_rdb.write_idx = 0;
    s_rdb.count = 0;
    s_rdb.snap_start = 0;
    s_rdb.snap_count = 0;
    s_rdb.snap_active = 0;
    rdb_uninstall_bus_tap();
}

int rdb_range_count(void) { return s_rdb.nranges; }

int rdb_range_get(int i, uint32_t *lo_out, uint32_t *hi_out)
{
    if (i < 0 || i >= s_rdb.nranges) return 0;
    if (lo_out) *lo_out = s_rdb.ranges[i].lo;
    if (hi_out) *hi_out = s_rdb.ranges[i].hi;
    return 1;
}

void rdb_snapshot_begin(void)
{
    if (s_rdb.count < RDB_LOG_SIZE)
        s_rdb.snap_start = 0;
    else
        s_rdb.snap_start = s_rdb.write_idx % RDB_LOG_SIZE;
    s_rdb.snap_count  = s_rdb.count;
    s_rdb.snap_active = 1;
}

void rdb_snapshot_end(void)  { s_rdb.snap_active = 0; }

uint32_t rdb_snapshot_count(void)
{
    return s_rdb.snap_active ? s_rdb.snap_count : 0;
}

int rdb_format_entry(uint32_t i, char *buf, size_t buflen)
{
    if (!s_rdb.snap_active || i >= s_rdb.snap_count) return -1;
    uint32_t idx = (s_rdb.snap_start + i) % RDB_LOG_SIZE;
    const RdbEntry *e = &s_rdb.log[idx];
    int n = snprintf(buf, buflen,
        "{\"f\":%u,\"adr\":\"0x%06X\",\"val\":\"0x%02X\",\"w\":%u,"
         "\"func\":\"0x%06X\",\"caller\":\"0x%06X\"}",
        (unsigned)e->frame,
        (unsigned)e->adr,
        (unsigned)(e->val & 0xFFu),
        (unsigned)e->width,
        (unsigned)(e->func   & 0xFFFFFFu),
        (unsigned)(e->caller & 0xFFFFFFu));
    return (n < 0 || (size_t)n >= buflen) ? -1 : n;
}

#endif /* SONIC_REVERSE_DEBUG */
