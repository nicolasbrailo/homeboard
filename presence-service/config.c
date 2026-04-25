#include "config.h"

#include <json-c/json.h>
#include <stdio.h>

int presence_config_load(const char *path, struct presence_config *cfg) {
  cfg->sensor_stabilization_delay_secs = 10;
  cfg->hysteresis_occupied = 5;
  cfg->hysteresis_vacant = 9;

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

  json_object_put(root);
  printf("Read config from '%s' (sensor_stabilization_delay=%us "
         "hyst_occupied=%u hyst_vacant=%u)\n",
         path, cfg->sensor_stabilization_delay_secs, cfg->hysteresis_occupied,
         cfg->hysteresis_vacant);
  return 0;
}
