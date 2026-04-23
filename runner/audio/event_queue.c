/*
 * event_queue.c — single-threaded cycle-stamped audio event ring.
 */
#include "event_queue.h"
#include <assert.h>
#include <stdio.h>

#define QUEUE_CAP 1024  /* handler ~50 writes + game ~20/frame; 1024 is lots of headroom */

static AudioEvent s_ring[QUEUE_CAP];
static size_t     s_head = 0;  /* producer writes here */
static size_t     s_tail = 0;  /* consumer reads here */
static size_t     s_overflow_count = 0;

void audio_event_push(uint32_t cycle_stamp, uint8_t port, uint8_t value)
{
    size_t next_head = (s_head + 1) % QUEUE_CAP;
    if (next_head == s_tail) {
        /* Overflow: drop the event. This shouldn't happen in practice
         * (capacity sized for worst-case + margin) but we fail soft rather
         * than crash. Counter is available for debugging. */
        s_overflow_count++;
        if (s_overflow_count < 10)
            fprintf(stderr, "[audio-queue] OVERFLOW cap=%d head=%zu tail=%zu\n",
                    QUEUE_CAP, s_head, s_tail);
        return;
    }
    s_ring[s_head].cycle_stamp = cycle_stamp;
    s_ring[s_head].port        = port;
    s_ring[s_head].value       = value;
    s_head = next_head;
}

int audio_event_pop(AudioEvent *out)
{
    if (s_tail == s_head) return 0;
    *out = s_ring[s_tail];
    s_tail = (s_tail + 1) % QUEUE_CAP;
    return 1;
}

void audio_event_queue_reset(void)
{
    s_head = s_tail = 0;
}

size_t audio_event_queue_count(void)
{
    return (s_head >= s_tail) ? (s_head - s_tail) : (QUEUE_CAP - s_tail + s_head);
}
