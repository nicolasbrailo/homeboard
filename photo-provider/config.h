#pragma once

#include <stdbool.h>
#include <stdint.h>

struct pp_config {
  char server_url[256];
  uint32_t target_w;
  uint32_t target_h;
  bool embed_qr;
  uint32_t cache_depth;
  uint32_t history_depth;
  bool dump_to_disk;
  char dump_dir[256];
  uint32_t connect_timeout_s;
  uint32_t request_timeout_s;
};

int pp_config_load(const char *path, struct pp_config *cfg);
