#include "display.h"
#include "dbus_helpers.h"

#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <systemd/sd-bus.h>
#include <time.h>

#define DBUS_OCCUPANCY_SERVICE "io.homeboard.Occupancy"
#define DBUS_OCCUPANCY_PATH "/io/homeboard/Occupancy"
#define DBUS_OCCUPANCY_INTERFACE "io.homeboard.Occupancy1"
#define DBUS_OCCUPANCY_SIGNAL "StateChanged"

#define DBUS_DISPLAY_SERVICE "io.homeboard.Display"
#define DBUS_DISPLAY_PATH "/io/homeboard/Display"
#define DBUS_DISPLAY_INTERFACE "io.homeboard.Display1"

struct Display {
  sd_bus *dbus;
  sd_bus_slot *occupancy_monitor;
  sd_bus_slot *occupancy_name_owner_monitor;

  display_state_cb on_display_turned_on;
  display_state_cb on_display_turned_off;
  void *ud;

  struct OccupancyReport {
    bool occupied;
    int distance;
  } last_occupancy_report;

  // When true, suppress real occupied=true reports. Cleared by a real
  // occupied=false report (or occupancy service dropping out), or by
  // display_force_on. Set by display_force_off.
  bool force_off_latched;

  int requested_state;
};

static void update_display_state(struct Display *display) {
  if (display->last_occupancy_report.occupied && display->requested_state == 1) {
    // Do nothing, display already in expected state
    return;
  } else if (!display->last_occupancy_report.occupied && display->requested_state == 0) {
    // Do nothing, display already in expected state
    return;
  }

  // Display in unknown state, or state doesn't match presence status
  sd_bus_error err = SD_BUS_ERROR_NULL;
  sd_bus_message *reply = NULL;
  const char *method = display->last_occupancy_report.occupied == true ? "On" : "Off";
  const int r = sd_bus_call_method(display->dbus, DBUS_DISPLAY_SERVICE, DBUS_DISPLAY_PATH, DBUS_DISPLAY_INTERFACE,
                                   method, &err, &reply, "");
  if (r < 0) {
    fprintf(stderr, "Failed to call Display.%s: %s\n", method, err.message ? err.message : strerror(-r));
    display->requested_state = -1;
  } else {
    printf("Occupancy state changed, calling Display.%s()\n", method);
    display->requested_state = display->last_occupancy_report.occupied ? 1 : 0;
    display_state_cb cb =
        display->last_occupancy_report.occupied ? display->on_display_turned_on : display->on_display_turned_off;
    cb(display->ud);
  }

  sd_bus_error_free(&err);
  sd_bus_message_unref(reply);
}

static int on_occupancy_state_changed(sd_bus_message *m, void *userdata, sd_bus_error *ret_error) {
  struct Display *display = userdata;
  // sd_bus_message_read's "b" reads into int* (4 bytes), not bool* — writing
  // into a bool local would corrupt the stack.
  int occupied_raw = 0;
  uint32_t distance = 0;
  const int r = sd_bus_message_read(m, "bu", &occupied_raw, &distance);
  if (r < 0) {
    fprintf(stderr, "sd_bus_message_read: %s\n", strerror(-r));
    return 0;
  }
  const bool occupied = occupied_raw != 0;

  if (display->force_off_latched && occupied) {
    printf("Occupancy=true report suppressed (force-off latched)\n");
    return 0;
  }
  if (!occupied)
    display->force_off_latched = false;

  display->last_occupancy_report.occupied = occupied;
  display->last_occupancy_report.distance = (int)distance;
  update_display_state(display);
  return 0;
}

static void on_occupancy_name_owner_changed(void *ud, bool up) {
  if (up)
    return;
  fprintf(stderr, "WARNING: Occupancy service is down, assuming no presence (and slideshow will shutdown)\n");
  struct Display *display = ud;
  display->force_off_latched = false;
  display->last_occupancy_report.occupied = false;
  display->last_occupancy_report.distance = 0;
  update_display_state(display);
}

struct Display *display_init(sd_bus *bus, display_state_cb on_display_turned_on, display_state_cb on_display_turned_off,
                             void *ud) {
  struct Display *s = calloc(1, sizeof(*s));
  if (!s) {
    return NULL;
  }

  s->dbus = bus;
  s->on_display_turned_on = on_display_turned_on;
  s->on_display_turned_off = on_display_turned_off;
  s->ud = ud;

  int r = sd_bus_match_signal(s->dbus, &s->occupancy_monitor, DBUS_OCCUPANCY_SERVICE, DBUS_OCCUPANCY_PATH,
                              DBUS_OCCUPANCY_INTERFACE, DBUS_OCCUPANCY_SIGNAL, on_occupancy_state_changed, s);
  if (r < 0) {
    fprintf(stderr, "sd_bus_match_signal: %s\n", strerror(-r));
    display_free(s);
    return NULL;
  }

  s->occupancy_name_owner_monitor =
      on_service_updown(s->dbus, DBUS_OCCUPANCY_SERVICE, on_occupancy_name_owner_changed, s);
  if (!is_service_up(s->dbus, DBUS_OCCUPANCY_SERVICE)) {
    fprintf(stderr, "WARNING: %s is not running; no occupancy signals will arrive until it starts\n",
            DBUS_OCCUPANCY_SERVICE);
  }

  printf("Display controller now monitoring occupancy signals %s.%s\n", DBUS_OCCUPANCY_INTERFACE,
         DBUS_OCCUPANCY_SIGNAL);
  return s;
}

void display_force_on(struct Display *s) {
  printf("Force slideshow on\n");
  s->force_off_latched = false;
  s->last_occupancy_report.occupied = true;
  s->last_occupancy_report.distance = 0;
  update_display_state(s);
}

void display_force_off(struct Display *s) {
  printf("Force slideshow off (latched until next occupied=false report)\n");
  s->force_off_latched = true;
  s->last_occupancy_report.occupied = false;
  s->last_occupancy_report.distance = 0;
  update_display_state(s);
}

void display_free(struct Display *s) {
  if (!s)
    return;
  if (s->occupancy_monitor)
    sd_bus_slot_unref(s->occupancy_monitor);
  if (s->occupancy_name_owner_monitor)
    sd_bus_slot_unref(s->occupancy_name_owner_monitor);

  // Try to shutdown the display once this service goes down
  sd_bus_error err = SD_BUS_ERROR_NULL;
  sd_bus_message *reply = NULL;
  const int r = sd_bus_call_method(s->dbus, DBUS_DISPLAY_SERVICE, DBUS_DISPLAY_PATH, DBUS_DISPLAY_INTERFACE, "Off",
                                   &err, &reply, "");
  if (r < 0) {
    fprintf(stderr, "Failed to call Display.Off: %s\n", err.message ? err.message : strerror(-r));
  }

  free(s);
}
