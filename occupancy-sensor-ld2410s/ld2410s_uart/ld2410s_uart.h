#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct LD2410S_uart;

struct LD2410S_report {
  bool occupied;
  uint16_t distance;
};

struct LD2410S_common_params {
  uint32_t farthest_gate;
  uint32_t nearest_gate;
  uint32_t unmanned_delay;
  uint32_t status_report_freq;
  uint32_t dist_report_freq;
  uint32_t response_speed;
};

struct LD2410S_threshold_params {
  uint32_t trigger[8]; /* gates 0-7 */
  uint32_t holding[8]; /* gates 0-7 */
};

struct LD2410S_snr_params {
  uint32_t trigger[8]; /* gates 8-15 */
  uint32_t hold[8];    /* gates 8-15 */
};

typedef void (*ld2410s_report_cb)(const struct LD2410S_report *report, void *user_data);
typedef void (*ld2410s_calibration_cb)(uint16_t progress_pct, void *user_data);

/* Lifecycle. Callbacks are invoked from the reader thread. */
struct LD2410S_uart *ld2410s_uart_init(const char *dev_path, bool debug,
                                       ld2410s_report_cb report_cb, void *report_user_data,
                                       ld2410s_calibration_cb cal_cb, void *cal_user_data);
void ld2410s_uart_free(struct LD2410S_uart *s);
int ld2410s_uart_start(struct LD2410S_uart *s);

/* getters block until response or timeout, ~6s max */
int ld2410s_uart_get_firmware(struct LD2410S_uart *s, uint8_t *out, size_t out_len, size_t *actual_len);
int ld2410s_uart_get_serial(struct LD2410S_uart *s, char *out, size_t out_len);
int ld2410s_uart_get_common_params(struct LD2410S_uart *s, struct LD2410S_common_params *out);
int ld2410s_uart_get_threshold(struct LD2410S_uart *s, struct LD2410S_threshold_params *out);
int ld2410s_uart_get_snr(struct LD2410S_uart *s, struct LD2410S_snr_params *out);

/* setters */
int ld2410s_uart_set_serial(struct LD2410S_uart *s, const char *serial);
int ld2410s_uart_set_param(struct LD2410S_uart *s, const char *name, uint32_t value);

/* Kick off calibration. Returns as soon as the command is acked; progress is
 * reported via the calibration callback registered in ld2410s_uart_init. Done when progress_pct reaches 100. */
int ld2410s_uart_start_calibration(struct LD2410S_uart *s, uint16_t trigger, uint16_t retention,
                                   uint16_t duration_secs);

/* Return how many vacant reports where last seen (0 if currently reports occupied) */
size_t ld2410s_uart_get_vacant_reports_count(struct LD2410S_uart *s);
