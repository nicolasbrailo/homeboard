#include "config.h"

#include <json-c/json.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

int ambience_config_load(const char *path, struct ambience_config *cfg) {
  struct json_object *root = json_object_from_file(path);
  if (!root) {
    fprintf(stderr, "Failed to parse config: %s\n", path);
    return -1;
  }

  cfg->transition_time_s = 30;
  cfg->rotation = 0;
  cfg->embed_qr = false;
  cfg->use_eink_for_metadata = false;
  cfg->fallback_image[0] = '\0';

  struct json_object *val;
  if (json_object_object_get_ex(root, "transition_time_s", &val)) {
    int n = json_object_get_int(val);
    if (n < 3 || n > 300) {
      fprintf(stderr, "Invalid transition time %d (must be (3, 300])\n", n);
    }
    cfg->transition_time_s = n < 1 ? 1 : (uint32_t)n;
  }
  if (json_object_object_get_ex(root, "rotation", &val)) {
    int n = json_object_get_int(val);
    if (n != 0 && n != 90 && n != 180 && n != 270) {
      fprintf(stderr, "Invalid rotation %d (must be 0, 90, 180, 270)\n", n);
      json_object_put(root);
      return -1;
    }
    cfg->rotation = (uint32_t)n;
  }
  if (json_object_object_get_ex(root, "embed_qr", &val))
    cfg->embed_qr = json_object_get_boolean(val);
  if (json_object_object_get_ex(root, "use_eink_for_metadata", &val))
    cfg->use_eink_for_metadata = json_object_get_boolean(val);
  if (json_object_object_get_ex(root, "fallback_image", &val)) {
    const char *s = json_object_get_string(val);
    if (s && s[0] != '\0') {
      if (access(s, R_OK) == 0) {
        snprintf(cfg->fallback_image, sizeof(cfg->fallback_image), "%s", s);
      } else {
        fprintf(stderr, "fallback_image '%s' not readable, ignoring\n", s);
      }
    }
  }

  json_object_put(root);
  printf("Config loaded: transition_time_s=%u rotation=%u embed_qr=%d use_eink_for_metadata=%d fallback_image=%s\n",
         cfg->transition_time_s, cfg->rotation, cfg->embed_qr, cfg->use_eink_for_metadata, cfg->fallback_image);
  return 0;
}
