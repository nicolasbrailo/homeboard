#include <stdio.h>
#include <string.h>
#include <systemd/sd-bus.h>

#define DBUS_SERVICE "io.homeboard.Display"
#define DBUS_PATH "/io/homeboard/Display"
#define DBUS_INTERFACE "io.homeboard.Display1"

int main(int argc, char *argv[]) {
  if (argc != 2 || !(strcmp(argv[1], "on") == 0 || strcmp(argv[1], "off") == 0 || strcmp(argv[1], "status") == 0)) {
    fprintf(stderr, "Usage: %s on|off|status\n", argv[0]);
    return 1;
  }

  sd_bus *bus = NULL;
  int r = sd_bus_open_system(&bus);
  if (r < 0) {
    fprintf(stderr, "sd_bus_open_system: %s\n", strerror(-r));
    return 1;
  }

  sd_bus_error err = SD_BUS_ERROR_NULL;
  sd_bus_message *reply = NULL;
  int rc = 1;

  if (strcmp(argv[1], "status") == 0) {
    r = sd_bus_call_method(bus, DBUS_SERVICE, DBUS_PATH, DBUS_INTERFACE, "Status", &err, &reply, "");
    if (r < 0) {
      fprintf(stderr, "Status: %s\n", err.message ? err.message : strerror(-r));
      goto out;
    }
    const char *state = NULL;
    if (sd_bus_message_read(reply, "s", &state) < 0) {
      fprintf(stderr, "failed to read reply\n");
      goto out;
    }
    printf("%s\n", state);
  } else {
    const char *method = strcmp(argv[1], "on") == 0 ? "On" : "Off";
    r = sd_bus_call_method(bus, DBUS_SERVICE, DBUS_PATH, DBUS_INTERFACE, method, &err, &reply, "");
    if (r < 0) {
      fprintf(stderr, "%s: %s\n", method, err.message ? err.message : strerror(-r));
      goto out;
    }
    printf("ok\n");
  }
  rc = 0;

out:
  sd_bus_error_free(&err);
  sd_bus_message_unref(reply);
  sd_bus_flush_close_unref(bus);
  return rc;
}
