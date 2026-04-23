#pragma once

#include <stdint.h>

#include <stdbool.h>

struct ambience_config {
  uint32_t transition_time_s;
  uint32_t rotation; // 0, 90, 180, 270
  bool embed_qr;
  bool use_eink_for_metadata;
  char fallback_image[255];
};

int ambience_config_load(const char *path, struct ambience_config *cfg);
