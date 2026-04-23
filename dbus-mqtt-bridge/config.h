#pragma once

#include <stdint.h>

struct rc_config {
  char mqtt_host[128];
  uint16_t mqtt_port;
  char mqtt_client_id[64];
  char mqtt_user[64];
  char mqtt_pass[128];
  uint16_t mqtt_keepalive_s;
  char topic_prefix[64];
};

int rc_config_load(const char *path, struct rc_config *cfg);
