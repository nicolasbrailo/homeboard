#pragma once

#include <stdbool.h>
#include <stdint.h>

enum pp_backend_type {
  PP_BACKEND_WWWSLIDE,
  PP_BACKEND_IMMICH,
};

struct pp_immich_config {
  char server_url[256];
  char api_key[256];
  uint32_t max_pictures_per_album; // 0: no limit
  uint32_t percent_per_album;      // 1..100; 0 means 100
  uint32_t album_refresh_s;        // 0: never refresh the album list
};

struct pp_config {
  enum pp_backend_type backend;

  // wwwslide backend
  char server_url[256];

  // All backends
  uint32_t target_w;
  uint32_t target_h;
  bool embed_qr;
  uint32_t cache_depth;
  uint32_t history_depth;
  bool dump_to_disk;
  char dump_dir[256];
  uint32_t connect_timeout_s;
  uint32_t request_timeout_s;

  // immich backend; only validated if selected
  struct pp_immich_config immich;
};

int pp_config_load(const char *path, struct pp_config *cfg);
