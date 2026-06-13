#include "host_info.h"
#include "misc/net.h"

#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/utsname.h>
#include <time.h>
#include <unistd.h>

static int read_trim(const char *path, char *buf, size_t buf_sz) {
  int fd = open(path, O_RDONLY);
  if (fd < 0)
    return -1;
  ssize_t n = read(fd, buf, buf_sz - 1);
  close(fd);
  if (n < 0)
    return -1;
  buf[n] = '\0';
  while (n > 0 &&
         (buf[n - 1] == '\n' || buf[n - 1] == '\r' || buf[n - 1] == ' ' ||
          buf[n - 1] == '\t' || buf[n - 1] == '\0')) {
    buf[--n] = '\0';
  }
  return (int)n;
}

int rc_host_info_collect(struct rc_host_info *info) {
  memset(info, 0, sizeof(*info));

  if (read_trim("/etc/machine-id", info->machine_id,
                sizeof(info->machine_id)) <= 0) {
    return -1;
  }

  if (gethostname(info->hostname, sizeof(info->hostname) - 1) != 0) {
    snprintf(info->hostname, sizeof(info->hostname), "unknown");
  }
  info->hostname[sizeof(info->hostname) - 1] = '\0';

  if (read_trim("/sys/firmware/devicetree/base/model", info->host_model,
                sizeof(info->host_model)) <= 0 &&
      read_trim("/sys/class/dmi/id/product_name", info->host_model,
                sizeof(info->host_model)) <= 0) {
    struct utsname u;
    if (uname(&u) == 0) {
      snprintf(info->host_model, sizeof(info->host_model), "%s %s", u.sysname,
               u.machine);
    } else {
      snprintf(info->host_model, sizeof(info->host_model), "unknown");
    }
  }

  collect_ip(info->ip, sizeof(info->ip));

  info->started_at = time(NULL);
  return 0;
}

static size_t json_escape(const char *src, char *dst, size_t dst_sz) {
  size_t j = 0;
  if (dst_sz == 0)
    return 0;
  for (size_t i = 0; src[i] && j + 2 < dst_sz; i++) {
    unsigned char c = (unsigned char)src[i];
    if (c == '"' || c == '\\') {
      if (j + 3 >= dst_sz)
        break;
      dst[j++] = '\\';
      dst[j++] = (char)c;
    } else if (c < 0x20) {
      // Drop control characters; they shouldn't appear in hostnames or models.
      continue;
    } else {
      dst[j++] = (char)c;
    }
  }
  dst[j] = '\0';
  return j;
}

int rc_host_info_format_online_json(const struct rc_host_info *info, char *buf,
                                    size_t buf_sz) {
  char hostname_e[256];
  char model_e[256];
  char ip_e[64];
  json_escape(info->hostname, hostname_e, sizeof(hostname_e));
  json_escape(info->host_model, model_e, sizeof(model_e));
  json_escape(info->ip, ip_e, sizeof(ip_e));

  char iso[32] = "";
  struct tm tm;
  if (gmtime_r(&info->started_at, &tm) != NULL) {
    strftime(iso, sizeof(iso), "%Y-%m-%dT%H:%M:%SZ", &tm);
  }

  int n;
  if (ip_e[0]) {
    n = snprintf(buf, buf_sz,
                 "{\"state\":\"online\","
                 "\"machine_id\":\"%s\",\"hostname\":\"%s\","
                 "\"ip\":\"%s\",\"host_model\":\"%s\","
                 "\"started_at\":%lld,\"started_at_iso\":\"%s\"}",
                 info->machine_id, hostname_e, ip_e, model_e,
                 (long long)info->started_at, iso);
  } else {
    n = snprintf(buf, buf_sz,
                 "{\"state\":\"online\","
                 "\"machine_id\":\"%s\",\"hostname\":\"%s\","
                 "\"host_model\":\"%s\","
                 "\"started_at\":%lld,\"started_at_iso\":\"%s\"}",
                 info->machine_id, hostname_e, model_e,
                 (long long)info->started_at, iso);
  }
  if (n < 0 || (size_t)n >= buf_sz)
    return -1;
  return n;
}

int rc_host_info_format_offline_json(const struct rc_host_info *info, char *buf,
                                     size_t buf_sz) {
  char hostname_e[256];
  char model_e[256];
  json_escape(info->hostname, hostname_e, sizeof(hostname_e));
  json_escape(info->host_model, model_e, sizeof(model_e));

  int n = snprintf(buf, buf_sz,
                   "{\"state\":\"offline\","
                   "\"machine_id\":\"%s\",\"hostname\":\"%s\","
                   "\"host_model\":\"%s\","
                   "\"started_at\":%lld}",
                   info->machine_id, hostname_e, model_e,
                   (long long)info->started_at);
  if (n < 0 || (size_t)n >= buf_sz)
    return -1;
  return n;
}
