#include "config.h"

#include <json-c/json.h>
#include <stdio.h>
#include <string.h>

static int parse_backend(const char *name, enum pp_backend_type *out) {
  if (!name)
    return -1;
  if (strcmp(name, "wwwslide") == 0) {
    *out = PP_BACKEND_WWWSLIDE;
    return 0;
  }
  if (strcmp(name, "immich") == 0) {
    *out = PP_BACKEND_IMMICH;
    return 0;
  }
  return -1;
}

static void load_immich_section(struct json_object *immich,
                                struct pp_immich_config *icfg) {
  struct json_object *val;
  if (json_object_object_get_ex(immich, "server_url", &val))
    strncpy(icfg->server_url, json_object_get_string(val),
            sizeof(icfg->server_url) - 1);
  if (json_object_object_get_ex(immich, "api_key", &val))
    strncpy(icfg->api_key, json_object_get_string(val),
            sizeof(icfg->api_key) - 1);
  if (json_object_object_get_ex(immich, "max_pictures_per_album", &val)) {
    int n = json_object_get_int(val);
    icfg->max_pictures_per_album = n < 0 ? 0 : (uint32_t)n;
  }
  if (json_object_object_get_ex(immich, "percent_per_album", &val)) {
    int n = json_object_get_int(val);
    icfg->percent_per_album = n < 0 ? 0 : n > 100 ? 100 : (uint32_t)n;
  }
  if (json_object_object_get_ex(immich, "album_refresh_s", &val)) {
    int n = json_object_get_int(val);
    icfg->album_refresh_s = n < 0 ? 0 : (uint32_t)n;
  }
}

static int check_immich(const struct pp_immich_config *icfg) {
  if (icfg->server_url[0] == '\0') {
    fprintf(stderr, "Config: immich.server_url is required\n");
    return -1;
  }
  if (icfg->api_key[0] == '\0') {
    fprintf(stderr, "Config: immich.api_key is required\n");
    return -1;
  }
  return 0;
}

int pp_config_load(const char *path, struct pp_config *cfg) {
  struct json_object *root = json_object_from_file(path);
  if (!root) {
    fprintf(stderr, "Failed to parse config: %s\n", path);
    return -1;
  }

  memset(cfg, 0, sizeof(*cfg));
  cfg->backend = PP_BACKEND_WWWSLIDE;
  strncpy(cfg->server_url, "http://homeboard.local:8080",
          sizeof(cfg->server_url));
  cfg->target_w = 1920;
  cfg->target_h = 1080;
  cfg->embed_qr = true;
  cfg->cache_depth = 2;
  cfg->history_depth = 8;
  cfg->dump_to_disk = false;
  strncpy(cfg->dump_dir, "/tmp/photo-provider", sizeof(cfg->dump_dir));
  cfg->connect_timeout_s = 5;
  cfg->request_timeout_s = 60;

  struct json_object *val;
  // Unlike other keys, a bad backend is fatal: silently falling back to
  // wwwslide would serve photos from a server the user didn't ask for.
  if (json_object_object_get_ex(root, "backend", &val) &&
      parse_backend(json_object_get_string(val), &cfg->backend) < 0) {
    fprintf(stderr,
            "Config %s: unknown backend %s (expected \"wwwslide\" or "
            "\"immich\")\n",
            path, json_object_to_json_string(val));
    json_object_put(root);
    return -1;
  }
  if (json_object_object_get_ex(root, "server_url", &val))
    strncpy(cfg->server_url, json_object_get_string(val),
            sizeof(cfg->server_url) - 1);
  if (json_object_object_get_ex(root, "target_size_w", &val))
    cfg->target_w = (uint32_t)json_object_get_int(val);
  if (json_object_object_get_ex(root, "target_size_h", &val))
    cfg->target_h = (uint32_t)json_object_get_int(val);
  if (json_object_object_get_ex(root, "embed_qr", &val))
    cfg->embed_qr = json_object_get_boolean(val);
  if (json_object_object_get_ex(root, "cache_depth", &val)) {
    int n = json_object_get_int(val);
    cfg->cache_depth = n < 1 ? 1 : (uint32_t)n;
  }
  if (json_object_object_get_ex(root, "history_depth", &val)) {
    int n = json_object_get_int(val);
    cfg->history_depth = n < 0 ? 0 : (uint32_t)n;
  }
  if (json_object_object_get_ex(root, "dump_to_disk", &val))
    cfg->dump_to_disk = json_object_get_boolean(val);
  if (json_object_object_get_ex(root, "dump_dir", &val))
    strncpy(cfg->dump_dir, json_object_get_string(val),
            sizeof(cfg->dump_dir) - 1);
  if (json_object_object_get_ex(root, "connect_timeout_s", &val))
    cfg->connect_timeout_s = (uint32_t)json_object_get_int(val);
  if (json_object_object_get_ex(root, "request_timeout_s", &val))
    cfg->request_timeout_s = (uint32_t)json_object_get_int(val);
  if (json_object_object_get_ex(root, "immich", &val))
    load_immich_section(val, &cfg->immich);

  json_object_put(root);

  if (cfg->backend == PP_BACKEND_IMMICH && check_immich(&cfg->immich) < 0)
    return -1;

  printf("Config loaded: backend=%s %ux%u qr=%d cache=%u history=%u dump=%d "
         "dir=%s\n",
         cfg->backend == PP_BACKEND_IMMICH ? "immich" : "wwwslide",
         cfg->target_w, cfg->target_h, cfg->embed_qr, cfg->cache_depth,
         cfg->history_depth, cfg->dump_to_disk, cfg->dump_dir);
  if (cfg->backend == PP_BACKEND_IMMICH)
    printf("Immich: server=%s max_per_album=%u percent_per_album=%u "
           "album_refresh_s=%u\n",
           cfg->immich.server_url, cfg->immich.max_pictures_per_album,
           cfg->immich.percent_per_album, cfg->immich.album_refresh_s);
  else
    printf("wwwslide: server=%s\n", cfg->server_url);
  return 0;
}
