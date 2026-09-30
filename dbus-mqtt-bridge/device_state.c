#include "device_state.h"
#include "mqtt.h"

#include <json-c/json.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

// The LD2410S gates are about 75 cm apart, so smaller moves are mostly noise
#define DISTANCE_STEP_CM 50
#define WIFI_STEP_DBM 5
#define WIFI_POLL_S 10

// photo-provider holds these in fixed buffers of this size plus a NUL
#define ALBUM_FILTER_STR_CAP 511

struct rc_state {
  struct rc_mqtt *mqtt;

  // -1 = unknown (null)
  int occupied;
  int distance_cm;
  int slideshow_active;

  bool wifi_rssi_valid;
  int wifi_rssi;

  bool have_album_filter;
  char album_name[ALBUM_FILTER_STR_CAP + 1];
  char album_exclude[ALBUM_FILTER_STR_CAP + 1];
  uint32_t album_from_year;
  uint32_t album_to_year;

  time_t next_wifi_poll;
  char *last_published; // serialized without ts
};

struct rc_state *rc_state_new(struct rc_mqtt *mqtt) {
  struct rc_state *s = calloc(1, sizeof(*s));
  if (!s)
    return NULL;
  s->mqtt = mqtt;
  s->occupied = -1;
  s->distance_cm = -1;
  s->slideshow_active = -1;
  return s;
}

void rc_state_free(struct rc_state *s) {
  if (!s)
    return;
  free(s->last_published);
  free(s);
}

static struct json_object *tri(int v) {
  return v < 0 ? NULL : json_object_new_boolean(v);
}

static struct json_object *opt_int(bool valid, int v) {
  return valid ? json_object_new_int(v) : NULL;
}

static char *to_json(const struct rc_state *s, long long ts) {
  struct json_object *root = json_object_new_object();
  if (!root)
    return NULL;

  struct json_object *occ = json_object_new_object();
  json_object_object_add(occ, "occupied", tri(s->occupied));
  json_object_object_add(occ, "source", json_object_new_string("mmwave"));
  json_object_object_add(occ, "distance_cm",
                         opt_int(s->distance_cm >= 0, s->distance_cm));
  json_object_object_add(root, "occupancy", occ);

  struct json_object *slide = json_object_new_object();
  json_object_object_add(slide, "active", tri(s->slideshow_active));
  json_object_object_add(slide, "shown_in", NULL);
  json_object_object_add(slide, "night_cover", NULL);
  struct json_object *filter = NULL;
  if (s->have_album_filter) {
    filter = json_object_new_object();
    json_object_object_add(filter, "name",
                           json_object_new_string(s->album_name));
    json_object_object_add(filter, "exclude",
                           json_object_new_string(s->album_exclude));
    json_object_object_add(filter, "from_year",
                           json_object_new_int64(s->album_from_year));
    json_object_object_add(filter, "to_year",
                           json_object_new_int64(s->album_to_year));
  }
  json_object_object_add(slide, "album_filter", filter);
  json_object_object_add(root, "slideshow", slide);

  json_object_object_add(root, "screen", NULL);
  json_object_object_add(root, "errors", NULL);
  json_object_object_add(root, "battery", NULL);
  json_object_object_add(root, "wifi_rssi",
                         opt_int(s->wifi_rssi_valid, s->wifi_rssi));
  json_object_object_add(root, "light_lux", NULL);
  json_object_object_add(root, "app", NULL);
  if (ts >= 0)
    json_object_object_add(root, "ts", json_object_new_int64(ts));

  const char *str = json_object_to_json_string_ext(
      root, JSON_C_TO_STRING_PLAIN | JSON_C_TO_STRING_NOSLASHESCAPE);
  char *ret = str ? strdup(str) : NULL;
  json_object_put(root);
  return ret;
}

// Publishes the record if it differs from the last one published, ts aside
static void publish(struct rc_state *s) {
  char *cur = to_json(s, -1);
  if (!cur)
    return;
  if (s->last_published && strcmp(cur, s->last_published) == 0) {
    free(cur);
    return;
  }
  char *full = to_json(s, (long long)time(NULL));
  if (full &&
      rc_mqtt_publish(s->mqtt, "state", full, strlen(full), true) == 0) {
    free(s->last_published);
    s->last_published = cur;
    cur = NULL;
  }
  free(full);
  free(cur);
}

void rc_state_set_occupancy(struct rc_state *s, bool occupied,
                            uint32_t distance_cm) {
  s->occupied = occupied;
  // 0 means no distance: the sensor only measures it with UART enabled
  if (!occupied || distance_cm == 0 || distance_cm > INT32_MAX) {
    s->distance_cm = -1;
  } else if (s->distance_cm < 0 ||
             abs((int)distance_cm - s->distance_cm) >= DISTANCE_STEP_CM) {
    s->distance_cm = (int)distance_cm;
  }
  publish(s);
}

void rc_state_set_slideshow_active(struct rc_state *s, bool active) {
  s->slideshow_active = active;
  publish(s);
}

void rc_state_set_album_filter(struct rc_state *s, const char *name,
                               const char *exclude, uint32_t from_year,
                               uint32_t to_year) {
  s->have_album_filter = true;
  snprintf(s->album_name, sizeof(s->album_name), "%s", name);
  snprintf(s->album_exclude, sizeof(s->album_exclude), "%s", exclude);
  s->album_from_year = from_year;
  s->album_to_year = to_year;
  publish(s);
}

// Signal level of the first interface in /proc/net/wireless, in dBm. Returns
// false without a usable reading (no wlan, not associated).
static bool read_wifi_rssi(int *dbm) {
  FILE *f = fopen("/proc/net/wireless", "r");
  if (!f)
    return false;
  // Two header lines, then one per interface:
  //  wlan0: 0000   70.  -40.  -256        0      0 ...
  char line[256];
  bool ok = false;
  for (int n = 0; fgets(line, sizeof(line), f); n++) {
    const char *p = strchr(line, ':');
    if (n < 2 || !p)
      continue;
    float link, level;
    if (sscanf(p + 1, " %*x %f %f", &link, &level) == 2 && level < 0) {
      *dbm = (int)(level - 0.5f);
      ok = true;
    }
    break;
  }
  fclose(f);
  return ok;
}

void rc_state_poll_wifi(struct rc_state *s) {
  struct timespec now;
  clock_gettime(CLOCK_MONOTONIC, &now);
  if (now.tv_sec < s->next_wifi_poll)
    return;
  s->next_wifi_poll = now.tv_sec + WIFI_POLL_S;

  int dbm;
  if (!read_wifi_rssi(&dbm)) {
    s->wifi_rssi_valid = false;
  } else if (!s->wifi_rssi_valid || abs(dbm - s->wifi_rssi) >= WIFI_STEP_DBM) {
    s->wifi_rssi_valid = true;
    s->wifi_rssi = dbm;
  }
  publish(s);
}
