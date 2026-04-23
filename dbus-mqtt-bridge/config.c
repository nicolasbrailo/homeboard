#include "config.h"

#include <json-c/json.h>
#include <stdio.h>
#include <string.h>

static void copy_str(char *dst, size_t dstsz, const char *src) {
  if (!src) {
    dst[0] = '\0';
    return;
  }
  strncpy(dst, src, dstsz - 1);
  dst[dstsz - 1] = '\0';
}

int rc_config_load(const char *path, struct rc_config *cfg) {
  struct json_object *root = json_object_from_file(path);
  if (!root) {
    fprintf(stderr, "Failed to parse config: %s\n", path);
    return -1;
  }

  copy_str(cfg->mqtt_host, sizeof(cfg->mqtt_host), "127.0.0.1");
  cfg->mqtt_port = 1883;
  copy_str(cfg->mqtt_client_id, sizeof(cfg->mqtt_client_id), "homeboard-bridge");
  cfg->mqtt_user[0] = '\0';
  cfg->mqtt_pass[0] = '\0';
  cfg->mqtt_keepalive_s = 30;
  copy_str(cfg->topic_prefix, sizeof(cfg->topic_prefix), "homeboard/");

  struct json_object *val;
  if (json_object_object_get_ex(root, "mqtt_host", &val))
    copy_str(cfg->mqtt_host, sizeof(cfg->mqtt_host), json_object_get_string(val));
  if (json_object_object_get_ex(root, "mqtt_port", &val)) {
    int n = json_object_get_int(val);
    if (n > 0 && n < 65536)
      cfg->mqtt_port = (uint16_t)n;
    else
      fprintf(stderr, "Invalid mqtt_port %d, using default %u\n", n, cfg->mqtt_port);
  }
  if (json_object_object_get_ex(root, "mqtt_client_id", &val))
    copy_str(cfg->mqtt_client_id, sizeof(cfg->mqtt_client_id), json_object_get_string(val));
  if (json_object_object_get_ex(root, "mqtt_user", &val))
    copy_str(cfg->mqtt_user, sizeof(cfg->mqtt_user), json_object_get_string(val));
  if (json_object_object_get_ex(root, "mqtt_pass", &val))
    copy_str(cfg->mqtt_pass, sizeof(cfg->mqtt_pass), json_object_get_string(val));
  if (json_object_object_get_ex(root, "mqtt_keepalive_s", &val)) {
    int n = json_object_get_int(val);
    if (n > 0 && n < 3600)
      cfg->mqtt_keepalive_s = (uint16_t)n;
  }
  if (json_object_object_get_ex(root, "topic_prefix", &val)) {
    const char *s = json_object_get_string(val);
    size_t n = s ? strlen(s) : 0;
    if (n == 0 || s[n - 1] != '/') {
      fprintf(stderr, "topic_prefix must end with '/': got '%s'\n", s ? s : "(null)");
      json_object_put(root);
      return -1;
    }
    copy_str(cfg->topic_prefix, sizeof(cfg->topic_prefix), s);
  }

  json_object_put(root);
  printf("Config: broker=%s:%u client_id=%s topic_prefix=%s\n", cfg->mqtt_host, cfg->mqtt_port, cfg->mqtt_client_id,
         cfg->topic_prefix);
  return 0;
}
