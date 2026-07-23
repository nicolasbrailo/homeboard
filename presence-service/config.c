#include "config.h"

#include <json-c/json.h>
#include <stdio.h>

// Parses an "HH:MM" 24h local time into minutes since midnight [0,1440).
// Returns -1 on any malformed input.
static int parse_hhmm(const char *s) {
  if (!s)
    return -1;
  int h, m;
  char extra;
  if (sscanf(s, "%d:%d%c", &h, &m, &extra) != 2)
    return -1;
  if (h < 0 || h > 23 || m < 0 || m > 59)
    return -1;
  return h * 60 + m;
}

int presence_config_load(const char *path, struct presence_config *cfg) {
  cfg->sensor_stabilization_delay_secs = 10;
  cfg->hysteresis_occupied = 5;
  cfg->hysteresis_vacant = 9;
  cfg->silent_hysteresis_occupied = 30;
  cfg->silent_hysteresis_vacant = 5;
  cfg->silent_start_min = -1; /* silent period disabled unless configured */
  cfg->silent_end_min = -1;

  struct json_object *root = json_object_from_file(path);
  if (!root) {
    fprintf(stderr, "Failed to parse config: %s\n", path);
    return -1;
  }

  struct json_object *val;
  if (json_object_object_get_ex(root, "sensor_stabilization_delay_secs", &val))
    cfg->sensor_stabilization_delay_secs = (unsigned)json_object_get_int(val);
  if (json_object_object_get_ex(root, "hysteresis_occupied", &val))
    cfg->hysteresis_occupied = (unsigned)json_object_get_int(val);
  if (json_object_object_get_ex(root, "hysteresis_vacant", &val))
    cfg->hysteresis_vacant = (unsigned)json_object_get_int(val);
  if (json_object_object_get_ex(root, "silent_hysteresis_occupied", &val))
    cfg->silent_hysteresis_occupied = (unsigned)json_object_get_int(val);
  if (json_object_object_get_ex(root, "silent_hysteresis_vacant", &val))
    cfg->silent_hysteresis_vacant = (unsigned)json_object_get_int(val);
  if (json_object_object_get_ex(root, "silent_start", &val))
    cfg->silent_start_min = parse_hhmm(json_object_get_string(val));
  if (json_object_object_get_ex(root, "silent_end", &val))
    cfg->silent_end_min = parse_hhmm(json_object_get_string(val));

  // A silent period needs both bounds; if either is missing/malformed, treat
  // the whole period as disabled so behaviour falls back to normal hysteresis.
  if (cfg->silent_start_min < 0 || cfg->silent_end_min < 0) {
    cfg->silent_start_min = -1;
    cfg->silent_end_min = -1;
  }

  json_object_put(root);
  printf("Read config from '%s' (sensor_stabilization_delay=%us "
         "hyst_occupied=%u hyst_vacant=%u)\n",
         path, cfg->sensor_stabilization_delay_secs, cfg->hysteresis_occupied,
         cfg->hysteresis_vacant);
  if (cfg->silent_start_min >= 0)
    printf("  silent period %02d:%02d-%02d:%02d (hyst_occupied=%u "
           "hyst_vacant=%u)\n",
           cfg->silent_start_min / 60, cfg->silent_start_min % 60,
           cfg->silent_end_min / 60, cfg->silent_end_min % 60,
           cfg->silent_hysteresis_occupied, cfg->silent_hysteresis_vacant);
  else
    printf("  silent period disabled\n");
  return 0;
}
