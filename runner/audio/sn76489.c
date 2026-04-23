/*
 * sn76489.c — cycle-stamped SN76489 PSG wrapper over clownmdemu's PSG library.
 *
 * Parallel to ym2612.c. Same rationale: reuse clownmdemu's PSG state
 * machine (known-correct) as a pure library. Cycle-to-sample conversion
 * happens here: PSG_Update takes a sample count, but our API takes a
 * 68K-cycle delta. We accumulate leftover cycles to avoid drift.
 */
#include "sn76489.h"        /* our wrapper API */
#include "psg.h"            /* clownmdemu's PSG — on include path */
#include "clownmdemu.h"     /* CLOWNMDEMU_MASTER_CLOCK_NTSC + dividers */
#include <string.h>

/* The clownmdemu PSG runs at 1 sample every (Z80_CLOCK_DIVIDER *
 * PSG_SAMPLE_RATE_DIVIDER) = 15 * 16 = 240 MASTER cycles. Our audio-cycle
 * counter is in 68K cycles; 68K = master/7, so 1 sample every 240/7 ≈
 * 34.2857 68K cycles. We track leftover cycles in 68K units to avoid drift.
 *
 * Exact math: 1 sample per (15 * 16) / 7 = 240/7 68K cycles. Multiply
 * everything by 7 to stay in integer math:
 *     samples_to_emit = (cycles_68k * 7 + leftover_x7) / 240
 *     new_leftover_x7 = (cycles_68k * 7 + leftover_x7) - samples_to_emit * 240
 */
#define PSG_SAMPLE_DIVISOR_MASTER   240u   /* Z80_CLOCK_DIVIDER * PSG_SAMPLE_RATE_DIVIDER */

static PSG s_psg;
static int        s_inited = 0;
static uint32_t   s_leftover_master_cycles = 0;  /* in master-cycle units */

#define PSG_SCRATCH_SAMPLES  8192
static int16_t s_scratch[PSG_SCRATCH_SAMPLES];
static size_t  s_scratch_write = 0;
static size_t  s_scratch_read  = 0;

void psg_init(void)
{
    PSG_Initialise(&s_psg);
    s_scratch_write = s_scratch_read = 0;
    s_leftover_master_cycles = 0;
    s_inited = 1;
}

void psg_advance(uint32_t cycles_68k)
{
    if (!s_inited) psg_init();
    if (cycles_68k == 0) return;

    /* Convert 68K cycles to master cycles, add leftover. */
    uint64_t master_cycles = (uint64_t)cycles_68k * 7u + s_leftover_master_cycles;
    size_t   samples_to_emit = (size_t)(master_cycles / PSG_SAMPLE_DIVISOR_MASTER);
    s_leftover_master_cycles = (uint32_t)(master_cycles % PSG_SAMPLE_DIVISOR_MASTER);

    while (samples_to_emit > 0) {
        size_t avail = PSG_SCRATCH_SAMPLES - s_scratch_write;
        if (avail == 0) return;  /* scratch full; drop */
        size_t chunk = samples_to_emit < avail ? samples_to_emit : avail;
        memset(&s_scratch[s_scratch_write], 0, chunk * sizeof(int16_t));
        PSG_Update(&s_psg, &s_scratch[s_scratch_write], chunk);
        s_scratch_write  += chunk;
        samples_to_emit  -= chunk;
    }
}

void psg_write(uint8_t value)
{
    if (!s_inited) psg_init();
    PSG_DoCommand(&s_psg, value);
}

void psg_render(int16_t *out, size_t sample_count)
{
    if (!out || sample_count == 0) return;
    size_t available = s_scratch_write - s_scratch_read;
    size_t copy = sample_count < available ? sample_count : available;
    if (copy > 0) {
        memcpy(out, &s_scratch[s_scratch_read], copy * sizeof(int16_t));
        s_scratch_read += copy;
    }
    if (copy < sample_count) {
        memset(out + copy, 0, (sample_count - copy) * sizeof(int16_t));
    }
    if (s_scratch_read >= s_scratch_write) {
        s_scratch_read = s_scratch_write = 0;
    }
}

uint32_t psg_sample_rate(void)
{
    /* 53693175 / 240 = 223721 Hz NTSC. */
    return (uint32_t)(CLOWNMDEMU_MASTER_CLOCK_NTSC / PSG_SAMPLE_DIVISOR_MASTER);
}
