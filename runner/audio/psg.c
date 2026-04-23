/*
 * psg.c — PHASE 1 STUB. Real implementation in Phase 4.
 */
#include "psg.h"

void     psg_init(void)                                   { }
void     psg_advance(uint32_t cycles_68k)                 { (void)cycles_68k; }
void     psg_write(uint8_t value)                         { (void)value; }
void     psg_render(int16_t *out, size_t sample_count)    { (void)out; (void)sample_count; }
uint32_t psg_sample_rate(void)                            { return 223721; }
