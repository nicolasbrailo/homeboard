#include "dbus_listeners.h"
#include "dbus_helpers.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <systemd/sd-bus.h>
#include <systemd/sd-event.h>

#define DBUS_AMBIENCE_SERVICE "io.homeboard.Ambience"
#define DBUS_AMBIENCE_PATH "/io/homeboard/Ambience"
#define DBUS_AMBIENCE_INTERFACE "io.homeboard.Ambience1"

#define DBUS_PRESENCE_SERVICE "io.homeboard.Presence"
#define DBUS_PRESENCE_PATH "/io/homeboard/Presence"
#define DBUS_PRESENCE_INTERFACE "io.homeboard.Presence1"
#define DBUS_PRESENCE_SIGNAL "PresenceChanged"

#define DBUS_PHOTO_SERVICE "io.homeboard.PhotoProvider"

struct DBusListeners {
  sd_bus *bus;
  sd_bus_slot *vtable_slot;
  sd_bus_slot *presence_signal_slot;
  sd_bus_slot *presence_updown_slot;
  sd_bus_slot *photo_updown_slot;
  struct dbus_listeners_cbs cbs;
  void *ud;
};

static int method_next(sd_bus_message *m, void *userdata, sd_bus_error *err) {
  (void)err;
  struct DBusListeners *d = userdata;
  d->cbs.on_next(d->ud);
  return sd_bus_reply_method_return(m, NULL);
}

static int method_prev(sd_bus_message *m, void *userdata, sd_bus_error *err) {
  (void)err;
  struct DBusListeners *d = userdata;
  d->cbs.on_prev(d->ud);
  return sd_bus_reply_method_return(m, NULL);
}

static int method_set_transition_time(sd_bus_message *m, void *userdata,
                                      sd_bus_error *err) {
  struct DBusListeners *d = userdata;
  uint32_t seconds = 0;
  int r = sd_bus_message_read(m, "u", &seconds);
  if (r < 0)
    return r;
  if (!d->cbs.on_set_transition_time(d->ud, seconds)) {
    sd_bus_error_setf(err, SD_BUS_ERROR_INVALID_ARGS,
                      "invalid transition time %u", seconds);
    return -EINVAL;
  }
  return sd_bus_reply_method_return(m, NULL);
}

static int method_announce(sd_bus_message *m, void *userdata,
                           sd_bus_error *err) {
  struct DBusListeners *d = userdata;
  uint32_t timeout_seconds = 0;
  const char *msg = NULL;
  int r = sd_bus_message_read(m, "us", &timeout_seconds, &msg);
  if (r < 0)
    return r;
  r = d->cbs.on_announce(d->ud, timeout_seconds, msg);
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
  struct DBusListeners *d = userdata;
  uint32_t rotation = 0;
  const char *interp = NULL;
  const char *h_align = NULL;
  const char *v_align = NULL;
  int r =
      sd_bus_message_read(m, "usss", &rotation, &interp, &h_align, &v_align);
  if (r < 0)
    return r;
  r = d->cbs.on_set_render_config(d->ud, rotation, interp, h_align, v_align);
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
    SD_BUS_SIGNAL("DisplayingPhoto", "s", 0),
    SD_BUS_SIGNAL("SlideshowActive", "b", 0),
    SD_BUS_VTABLE_END,
};

static int on_presence_changed(sd_bus_message *m, void *userdata,
                               sd_bus_error *err) {
  (void)err;
  struct DBusListeners *d = userdata;
  // sd_bus_message_read's "b" reads into int* (4 bytes), not bool* — writing
  // into a bool local would corrupt the stack.
  int present_raw = 0;
  const int r = sd_bus_message_read(m, "b", &present_raw);
  if (r < 0) {
    fprintf(stderr, "presence PresenceChanged read: %s\n", strerror(-r));
    return 0;
  }
  d->cbs.on_presence_changed(d->ud, present_raw != 0);
  return 0;
}

static void on_presence_updown(void *ud, bool up) {
  struct DBusListeners *d = ud;
  d->cbs.on_presence_service_updown(d->ud, up);
}

static void on_photo_updown(void *ud, bool up) {
  struct DBusListeners *d = ud;
  d->cbs.on_photo_service_updown(d->ud, up);
}

struct DBusListeners *dbus_listeners_init(sd_event *event,
                                          const struct dbus_listeners_cbs *cbs,
                                          void *ud) {
  if (!event || !cbs)
    return NULL;
  struct DBusListeners *d = calloc(1, sizeof(*d));
  if (!d)
    return NULL;
  d->cbs = *cbs;
  d->ud = ud;

  int r = sd_bus_open_system(&d->bus);
  if (r < 0) {
    fprintf(stderr, "dbus_listeners: sd_bus_open_system: %s\n", strerror(-r));
    dbus_listeners_free(d);
    return NULL;
  }
  r = sd_bus_attach_event(d->bus, event, SD_EVENT_PRIORITY_NORMAL);
  if (r < 0) {
    fprintf(stderr, "dbus_listeners: sd_bus_attach_event: %s\n", strerror(-r));
    dbus_listeners_free(d);
    return NULL;
  }

  r = sd_bus_add_object_vtable(d->bus, &d->vtable_slot, DBUS_AMBIENCE_PATH,
                               DBUS_AMBIENCE_INTERFACE, g_vtable, d);
  if (r < 0) {
    fprintf(stderr, "sd_bus_add_object_vtable: %s\n", strerror(-r));
    dbus_listeners_free(d);
    return NULL;
  }
  r = sd_bus_request_name(d->bus, DBUS_AMBIENCE_SERVICE, 0);
  if (r < 0) {
    fprintf(stderr, "sd_bus_request_name(%s): %s\n", DBUS_AMBIENCE_SERVICE,
            strerror(-r));
    dbus_listeners_free(d);
    return NULL;
  }
  printf("Ambience service offered at %s\n", DBUS_AMBIENCE_SERVICE);

  r = sd_bus_match_signal(d->bus, &d->presence_signal_slot,
                          DBUS_PRESENCE_SERVICE, DBUS_PRESENCE_PATH,
                          DBUS_PRESENCE_INTERFACE, DBUS_PRESENCE_SIGNAL,
                          on_presence_changed, d);
  if (r < 0) {
    fprintf(stderr, "sd_bus_match_signal(presence): %s\n", strerror(-r));
    dbus_listeners_free(d);
    return NULL;
  }

  d->presence_updown_slot =
      on_service_updown(d->bus, DBUS_PRESENCE_SERVICE, on_presence_updown, d);
  if (!is_service_up(d->bus, DBUS_PRESENCE_SERVICE)) {
    fprintf(stderr,
            "WARNING: %s is not running; no presence signals will arrive until "
            "it starts\n",
            DBUS_PRESENCE_SERVICE);
  }

  d->photo_updown_slot =
      on_service_updown(d->bus, DBUS_PHOTO_SERVICE, on_photo_updown, d);
  if (!is_service_up(d->bus, DBUS_PHOTO_SERVICE)) {
    fprintf(stderr,
            "WARNING: %s is not running; photos can't be displayed until it "
            "starts\n",
            DBUS_PHOTO_SERVICE);
  }

  printf("Listeners ready: Ambience methods + %s.%s + %s/%s up-down monitors\n",
         DBUS_PRESENCE_INTERFACE, DBUS_PRESENCE_SIGNAL, DBUS_PRESENCE_SERVICE,
         DBUS_PHOTO_SERVICE);
  return d;
}

void dbus_listeners_free(struct DBusListeners *d) {
  if (!d)
    return;
  if (d->vtable_slot)
    sd_bus_slot_unref(d->vtable_slot);
  if (d->presence_signal_slot)
    sd_bus_slot_unref(d->presence_signal_slot);
  if (d->presence_updown_slot)
    sd_bus_slot_unref(d->presence_updown_slot);
  if (d->photo_updown_slot)
    sd_bus_slot_unref(d->photo_updown_slot);
  if (d->bus)
    sd_bus_flush_close_unref(d->bus);
  free(d);
}

int dbus_listeners_emit_slideshow_active(struct DBusListeners *d, bool active) {
  if (!d || !d->bus)
    return -EINVAL;
  int r = sd_bus_emit_signal(d->bus, DBUS_AMBIENCE_PATH, DBUS_AMBIENCE_INTERFACE,
                             "SlideshowActive", "b", (int)(active ? 1 : 0));
  if (r < 0)
    fprintf(stderr, "emit SlideshowActive: %s\n", strerror(-r));
  return r;
}

int dbus_listeners_emit_displaying_photo(struct DBusListeners *d,
                                         const char *meta) {
  if (!d || !d->bus)
    return -EINVAL;
  int r = sd_bus_emit_signal(d->bus, DBUS_AMBIENCE_PATH, DBUS_AMBIENCE_INTERFACE,
                             "DisplayingPhoto", "s", meta ? meta : "");
  if (r < 0)
    fprintf(stderr, "emit DisplayingPhoto: %s\n", strerror(-r));
  return r;
}
