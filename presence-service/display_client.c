#include "display_client.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <systemd/sd-bus.h>

#define DBUS_DISPLAY_SERVICE "io.homeboard.Display"
#define DBUS_DISPLAY_PATH "/io/homeboard/Display"
#define DBUS_DISPLAY_INTERFACE "io.homeboard.Display1"

struct DisplayClient {
  sd_bus *bus;
};

struct DisplayClient *display_client_init(void) {
  struct DisplayClient *c = calloc(1, sizeof(*c));
  if (!c)
    return NULL;
  int r = sd_bus_open_system(&c->bus);
  if (r < 0) {
    fprintf(stderr, "display_client: sd_bus_open_system: %s\n", strerror(-r));
    free(c);
    return NULL;
  }
  return c;
}

void display_client_free(struct DisplayClient *c) {
  if (!c)
    return;
  if (c->bus)
    sd_bus_flush_close_unref(c->bus);
  free(c);
}

static int call(struct DisplayClient *c, const char *method) {
  if (!c || !c->bus)
    return -1;
  sd_bus_error err = SD_BUS_ERROR_NULL;
  sd_bus_message *reply = NULL;
  const int r =
      sd_bus_call_method(c->bus, DBUS_DISPLAY_SERVICE, DBUS_DISPLAY_PATH,
                         DBUS_DISPLAY_INTERFACE, method, &err, &reply, "");
  if (r < 0)
    fprintf(stderr, "Failed to call Display.%s: %s\n", method,
            err.message ? err.message : strerror(-r));
  else
    printf("Called Display.%s()\n", method);
  sd_bus_error_free(&err);
  sd_bus_message_unref(reply);
  return r;
}

int display_client_on(struct DisplayClient *c) { return call(c, "On"); }
int display_client_off(struct DisplayClient *c) { return call(c, "Off"); }
