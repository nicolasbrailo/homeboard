#pragma once

#include "jpeg_render/img_render.h"
#include <stdbool.h>
#include <stdint.h>

struct DBusListeners;

struct dbus_listeners_cbs {
  // io.homeboard.Ambience1 method calls.
  void (*on_next)(void *ud);
  void (*on_prev)(void *ud);
  // Returns false on invalid value (caller reports InvalidArgs).
  bool (*on_set_transition_time)(void *ud, uint32_t seconds);
  // Returns 0 on success, negative errno on failure (-EINVAL, -EBUSY, ...).
  int (*on_set_render_config)(void *ud, const struct img_render_cfg *cfg);

  int (*on_overlay_requested)(void *ud, uint32_t timeout_seconds,
                              const char *svg);
  // Local test hook: load the SVG overlay directly from a file on the device.
  int (*on_overlay_from_file)(void *ud, uint32_t timeout_seconds,
                              const char *path);

  // Presence status change (present=true => someone is in the room, false =>
  // room is vacant)
  void (*on_presence_changed)(void *ud, bool present);

  // Invoked when a service we depend on goes away or appears from the bus.
  // `up=true` means the service is now reachable.
  void (*on_presence_service_updown)(void *ud, bool up);
  void (*on_photo_service_updown)(void *ud, bool up);
  void (*on_drm_mgr_updown)(void *ud, bool up);
};

struct DBusListeners *dbus_listeners_init(const struct dbus_listeners_cbs *cbs,
                                          void *ud, bool *all_deps_ready);
void dbus_listeners_free(struct DBusListeners *d);
int dbus_listeners_run_once(struct DBusListeners *s, int timeout_ms);
