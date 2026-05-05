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

// Format the host info as a JSON object. `state` is required and is emitted
// as the first field ("state":"online" / "state":"offline"); it lets a single
// retained payload double as both the live indicator and the LWT.
int rc_host_info_format_json(const struct rc_host_info *info, const char *state,
                             char *buf, size_t buf_sz);
