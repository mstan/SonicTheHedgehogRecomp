/*
 * psg.h — our own SN76489 PSG implementation. Parallel to ym2612.h.
 *
 * Phase 1: header stub. Real implementation in Phase 4.
 */
#ifndef AUDIO_PSG_H
#define AUDIO_PSG_H

#include <stdint.h>
#include <stddef.h>

void psg_init(void);
void psg_advance(uint32_t cycles_68k);
void psg_write(uint8_t value);
void psg_render(int16_t *out, size_t sample_count);
uint32_t psg_sample_rate(void);

#endif
