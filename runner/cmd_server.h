/*
 * cmd_server.h - Non-blocking TCP command server for debug queries.
 *
 * Listens on localhost, accepts line-delimited JSON commands,
 * returns JSON responses. Polled once per frame from main loop.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    bool     should_quit;
    int      run_extra_frames;
    bool     input_override;
    uint8_t  input_keys;      /* Genesis: Up/Down/Left/Right/A/B/C/Start */
} CmdResult;

/* Start listening on the given port. */
void cmd_server_init(int port);

/* Poll for incoming commands. Call once per frame. Returns actions for main loop. */
CmdResult cmd_server_poll(void);

/* After run_extra_frames completes, send the result back to the client. */
void cmd_server_send_frame_result(int frames_run);

/* Record per-frame state into the history ring buffer. Call after each frame. */
void cmd_server_record_frame(uint32_t frame_num);

/* Cleanup sockets. */
void cmd_server_shutdown(void);
