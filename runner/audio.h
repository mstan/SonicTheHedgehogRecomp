#ifndef AUDIO_H
#define AUDIO_H

#include <stddef.h>
#include <stdint.h>

/*
 * Output sample rate is the PSG rate (~223721 Hz NTSC).
 * The reference mixer (clownmdemu-frontend-common/mixer.h) uses PSG as the
 * base rate so PSG never needs resampling.  FM is upsampled to match.
 */
int  audio_init(int psg_sample_rate);
void audio_close(void);

/*
 * Mix one video frame and push to the SDL ring buffer.
 *
 *   fm_buf    — stereo int16 at FM rate  (~53267 Hz)
 *   fm_frames — number of FM stereo frames
 *   psg_buf   — mono   int16 at PSG rate (~223721 Hz)  ← output rate base
 *   psg_frames— number of PSG mono frames
 *
 * FM is upsampled to PSG rate; PSG is expanded mono→stereo.
 * Volume divisors match the reference: PSG÷8, FM÷1.
 */
void audio_flush(const int16_t *fm_buf,  size_t fm_frames,
                 const int16_t *psg_buf, size_t psg_frames);

#endif /* AUDIO_H */
