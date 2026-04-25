#pragma once

struct presence_config {
  unsigned sensor_stabilization_delay_secs;
  unsigned hysteresis_occupied;
  unsigned hysteresis_vacant;
};

// Loads JSON config from `path` into `cfg`. Missing keys keep defaults.
// Returns 0 on success, -1 on parse failure.
int presence_config_load(const char *path, struct presence_config *cfg);
