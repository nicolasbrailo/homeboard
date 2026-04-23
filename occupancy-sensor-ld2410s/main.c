#include "ld2410s.h"

#include <json-c/json.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <systemd/sd-bus.h>
#include <time.h>
#include <unistd.h>

#define DBUS_SERVICE "io.homeboard.Occupancy"
#define DBUS_PATH "/io/homeboard/Occupancy"
#define DBUS_INTERFACE "io.homeboard.Occupancy1"

struct config {
  struct LD2410S_config sensor;
  uint16_t cal_trigger;
  uint16_t cal_retention;
  uint16_t cal_duration;
};

static sd_bus *g_bus;
static volatile sig_atomic_t g_quit;

static void sig_handler(int sig) {
  (void)sig;
  g_quit = 1;
}

static int dbus_init(void) {
  int r = sd_bus_open_system(&g_bus);
  if (r < 0) {
    fprintf(stderr, "sd_bus_open_system: %s\n", strerror(-r));
    return -1;
  }
  r = sd_bus_request_name(g_bus, DBUS_SERVICE, 0);
  if (r < 0) {
    fprintf(stderr, "sd_bus_request_name(%s): %s\n", DBUS_SERVICE, strerror(-r));
    sd_bus_unref(g_bus);
    g_bus = NULL;
    return -1;
  }
  return 0;
}

static void on_state_change(bool occupied, uint16_t distance, void *ud) {
  (void)ud;
  time_t now = time(NULL);
  struct tm tm;
  localtime_r(&now, &tm);
  printf("%02d:%02d:%02d Occupancy=%s Distance=%u\n", tm.tm_hour, tm.tm_min, tm.tm_sec, occupied ? "True" : "False",
         distance);

  if (!g_bus)
    return;
  int r = sd_bus_emit_signal(g_bus, DBUS_PATH, DBUS_INTERFACE, "StateChanged", "bu", (int)occupied, (uint32_t)distance);
  if (r < 0)
    fprintf(stderr, "sd_bus_emit_signal: %s\n", strerror(-r));
  sd_bus_flush(g_bus);
}

static int load_config(const char *path, struct config *cfg) {
  struct json_object *root = json_object_from_file(path);
  if (!root) {
    fprintf(stderr, "Failed to parse config: %s\n", path);
    return -1;
  }

  cfg->sensor.enable_uart = false;
  strncpy(cfg->sensor.device, "/dev/ttyUSB0", sizeof(cfg->sensor.device));
  cfg->sensor.debug = false;
  cfg->sensor.sensor_report_gpio = -1;
  cfg->sensor.startup_delay_secs = 10;
  cfg->sensor.hysteresis_occupied = 3;
  cfg->sensor.hysteresis_vacant = 3;
  cfg->cal_trigger = 2;
  cfg->cal_retention = 1;
  cfg->cal_duration = 120;

  struct json_object *val;
  if (json_object_object_get_ex(root, "enable_uart", &val))
    cfg->sensor.enable_uart = json_object_get_boolean(val);
  if (json_object_object_get_ex(root, "device", &val))
    strncpy(cfg->sensor.device, json_object_get_string(val), sizeof(cfg->sensor.device) - 1);
  if (json_object_object_get_ex(root, "debug", &val))
    cfg->sensor.debug = json_object_get_boolean(val);
  if (json_object_object_get_ex(root, "sensor_report_gpio", &val))
    cfg->sensor.sensor_report_gpio = json_object_get_int(val);
  if (json_object_object_get_ex(root, "startup_delay_secs", &val))
    cfg->sensor.startup_delay_secs = (unsigned)json_object_get_int(val);
  if (json_object_object_get_ex(root, "hysteresis_occupied", &val))
    cfg->sensor.hysteresis_occupied = (unsigned)json_object_get_int(val);
  if (json_object_object_get_ex(root, "hysteresis_vacant", &val))
    cfg->sensor.hysteresis_vacant = (unsigned)json_object_get_int(val);
  if (json_object_object_get_ex(root, "calibration_trigger", &val))
    cfg->cal_trigger = (uint16_t)json_object_get_int(val);
  if (json_object_object_get_ex(root, "calibration_retention", &val))
    cfg->cal_retention = (uint16_t)json_object_get_int(val);
  if (json_object_object_get_ex(root, "calibration_duration", &val))
    cfg->cal_duration = (uint16_t)json_object_get_int(val);

  json_object_put(root);
  printf("Read config from '%s'\n", path);
  return 0;
}

static int apply_sensor_params(struct LD2410S *s, const struct config *cfg, const char *path) {
  if (!cfg->sensor.enable_uart) {
    return 0;
  }

  struct json_object *root = json_object_from_file(path);
  if (!root)
    return -1;

  struct LD2410S_common_params current;
  if (ld2410s_get_common_params(s, &current) < 0) {
    fprintf(stderr, "Failed to read current common params; writing unconditionally\n");
    memset(&current, 0xFF, sizeof(current)); /* force mismatch on every key */
  }

  struct {
    const char *key;
    uint32_t *field;
  } params[] = {
      {"farthest_gate", &current.farthest_gate},       {"nearest_gate", &current.nearest_gate},
      {"unmanned_delay", &current.unmanned_delay},     {"status_report_freq", &current.status_report_freq},
      {"dist_report_freq", &current.dist_report_freq}, {"response_speed", &current.response_speed},
  };

  printf("Verifying device config:\n");
  for (size_t i = 0; i < sizeof(params) / sizeof(params[0]); i++) {
    struct json_object *val;
    if (!json_object_object_get_ex(root, params[i].key, &val))
      continue;

    uint32_t desired = (uint32_t)json_object_get_int(val);
    if (desired == *params[i].field) {
      printf("Config: %s=%u [Expected value]\n", params[i].key, desired);
      continue;
    }

    if (ld2410s_set_param(s, params[i].key, desired) < 0)
      fprintf(stderr, "Config: Failed to set %s=%u (should stay = %u)\n", params[i].key, desired, *params[i].field);
    else
      printf("Config: Set %s=%u (was %u)\n", params[i].key, desired, *params[i].field);
  }

  json_object_put(root);
  return 0;
}

int main(int argc, char *argv[]) {
  setvbuf(stdout, NULL, _IOLBF, 0);

  if (argc < 2) {
    fprintf(stderr, "Usage: %s <config.json>\n", argv[0]);
    return 1;
  }

  struct config cfg;
  if (load_config(argv[1], &cfg) < 0)
    return 1;

  if (dbus_init() < 0)
    return 1;

  struct LD2410S *sensor = ld2410s_init(&cfg.sensor, on_state_change, NULL);
  if (!sensor) {
    sd_bus_flush_close_unref(g_bus);
    return 1;
  }

  apply_sensor_params(sensor, &cfg, argv[1]);

  signal(SIGTERM, sig_handler);
  signal(SIGINT, sig_handler);

  bool calibration_force = false;
  if (argc > 2 && strcmp(argv[2], "--calibrate") == 0) {
    printf("Will force calibration as soon as the room is empty (if UART is enabled)\n");
    calibration_force = true;
  }

  int last_cal_yday = -1;
  printf("Service is now running\n");
  while (!g_quit) {
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);

    if ((tm.tm_hour == 3 && tm.tm_yday != last_cal_yday) || calibration_force) {
      int ret = ld2410s_start_calibration(sensor, cfg.cal_trigger, cfg.cal_retention, cfg.cal_duration);
      if (ret == 0) {
        last_cal_yday = tm.tm_yday;
        calibration_force = false;
      }
    }

    sleep(1);
  }

  ld2410s_free(sensor);
  sd_bus_flush_close_unref(g_bus);
  return 0;
}
