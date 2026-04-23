/*
 * mixer.c — cycle-stamped event drain + sample render.
 *
 * Called once per wall frame from glue_end_of_wall_frame. Walks every
 * queued (cycle_stamp, port, value) event in order, advancing the YM2612
 * and PSG by the cycle delta BEFORE each write so envelope/LFO state
 * progresses between writes. After all events, advances to frame_end_cycle
 * and renders the full wall-frame's worth of samples.
 *
 * The advance-between-writes step is the architectural fix for the
 * boop/squelch artifact — writes no longer collapse onto a single FM-
 * sample boundary because the chip's sample clock genuinely ticks between
 * them at their real cycle spacing.
 */
#include "mixer.h"
#include "event_queue.h"
#include "ym2612.h"
#include "sn76489.h"

void audio_mixer_init(void)
{
    ym2612_init();
    psg_init();
}

void audio_mixer_drain(uint32_t frame_end_cycle,
                       int16_t *fm_stereo_out, size_t fm_cap, size_t *fm_written,
                       int16_t *psg_mono_out,  size_t psg_cap, size_t *psg_written)
{
    uint32_t prev_stamp = 0;
    AudioEvent e;

    while (audio_event_pop(&e)) {
        /* Clamp to monotonic: if generator/queue delivered an out-of-order
         * stamp we don't want to underflow. Shouldn't happen — all pushes
         * from one fiber in cycle order. */
        if (e.cycle_stamp < prev_stamp) e.cycle_stamp = prev_stamp;

        uint32_t delta = e.cycle_stamp - prev_stamp;
        if (delta > 0) {
            ym2612_advance(delta);
            psg_advance(delta);
        }
        switch (e.port) {
            case AUDIO_PORT_FM1_ADDR: ym2612_write(0, e.value); break;
            case AUDIO_PORT_FM1_DATA: ym2612_write(1, e.value); break;
            case AUDIO_PORT_FM2_ADDR: ym2612_write(2, e.value); break;
            case AUDIO_PORT_FM2_DATA: ym2612_write(3, e.value); break;
            case AUDIO_PORT_PSG:      psg_write(e.value);       break;
            default: break;
        }
        prev_stamp = e.cycle_stamp;
    }

    /* Advance to end of wall frame. */
    if (frame_end_cycle > prev_stamp) {
        uint32_t tail = frame_end_cycle - prev_stamp;
        ym2612_advance(tail);
        psg_advance(tail);
    }

    /* Copy generated samples into the caller's buffers. */
    size_t fm_avail  = ym2612_samples_available();
    size_t psg_avail = psg_samples_available();
    size_t fm_n  = fm_avail  < fm_cap  ? fm_avail  : fm_cap;
    size_t psg_n = psg_avail < psg_cap ? psg_avail : psg_cap;

    if (fm_stereo_out && fm_n > 0)  ym2612_render(fm_stereo_out,  fm_n);
    if (psg_mono_out  && psg_n > 0) psg_render(psg_mono_out,   psg_n);

    if (fm_written)  *fm_written  = fm_n;
    if (psg_written) *psg_written = psg_n;
}
