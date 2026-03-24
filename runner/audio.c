#include "audio.h"
#include <SDL2/SDL.h>
#include <stdio.h>

/* Volume divisors from the reference mixer (clownmdemu-frontend-common/mixer.h) */
#define FM_VOL_DIV  1
#define PSG_VOL_DIV 8

static SDL_AudioDeviceID s_dev = 0;

/* Scratch output buffer — sized for worst-case PSG frames per video frame.
 * PSG rate ~223721 Hz / 50 Hz (PAL) ≈ 4475 frames.  Double for headroom. */
#define OUT_BUF_FRAMES 16384
static int16_t s_out[OUT_BUF_FRAMES * 2];

int audio_init(int psg_sample_rate)
{
    SDL_AudioSpec want, got;
    SDL_memset(&want, 0, sizeof(want));
    want.freq     = psg_sample_rate;
    want.format   = AUDIO_S16SYS;
    want.channels = 2;
    want.samples  = 1024;
    want.callback = NULL;   /* push model via SDL_QueueAudio */

    s_dev = SDL_OpenAudioDevice(NULL, 0, &want, &got, 0);
    if (s_dev == 0) {
        fprintf(stderr, "audio: SDL_OpenAudioDevice failed: %s\n", SDL_GetError());
        return -1;
    }
    SDL_PauseAudioDevice(s_dev, 0);
    return 0;
}

void audio_close(void)
{
    if (s_dev != 0) {
        SDL_CloseAudioDevice(s_dev);
        s_dev = 0;
    }
}

void audio_flush(const int16_t *fm_buf,  size_t fm_frames,
                 const int16_t *psg_buf, size_t psg_frames)
{
    if (s_dev == 0 || psg_buf == NULL || psg_frames == 0)
        return;

    if (psg_frames > OUT_BUF_FRAMES)
        psg_frames = OUT_BUF_FRAMES;

    /* Reference mixer approach: iterate at PSG rate (no PSG resampling),
     * upsample FM to PSG rate via nearest-neighbour.
     * Divisors: PSG/8, FM/1 — same as clownmdemu-frontend-common/mixer.h */
    for (size_t i = 0; i < psg_frames; i++) {
        int32_t p = (int32_t)psg_buf[i] / PSG_VOL_DIV;
        int32_t l = p;
        int32_t r = p;

        if (fm_buf != NULL && fm_frames > 0) {
            size_t fi = i * fm_frames / psg_frames;
            l += (int32_t)fm_buf[fi * 2 + 0] / FM_VOL_DIV;
            r += (int32_t)fm_buf[fi * 2 + 1] / FM_VOL_DIV;
        }

        if (l >  32767) l =  32767; else if (l < -32768) l = -32768;
        if (r >  32767) r =  32767; else if (r < -32768) r = -32768;

        s_out[i * 2 + 0] = (int16_t)l;
        s_out[i * 2 + 1] = (int16_t)r;
    }

    /* Simple throttle: don't let the queue grow past ~3 video frames */
    Uint32 limit = (Uint32)(psg_frames * 2 * sizeof(int16_t) * 3);
    if (SDL_GetQueuedAudioSize(s_dev) <= limit)
        SDL_QueueAudio(s_dev, s_out, (Uint32)(psg_frames * 2 * sizeof(int16_t)));
}
