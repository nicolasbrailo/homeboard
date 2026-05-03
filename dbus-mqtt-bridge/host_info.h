#pragma once

#include <stddef.h>
#include <time.h>

struct rc_host_info {
  char machine_id[64];
  char hostname[128];
  char host_model[128];
  char ip[64];
  time_t started_at;
};

// Upper bound on bytes written by rc_host_info_format_json, accounting for
// worst-case JSON escaping (2x) of string fields plus syntax and timestamp.
#define RC_HOST_INFO_JSON_MAX 1024

int rc_host_info_collect(struct rc_host_info *info);

int rc_host_info_format_json(const struct rc_host_info *info, char *buf,
                             size_t buf_sz);
