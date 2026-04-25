#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <systemd/sd-event.h>

// Inbound D-Bus surface for ambience. Owns its own system-bus connection
// (attached to the shared sd_event) plus the well-known name
// io.homeboard.Ambience, dispatches method calls, and subscribes to the
// external signals ambience cares about: presence state changes, and
// name-owner changes for the presence and photo-provider services. Every
// external event reaches the orchestrator through the single
// `dbus_listeners_cbs` struct.
//
// Outbound D-Bus to other services lives in dedicated client modules
// (photo_client) that own their own bus connections. Outbound *signals*
// declared on the io.homeboard.Ambience1 interface (DisplayingPhoto,
// SlideshowActive) are emitted from here, on the bus that owns the name.

struct DBusListeners;

struct dbus_listeners_cbs {
  // io.homeboard.Ambience1 method calls.
  void (*on_next)(void *ud);
  void (*on_prev)(void *ud);
  // Returns false on invalid value (caller reports InvalidArgs).
  bool (*on_set_transition_time)(void *ud, uint32_t seconds);
  // Returns 0 on success, negative errno on failure (-EINVAL, -EBUSY, ...).
  int (*on_set_render_config)(void *ud, uint32_t rotation,
                              const char *interpolation,
                              const char *horizontal_align,
                              const char *vertical_align);
  int (*on_announce)(void *ud, uint32_t timeout_seconds, const char *msg);

  // External signals.
  void (*on_presence_changed)(void *ud, bool present);
  // Invoked when the presence / photo-provider services appear or disappear
  // from the bus. `up=true` means the service is now reachable.
  void (*on_presence_service_updown)(void *ud, bool up);
  void (*on_photo_service_updown)(void *ud, bool up);
};

// `event` is borrowed; the caller owns it and must outlive the listeners.
// `cbs` is copied; `ud` is shared across every callback.
struct DBusListeners *dbus_listeners_init(sd_event *event,
                                          const struct dbus_listeners_cbs *cbs,
                                          void *ud);
void dbus_listeners_free(struct DBusListeners *d);

// Emit SlideshowActive(b) / DisplayingPhoto(s) on io.homeboard.Ambience1.
int dbus_listeners_emit_slideshow_active(struct DBusListeners *d, bool active);
int dbus_listeners_emit_displaying_photo(struct DBusListeners *d,
                                         const char *meta);
