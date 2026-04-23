#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Synchronous request/response layer over the async transport.
 * Serializes callers with a mutex, turns the transport's queue + callback
 * pattern into plain blocking calls. Report and calibration frames are
 * forwarded verbatim to user callbacks. */

struct session;

/* Raw frame forwarders. Invoked from the reader thread. */
typedef void (*session_frame_cb)(void *ctx, const uint8_t *frame, size_t len);

struct session *session_init(const char *dev_path, bool debug,
                             session_frame_cb report_cb, void *report_ctx,
                             session_frame_cb cal_cb, void *cal_ctx);
void session_free(struct session *s);
int session_start(struct session *s);

/* Send a command wrapped with enable_config + cmd + end_config, wait for its
 * ack, copy response payload into out_buf. All three frames go out under a
 * single mutex hold so no other caller can interleave.
 * *out_len (if non-NULL) is set to the actual response length, even if > out_cap.
 * Returns 0 on success, -1 on timeout, queue-full, or nonzero device status. */
int session_cmd(struct session *s, uint16_t cmd_word,
                const void *in, size_t in_len,
                uint8_t *out_buf, size_t out_cap, size_t *out_len);
