#include "dbus_listeners.h"
#include "dbus_helpers/dbus_helpers.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <systemd/sd-bus.h>

#define DBUS_AMBIENCE_SERVICE "io.homeboard.Ambience"
#define DBUS_AMBIENCE_PATH "/io/homeboard/Ambience"
#define DBUS_AMBIENCE_INTERFACE "io.homeboard.Ambience1"

#define DBUS_PRESENCE_SERVICE "io.homeboard.Presence"
#define DBUS_PRESENCE_PATH "/io/homeboard/Presence"
#define DBUS_PRESENCE_INTERFACE "io.homeboard.Presence1"
#define DBUS_PRESENCE_SIGNAL "PresenceChanged"

#define DBUS_PHOTO_SERVICE "io.homeboard.PhotoProvider"
#define DBUS_DRM_MGR_SERVICE "io.homeboard.Display"

struct DBusListeners {
  sd_bus *bus;

  // Own interface
  sd_bus_slot *vtable;

  // Presence service interface
  sd_bus_slot *presence_signal_slot;

  // Monitor services we depend on
  sd_bus_slot *presence_updown_slot;
  sd_bus_slot *photo_updown_slot;
  sd_bus_slot *drm_mgr_updown_slot;

  // Upstream callbacks
  struct dbus_listeners_cbs cbs;
  void *ud;
};

// Own interface callbacks
static int method_next(sd_bus_message *m, void *userdata, sd_bus_error *err) {
  struct DBusListeners *s = userdata;
  s->cbs.on_next(s->ud);
  return sd_bus_reply_method_return(m, NULL);
}

static int method_prev(sd_bus_message *m, void *userdata, sd_bus_error *err) {
  struct DBusListeners *s = userdata;
  s->cbs.on_prev(s->ud);
  return sd_bus_reply_method_return(m, NULL);
}

static int method_set_transition_time(sd_bus_message *m, void *userdata,
                                      sd_bus_error *err) {
  struct DBusListeners *s = userdata;
  uint32_t seconds = 0;
  int r = sd_bus_message_read(m, "u", &seconds);
  if (r < 0)
    return r;
  if (!s->cbs.on_set_transition_time(s->ud, seconds)) {
    sd_bus_error_setf(err, SD_BUS_ERROR_INVALID_ARGS,
                      "invalid transition time %u", seconds);
    return -EINVAL;
  }
  return sd_bus_reply_method_return(m, NULL);
}

static int method_announce(sd_bus_message *m, void *userdata,
                           sd_bus_error *err) {
  struct DBusListeners *s = userdata;
  uint32_t timeout_seconds = 0;
  const char *msg = NULL;
  int r = sd_bus_message_read(m, "us", &timeout_seconds, &msg);
  if (r < 0)
    return r;
  r = s->cbs.on_announce(s->ud, timeout_seconds, msg);
  if (r < 0) {
    const char *errmsg;
    if (r == -EINVAL)
      errmsg = "invalid timeout";
    else if (r == -EBUSY)
      errmsg = "announcement already active";
    else
      errmsg = "unknown error";
    sd_bus_error_setf(err, SD_BUS_ERROR_INVALID_ARGS, "%s", errmsg);
    return r;
  }
  return sd_bus_reply_method_return(m, NULL);
}

static int method_set_render_config(sd_bus_message *m, void *userdata,
                                    sd_bus_error *err) {
  struct DBusListeners *s = userdata;
  uint32_t rotation = 0;
  const char *interp = NULL;
  const char *h_align = NULL;
  const char *v_align = NULL;
  int r =
      sd_bus_message_read(m, "usss", &rotation, &interp, &h_align, &v_align);
  if (r < 0)
    return r;

  struct img_render_cfg cfg = {
    .rot = img_render_cfg_parse_rot(rotation),
    .interp = img_render_cfg_parse_interpolation(interp),
    .h_align = img_render_cfg_parse_horizontal_align(h_align),
    .v_align = img_render_cfg_parse_vertical_align(v_align),
  };
  r = s->cbs.on_set_render_config(s->ud, &cfg);
  if (r < 0) {
    sd_bus_error_setf(err, SD_BUS_ERROR_INVALID_ARGS,
                      "invalid render config (rotation=%u interpolation=%s "
                      "h_align=%s v_align=%s)",
                      rotation, interp ? interp : "", h_align ? h_align : "",
                      v_align ? v_align : "");
    return r;
  }
  return sd_bus_reply_method_return(m, NULL);
}

static int method_set_remote_control_server(sd_bus_message *m, void *userdata, sd_bus_error *err) {
  struct DBusListeners *d = userdata;
  const char *url = NULL;
  const char *qr_img = NULL;
  int r = sd_bus_message_read(m, "ss", &url, &qr_img);
  if (r < 0)
    return r;
  d->cbs.on_set_remote_control_server(d->ud, url ? url : "", qr_img ? qr_img : "");
  return sd_bus_reply_method_return(m, NULL);
}

static const sd_bus_vtable g_vtable[] = {
    SD_BUS_VTABLE_START(0),
    SD_BUS_METHOD("Next", "", "", method_next, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("Prev", "", "", method_prev, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("SetTransitionTimeSecs", "u", "", method_set_transition_time,
                  SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("Announce", "us", "", method_announce,
                  SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("SetRenderConfig", "usss", "", method_set_render_config,
                  SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("SetRemoteControlServer", "ss", "", method_set_remote_control_server,
                  SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_SIGNAL("DisplayingPhoto", "s", 0),
    SD_BUS_SIGNAL("SlideshowActive", "b", 0),
    SD_BUS_VTABLE_END,
};

// Presence service callbacks
static int on_presence_changed(sd_bus_message *m, void *userdata,
                               sd_bus_error *err) {
  (void)err;
  struct DBusListeners *s = userdata;
  // sd_bus_message_read's "b" reads into int* (4 bytes), not bool* — writing
  // into a bool local would corrupt the stack.
  int present_raw = 0;
  const int r = sd_bus_message_read(m, "b", &present_raw);
  if (r < 0) {
    fprintf(stderr, "presence PresenceChanged read: %s\n", strerror(-r));
    return 0;
  }
  s->cbs.on_presence_changed(s->ud, present_raw != 0);
  return 0;
}

// CBs to monitor services we depend on
static void on_presence_updown(void *ud, bool up) {
  struct DBusListeners *s = ud;
  s->cbs.on_presence_service_updown(s->ud, up);
}

static void on_photo_updown(void *ud, bool up) {
  struct DBusListeners *s = ud;
  s->cbs.on_photo_service_updown(s->ud, up);
}

static void on_drm_mgr_updown(void *ud, bool up) {
  struct DBusListeners *s = ud;
  s->cbs.on_drm_mgr_updown(s->ud, up);
}


//
// Public API
//

struct DBusListeners *
dbus_listeners_init(const struct dbus_listeners_cbs *cbs, void *ud, bool* all_deps_ready) {
  *all_deps_ready = false;
  struct DBusListeners *s = calloc(1, sizeof(*s));
  if (!s)
    return NULL;

  s->cbs = *cbs;
  s->ud = ud;

  int r = sd_bus_open_system(&s->bus);
  if (r < 0) {
    fprintf(stderr, "sd_bus_open_system: %s\n", strerror(-r));
    dbus_listeners_free(s);
    return NULL;
  }

  // Set up own-methods config
  r = sd_bus_add_object_vtable(s->bus, &s->vtable, DBUS_AMBIENCE_PATH,
                               DBUS_AMBIENCE_INTERFACE, g_vtable, s);
  if (r < 0) {
    fprintf(stderr, "sd_bus_add_object_vtable: %s\n", strerror(-r));
    dbus_listeners_free(s);
    return NULL;
  }
  r = sd_bus_request_name(s->bus, DBUS_AMBIENCE_SERVICE, 0);
  if (r < 0) {
    fprintf(stderr, "sd_bus_request_name(%s): %s\n", DBUS_AMBIENCE_SERVICE,
            strerror(-r));
    dbus_listeners_free(s);
    return NULL;
  }

  *all_deps_ready = true;

  // Listen for presence service signals
  r = sd_bus_match_signal(s->bus, &s->presence_signal_slot,
                          DBUS_PRESENCE_SERVICE, DBUS_PRESENCE_PATH,
                          DBUS_PRESENCE_INTERFACE, DBUS_PRESENCE_SIGNAL,
                          on_presence_changed, s);
  if (r < 0) {
    fprintf(stderr, "sd_bus_match_signal(presence): %s\n", strerror(-r));
    dbus_listeners_free(s);
    return NULL;
  }
  s->presence_updown_slot = on_service_updown(s->bus, DBUS_PRESENCE_SERVICE, on_presence_updown, s);
  if (!is_service_up(s->bus, DBUS_PRESENCE_SERVICE)) {
    fprintf(stderr,
            "WARNING: %s not running; no presence signals until it starts\n",
            DBUS_PRESENCE_SERVICE);
    *all_deps_ready = false;
  }

  // Listen for photo provider signals
  s->photo_updown_slot = on_service_updown(s->bus, DBUS_PHOTO_SERVICE, on_photo_updown, s);
  if (!is_service_up(s->bus, DBUS_PHOTO_SERVICE)) {
    fprintf(stderr, "WARNING: %s is not running; photos can't be displayed until it starts\n", DBUS_PHOTO_SERVICE);
    *all_deps_ready = false;
  }

  // Listen for drm manager signals
  s->drm_mgr_updown_slot = on_service_updown(s->bus, DBUS_DRM_MGR_SERVICE, on_drm_mgr_updown, s);
  if (!is_service_up(s->bus, DBUS_DRM_MGR_SERVICE)) {
    fprintf(stderr, "WARNING: %s DRM manager isn't up, no display until it starts\n", DBUS_DRM_MGR_SERVICE);
    *all_deps_ready = false;
  }

  return s;
}

void dbus_listeners_free(struct DBusListeners *s) {
  if (!s)
    return;
  if (s->vtable)
    sd_bus_slot_unref(s->vtable);
  if (s->presence_signal_slot)
    sd_bus_slot_unref(s->presence_signal_slot);
  if (s->presence_updown_slot)
    sd_bus_slot_unref(s->presence_updown_slot);
  if (s->photo_updown_slot)
    sd_bus_slot_unref(s->photo_updown_slot);
  if (s->drm_mgr_updown_slot)
    sd_bus_slot_unref(s->drm_mgr_updown_slot);
  if (s->bus) {
    sd_bus_flush_close_unref(s->bus);
  }
  free(s);
}

int dbus_listeners_run_once(struct DBusListeners *s, int timeout_ms) {
  for (;;) {
    int r = sd_bus_process(s->bus, NULL);
    if (r < 0)
      return r;
    if (r == 0)
      break;
  }
  int r = sd_bus_wait(s->bus, (uint64_t)timeout_ms * 1000ULL);
  if (r < 0 && -r != EINTR)
    return r;
  return 0;
}
