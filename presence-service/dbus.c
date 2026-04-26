#include "dbus.h"
#include "dbus_helpers/dbus_helpers.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <systemd/sd-bus.h>

#define DBUS_PRESENCE_SERVICE "io.homeboard.Presence"
#define DBUS_PRESENCE_PATH "/io/homeboard/Presence"
#define DBUS_PRESENCE_INTERFACE "io.homeboard.Presence1"

#define DBUS_OCCUPANCY_SERVICE "io.homeboard.Occupancy"
#define DBUS_OCCUPANCY_PATH "/io/homeboard/Occupancy"
#define DBUS_OCCUPANCY_INTERFACE "io.homeboard.Occupancy1"
#define DBUS_OCCUPANCY_SIGNAL "Report"

struct PresenceDBus {
  sd_bus *bus;
  sd_bus_slot *vtable_slot;
  sd_bus_slot *sensor_signal_slot;
  sd_bus_slot *sensor_updown_slot;
  struct presence_dbus_cbs cbs;
  void *ud;
};

static int method_force_on(sd_bus_message *m, void *userdata,
                           sd_bus_error *err) {
  struct PresenceDBus *d = userdata;
  d->cbs.on_force_on(d->ud);
  return sd_bus_reply_method_return(m, NULL);
}

static int method_force_off(sd_bus_message *m, void *userdata,
                            sd_bus_error *err) {
  struct PresenceDBus *d = userdata;
  d->cbs.on_force_off(d->ud);
  return sd_bus_reply_method_return(m, NULL);
}

static const sd_bus_vtable g_vtable[] = {
    SD_BUS_VTABLE_START(0),
    SD_BUS_METHOD("ForceOn", "", "", method_force_on,
                  SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("ForceOff", "", "", method_force_off,
                  SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_SIGNAL("PresenceChanged", "b", 0),
    SD_BUS_VTABLE_END,
};

static int on_sensor_report(sd_bus_message *m, void *userdata,
                            sd_bus_error *err) {
  struct PresenceDBus *d = userdata;
  int occupied_raw = 0;
  uint32_t distance = 0;
  // sd_bus_message_read's "b" reads into int* (4 bytes), not bool*.
  const int r = sd_bus_message_read(m, "bu", &occupied_raw, &distance);
  if (r < 0) {
    fprintf(stderr, "occupancy Report read: %s\n", strerror(-r));
    return 0;
  }
  d->cbs.on_sensor_report(d->ud, occupied_raw != 0);
  return 0;
}

static void on_sensor_updown(void *ud, bool up) {
  struct PresenceDBus *d = ud;
  d->cbs.on_sensor_updown(d->ud, up);
}

struct PresenceDBus *presence_dbus_init(const struct presence_dbus_cbs *cbs,
                                        void *ud) {
  if (!cbs)
    return NULL;
  struct PresenceDBus *d = calloc(1, sizeof(*d));
  if (!d)
    return NULL;
  d->cbs = *cbs;
  d->ud = ud;

  int r = sd_bus_open_system(&d->bus);
  if (r < 0) {
    fprintf(stderr, "sd_bus_open_system: %s\n", strerror(-r));
    presence_dbus_free(d);
    return NULL;
  }

  r = sd_bus_add_object_vtable(d->bus, &d->vtable_slot, DBUS_PRESENCE_PATH,
                               DBUS_PRESENCE_INTERFACE, g_vtable, d);
  if (r < 0) {
    fprintf(stderr, "sd_bus_add_object_vtable: %s\n", strerror(-r));
    presence_dbus_free(d);
    return NULL;
  }
  r = sd_bus_request_name(d->bus, DBUS_PRESENCE_SERVICE, 0);
  if (r < 0) {
    fprintf(stderr, "sd_bus_request_name(%s): %s\n", DBUS_PRESENCE_SERVICE,
            strerror(-r));
    presence_dbus_free(d);
    return NULL;
  }
  printf("Presence service offered at %s\n", DBUS_PRESENCE_SERVICE);

  r = sd_bus_match_signal(d->bus, &d->sensor_signal_slot,
                          DBUS_OCCUPANCY_SERVICE, DBUS_OCCUPANCY_PATH,
                          DBUS_OCCUPANCY_INTERFACE, DBUS_OCCUPANCY_SIGNAL,
                          on_sensor_report, d);
  if (r < 0) {
    fprintf(stderr, "sd_bus_match_signal(occupancy): %s\n", strerror(-r));
    presence_dbus_free(d);
    return NULL;
  }

  d->sensor_updown_slot =
      on_service_updown(d->bus, DBUS_OCCUPANCY_SERVICE, on_sensor_updown, d);
  if (!is_service_up(d->bus, DBUS_OCCUPANCY_SERVICE)) {
    fprintf(stderr,
            "WARNING: %s is not running; no occupancy reports will arrive "
            "until it starts\n",
            DBUS_OCCUPANCY_SERVICE);
  }

  return d;
}

void presence_dbus_free(struct PresenceDBus *d) {
  if (!d)
    return;
  if (d->vtable_slot)
    sd_bus_slot_unref(d->vtable_slot);
  if (d->sensor_signal_slot)
    sd_bus_slot_unref(d->sensor_signal_slot);
  if (d->sensor_updown_slot)
    sd_bus_slot_unref(d->sensor_updown_slot);
  if (d->bus)
    sd_bus_flush_close_unref(d->bus);
  free(d);
}

int presence_dbus_run_once(struct PresenceDBus *d) {
  int r = sd_bus_process(d->bus, NULL);
  if (r < 0) {
    fprintf(stderr, "sd_bus_process: %s\n", strerror(-r));
    return r;
  }
  if (r > 0)
    return 0;
  r = sd_bus_wait(d->bus, (uint64_t)-1);
  if (r < 0 && -r != EINTR) {
    fprintf(stderr, "sd_bus_wait: %s\n", strerror(-r));
    return r;
  }
  return 0;
}

int presence_dbus_emit_changed(struct PresenceDBus *d, bool present) {
  if (!d || !d->bus)
    return -EINVAL;
  int r =
      sd_bus_emit_signal(d->bus, DBUS_PRESENCE_PATH, DBUS_PRESENCE_INTERFACE,
                         "PresenceChanged", "b", (int)(present ? 1 : 0));
  if (r < 0)
    fprintf(stderr, "emit PresenceChanged: %s\n", strerror(-r));
  return r;
}
