#include "config.h"

#include <json-c/json.h>
#include <stdio.h>
#include <string.h>

int pp_config_load(const char *path, struct pp_config *cfg) {
  struct json_object *root = json_object_from_file(path);
  if (!root) {
    fprintf(stderr, "Failed to parse config: %s\n", path);
    return -1;
  }

  strncpy(cfg->server_url, "http://homeboard.local:8080", sizeof(cfg->server_url));
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
  if (json_object_object_get_ex(root, "server_url", &val))
    strncpy(cfg->server_url, json_object_get_string(val), sizeof(cfg->server_url) - 1);
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
    strncpy(cfg->dump_dir, json_object_get_string(val), sizeof(cfg->dump_dir) - 1);
  if (json_object_object_get_ex(root, "connect_timeout_s", &val))
    cfg->connect_timeout_s = (uint32_t)json_object_get_int(val);
  if (json_object_object_get_ex(root, "request_timeout_s", &val))
    cfg->request_timeout_s = (uint32_t)json_object_get_int(val);

  json_object_put(root);
  printf("Config loaded: server=%s %ux%u qr=%d cache=%u history=%u dump=%d dir=%s\n", cfg->server_url, cfg->target_w,
         cfg->target_h, cfg->embed_qr, cfg->cache_depth, cfg->history_depth, cfg->dump_to_disk, cfg->dump_dir);
  return 0;
}
