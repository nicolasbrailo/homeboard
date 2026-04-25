#pragma once

#include <stdbool.h>

// Inbound D-Bus surface for presence-service. Owns its own system-bus
// connection plus the io.homeboard.Presence well-known name, exposes
// ForceOn / ForceOff methods, and emits PresenceChanged. Subscribes to:
//   - io.homeboard.Occupancy1.Report(b occupied, u distance)
//   - NameOwnerChanged for io.homeboard.Occupancy

struct PresenceDBus;

struct presence_dbus_cbs {
  void (*on_force_on)(void *ud);
  void (*on_force_off)(void *ud);
  // Raw report from the occupancy sensor.
  void (*on_sensor_report)(void *ud, bool occupied);
  // Sensor service appeared (up=true) or disappeared (up=false).
  void (*on_sensor_updown)(void *ud, bool up);
};

struct PresenceDBus *presence_dbus_init(const struct presence_dbus_cbs *cbs,
                                        void *ud);
void presence_dbus_free(struct PresenceDBus *d);

int presence_dbus_run_once(struct PresenceDBus *d);

// Emit PresenceChanged(b) on io.homeboard.Presence1.
int presence_dbus_emit_changed(struct PresenceDBus *d, bool present);
