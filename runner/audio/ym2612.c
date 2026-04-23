/*
 * ym2612.c — PHASE 1 STUB. Real implementation in Phase 3.
 *
 * Links but does nothing. Game still runs on clownmdemu's audio output
 * during Phase 1 scaffolding.
 */
#include "ym2612.h"

void     ym2612_init(void)                                  { }
void     ym2612_advance(uint32_t cycles_68k)                { (void)cycles_68k; }
void     ym2612_write(uint8_t port, uint8_t value)          { (void)port; (void)value; }
void     ym2612_render(int16_t *out, size_t sample_count)   { (void)out; (void)sample_count; }
uint32_t ym2612_sample_rate(void)                           { return 223721; }
