#pragma once

#include "ld2410s_uart/ld2410s_uart.h"

#include <stdbool.h>
#include <stdint.h>

struct LD2410S;

struct LD2410S_config {
  bool enable_uart;
  char device[64];
  bool debug;
  int sensor_report_gpio; /* -1 disables GPIO */
  unsigned startup_delay_secs;
  unsigned hysteresis_occupied;
  unsigned hysteresis_vacant;
};

typedef void (*ld2410s_state_change_cb)(bool occupied, uint16_t distance, void *user_data);

struct LD2410S *ld2410s_init(const struct LD2410S_config *cfg, ld2410s_state_change_cb cb, void *user_data);
void ld2410s_free(struct LD2410S *s);

int ld2410s_get_common_params(struct LD2410S *s, struct LD2410S_common_params *out);
int ld2410s_set_param(struct LD2410S *s, const char *name, uint32_t value);
int ld2410s_start_calibration(struct LD2410S *s, uint16_t trigger, uint16_t retention, uint16_t duration_secs);
