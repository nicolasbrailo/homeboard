#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define TRANSPORT_MAX_DATA 128
struct transport;

/* Async, called when sensor sends a command response */
typedef void (*transport_cmd_response_cb)(void *ctx, uint16_t resp_cmd, uint16_t status,
                                          const uint8_t *data, size_t data_len);
/* Called when sensor sends an occupancy report */
typedef void (*transport_report_cb)(void *ctx, const uint8_t *frame, size_t len);

/* Called when sensor sends a calibration update report */
typedef void (*transport_calibration_cb)(void *ctx, const uint8_t *frame, size_t len);

/* Callbacks run on the device thread, not on the main app thread. Each
 * callback has its own ctx so they can be routed to different owners. */
struct transport *transport_init(const char *dev_path, bool debug,
                                 transport_cmd_response_cb on_cmd_response, void *cmd_response_ctx,
                                 transport_report_cb on_report, void *report_ctx,
                                 transport_calibration_cb on_calibration, void *calibration_ctx);
/* Start bg device thread */
int transport_start(struct transport *t);
void transport_free(struct transport *t);

/* Enqueue a command for the reader thread to send. Non-blocking.
 * Returns false if queue is full. */
bool transport_enqueue(struct transport *t, uint16_t cmd_word,
                       const void *data, size_t data_len);
