/*
 * mixer.c — PHASE 1 STUB. Real implementation in Phase 5.
 */
#include "mixer.h"

void audio_mixer_init(void) { }

void audio_mixer_drain(uint32_t frame_end_cycle,
                       int16_t *fm_stereo_out, size_t fm_cap, size_t *fm_written,
                       int16_t *psg_mono_out,  size_t psg_cap, size_t *psg_written)
{
    (void)frame_end_cycle; (void)fm_stereo_out; (void)fm_cap;
    (void)psg_mono_out; (void)psg_cap;
    if (fm_written)  *fm_written  = 0;
    if (psg_written) *psg_written = 0;
}
