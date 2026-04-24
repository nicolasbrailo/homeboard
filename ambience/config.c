#include "config.h"

#include <json-c/json.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static const char *h_align_name(enum horizontal_align a) {
  switch (a) {
  case HORIZONTAL_ALIGN_LEFT:
    return "left";
  case HORIZONTAL_ALIGN_RIGHT:
    return "right";
  case HORIZONTAL_ALIGN_CENTER:
  default:
    return "center";
  }
}

static const char *v_align_name(enum vertical_align a) {
  switch (a) {
  case VERTICAL_ALIGN_TOP:
    return "top";
  case VERTICAL_ALIGN_BOTTOM:
    return "bottom";
  case VERTICAL_ALIGN_CENTER:
  default:
    return "center";
  }
}

int ambience_config_load(const char *path, struct ambience_config *cfg) {
  struct json_object *root = json_object_from_file(path);
  if (!root) {
    fprintf(stderr, "Failed to parse config: %s\n", path);
    return -1;
  }

  cfg->transition_time_s = 30;
  cfg->render.rot = ROT_0;
  cfg->render.interp = INTERP_BILINEAR;
  cfg->render.h_align = HORIZONTAL_ALIGN_CENTER;
  cfg->render.v_align = VERTICAL_ALIGN_CENTER;
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
    cfg->render.rot = (enum rotation)n;
  }
  if (json_object_object_get_ex(root, "interpolation", &val)) {
    const char *s = json_object_get_string(val);
    if (s && strcmp(s, "nearest") == 0)
      cfg->render.interp = INTERP_NEAREST;
    else if (s && strcmp(s, "bilinear") == 0)
      cfg->render.interp = INTERP_BILINEAR;
    else {
      fprintf(stderr, "Invalid interpolation '%s' (must be nearest or bilinear)\n", s ? s : "");
      json_object_put(root);
      return -1;
    }
  }
  if (json_object_object_get_ex(root, "horizontal_align", &val)) {
    const char *s = json_object_get_string(val);
    if (s && strcmp(s, "left") == 0)
      cfg->render.h_align = HORIZONTAL_ALIGN_LEFT;
    else if (s && strcmp(s, "center") == 0)
      cfg->render.h_align = HORIZONTAL_ALIGN_CENTER;
    else if (s && strcmp(s, "right") == 0)
      cfg->render.h_align = HORIZONTAL_ALIGN_RIGHT;
    else {
      fprintf(stderr, "Invalid horizontal_align '%s' (must be left, center, right)\n", s ? s : "");
      json_object_put(root);
      return -1;
    }
  }
  if (json_object_object_get_ex(root, "vertical_align", &val)) {
    const char *s = json_object_get_string(val);
    if (s && strcmp(s, "top") == 0)
      cfg->render.v_align = VERTICAL_ALIGN_TOP;
    else if (s && strcmp(s, "center") == 0)
      cfg->render.v_align = VERTICAL_ALIGN_CENTER;
    else if (s && strcmp(s, "bottom") == 0)
      cfg->render.v_align = VERTICAL_ALIGN_BOTTOM;
    else {
      fprintf(stderr, "Invalid vertical_align '%s' (must be top, center, bottom)\n", s ? s : "");
      json_object_put(root);
      return -1;
    }
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
  printf("Config loaded: transition_time_s=%u rotation=%u interpolation=%s h_align=%s v_align=%s embed_qr=%d "
         "use_eink_for_metadata=%d fallback_image=%s\n",
         cfg->transition_time_s, (uint32_t)cfg->render.rot,
         cfg->render.interp == INTERP_BILINEAR ? "bilinear" : "nearest", h_align_name(cfg->render.h_align),
         v_align_name(cfg->render.v_align), cfg->embed_qr, cfg->use_eink_for_metadata, cfg->fallback_image);
  return 0;
}
