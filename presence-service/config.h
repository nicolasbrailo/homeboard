#pragma once

struct presence_config {
  unsigned sensor_stabilization_delay_secs;
  unsigned hysteresis_occupied;
  unsigned hysteresis_vacant;

  // "Silent" (e.g. nighttime) period, during which stricter hysteresis is
  // applied to suppress spurious wakeups. Times are minutes since midnight
  // in local time, [0,1440). The window wraps midnight when start > end.
  // silent_start_min < 0 disables the silent period entirely.
  unsigned silent_hysteresis_occupied;
  unsigned silent_hysteresis_vacant;
  int silent_start_min;
  int silent_end_min;
};

// Loads JSON config from `path` into `cfg`. Missing keys keep defaults.
// Returns 0 on success, -1 on parse failure.
int presence_config_load(const char *path, struct presence_config *cfg);
