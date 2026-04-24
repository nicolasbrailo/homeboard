#pragma once

#include <stdint.h>

#include <stdbool.h>

#include "jpeg_render/img_render.h"

struct ambience_config {
  uint32_t transition_time_s;
  struct img_render_cfg render;
  bool embed_qr;
  bool use_eink_for_metadata;
  char fallback_image[255];
};

int ambience_config_load(const char *path, struct ambience_config *cfg);
