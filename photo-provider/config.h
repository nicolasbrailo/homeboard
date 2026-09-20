#pragma once

#include <stdbool.h>
#include <stdint.h>

enum pp_backend_type {
  PP_BACKEND_WWWSLIDE,
  PP_BACKEND_IMMICH,
};

// Which albums pictures may come from. Empty strings and 0 years mean "no
// constraint of that kind", so an all-zero filter draws from every album.
// See libimmich-random's immich_album_filter for the matching rules.
struct pp_album_filter_config {
  char name[512];    // Comma-separated globs; keep an album matching any
  char exclude[512]; // Comma-separated globs; drop an album matching any
  uint32_t from_year;
  uint32_t to_year;
};

struct pp_immich_config {
  char server_url[256];
  char api_key[256];
  uint32_t max_pictures_per_album; // 0: no limit
  uint32_t percent_per_album;      // 1..100; 0 means 100
  uint32_t album_refresh_s;        // 0: never refresh the album list
  struct pp_album_filter_config album_filter;
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

// Writes cfg back to path, so a setting changed over D-Bus survives a restart.
// Rewrites the whole file: every key pp_config_load reads is written back, and
// anything else the file held is dropped. Returns 0 on success.
int pp_config_save(const char *path, const struct pp_config *cfg);
