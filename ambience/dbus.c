#include "dbus.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <systemd/sd-bus.h>

#define DBUS_SERVICE "io.homeboard.Ambience"
#define DBUS_PATH "/io/homeboard/Ambience"
#define DBUS_INTERFACE "io.homeboard.Ambience1"

struct AmbienceDbus {
  sd_bus *bus;
  sd_bus_slot *vtable_slot;
  ambience_next_cb on_next;
  ambience_prev_cb on_prev;
  ambience_force_cb on_force_on;
  ambience_force_cb on_force_off;
  ambience_set_transition_time_cb on_set_transition_time;
  ambience_announce_requested_cb on_announce_requested_cb;
  ambience_set_render_config_cb on_set_render_config;
  void *ud;
};

static int method_next(sd_bus_message *m, void *userdata, sd_bus_error *err) {
  (void)err;
  struct AmbienceDbus *d = userdata;
  d->on_next(d->ud);
  return sd_bus_reply_method_return(m, NULL);
}

static int method_prev(sd_bus_message *m, void *userdata, sd_bus_error *err) {
  (void)err;
  struct AmbienceDbus *d = userdata;
  d->on_prev(d->ud);
  return sd_bus_reply_method_return(m, NULL);
}

static int method_force_on(sd_bus_message *m, void *userdata, sd_bus_error *err) {
  (void)err;
  struct AmbienceDbus *d = userdata;
  d->on_force_on(d->ud);
  return sd_bus_reply_method_return(m, NULL);
}

static int method_force_off(sd_bus_message *m, void *userdata, sd_bus_error *err) {
  (void)err;
  struct AmbienceDbus *d = userdata;
  d->on_force_off(d->ud);
  return sd_bus_reply_method_return(m, NULL);
}

static int method_set_transition_time(sd_bus_message *m, void *userdata, sd_bus_error *err) {
  struct AmbienceDbus *d = userdata;
  uint32_t seconds = 0;
  int r = sd_bus_message_read(m, "u", &seconds);
  if (r < 0)
    return r;
  if (!d->on_set_transition_time(d->ud, seconds)) {
    sd_bus_error_setf(err, SD_BUS_ERROR_INVALID_ARGS, "invalid transition time %u", seconds);
    return -EINVAL;
  }
  return sd_bus_reply_method_return(m, NULL);
}

static int method_announce_requested(sd_bus_message *m, void *userdata, sd_bus_error *err) {
  struct AmbienceDbus *d = userdata;
  uint32_t timeout_seconds = 0;
  const char* msg = NULL;
  int r = sd_bus_message_read(m, "us", &timeout_seconds, &msg);
  if (r < 0)
    return r;
  r = d->on_announce_requested_cb(d->ud, timeout_seconds, msg);
  if (r < 0) {
    const char *errmsg;
    if (r == -EINVAL) {
      errmsg = "invalid timeout";
    } else if (r == -EBUSY) {
      errmsg = "announcement already active";
    } else {
      errmsg = "unknown error";
    }
    sd_bus_error_setf(err, SD_BUS_ERROR_INVALID_ARGS, "%s", errmsg);
    return r;
  }
  return sd_bus_reply_method_return(m, NULL);
}

static int method_set_render_config(sd_bus_message *m, void *userdata, sd_bus_error *err) {
  struct AmbienceDbus *d = userdata;
  uint32_t rotation = 0;
  const char *interp = NULL;
  const char *h_align = NULL;
  const char *v_align = NULL;
  int r = sd_bus_message_read(m, "usss", &rotation, &interp, &h_align, &v_align);
  if (r < 0)
    return r;
  r = d->on_set_render_config(d->ud, rotation, interp, h_align, v_align);
  if (r < 0) {
    sd_bus_error_setf(err, SD_BUS_ERROR_INVALID_ARGS,
                      "invalid render config (rotation=%u interpolation=%s h_align=%s v_align=%s)", rotation,
                      interp ? interp : "", h_align ? h_align : "", v_align ? v_align : "");
    return r;
  }
  return sd_bus_reply_method_return(m, NULL);
}

static const sd_bus_vtable g_vtable[] = {
    SD_BUS_VTABLE_START(0),
    SD_BUS_METHOD("Next", "", "", method_next, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("Prev", "", "", method_prev, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("ForceSlideshowOn", "", "", method_force_on, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("ForceSlideshowOff", "", "", method_force_off, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("SetTransitionTimeSecs", "u", "", method_set_transition_time, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("Announce", "us", "", method_announce_requested, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("SetRenderConfig", "usss", "", method_set_render_config, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_SIGNAL("DisplayingPhoto", "s", 0),
    SD_BUS_SIGNAL("SlideshowActive", "b", 0),
    SD_BUS_VTABLE_END,
};

int ambience_dbus_emit_displaying_photo(sd_bus *bus, const char *meta) {
  if (!bus)
    return -EINVAL;
  int r = sd_bus_emit_signal(bus, DBUS_PATH, DBUS_INTERFACE, "DisplayingPhoto", "s", meta ? meta : "");
  if (r < 0)
    fprintf(stderr, "emit DisplayingPhoto: %s\n", strerror(-r));
  return r;
}

int ambience_dbus_emit_slideshow_active(sd_bus *bus, bool active) {
  if (!bus)
    return -EINVAL;
  int r = sd_bus_emit_signal(bus, DBUS_PATH, DBUS_INTERFACE, "SlideshowActive", "b", (int)(active ? 1 : 0));
  if (r < 0)
    fprintf(stderr, "emit SlideshowActive: %s\n", strerror(-r));
  return r;
}

struct AmbienceDbus *ambience_dbus_init(sd_bus *bus, ambience_next_cb on_next, ambience_prev_cb on_prev,
                                        ambience_force_cb on_force_on, ambience_force_cb on_force_off,
                                        ambience_set_transition_time_cb on_set_transition_time,
                                        ambience_announce_requested_cb on_announce_requested_cb,
                                        ambience_set_render_config_cb on_set_render_config, void *ud) {
  if (!bus)
    return NULL;
  struct AmbienceDbus *d = calloc(1, sizeof(*d));
  if (!d)
    return NULL;
  d->bus = bus;
  d->on_next = on_next;
  d->on_prev = on_prev;
  d->on_force_on = on_force_on;
  d->on_force_off = on_force_off;
  d->on_set_transition_time = on_set_transition_time;
  d->on_announce_requested_cb = on_announce_requested_cb;
  d->on_set_render_config = on_set_render_config;
  d->ud = ud;

  int r = sd_bus_add_object_vtable(d->bus, &d->vtable_slot, DBUS_PATH, DBUS_INTERFACE, g_vtable, d);
  if (r < 0) {
    fprintf(stderr, "sd_bus_add_object_vtable: %s\n", strerror(-r));
    ambience_dbus_free(d);
    return NULL;
  }
  r = sd_bus_request_name(d->bus, DBUS_SERVICE, 0);
  if (r < 0) {
    fprintf(stderr, "sd_bus_request_name(%s): %s\n", DBUS_SERVICE, strerror(-r));
    ambience_dbus_free(d);
    return NULL;
  }
  printf("Ambience service offered at %s\n", DBUS_SERVICE);
  return d;
}

void ambience_dbus_free(struct AmbienceDbus *d) {
  if (!d)
    return;
  if (d->vtable_slot)
    sd_bus_slot_unref(d->vtable_slot);
  free(d);
}

int ambience_dbus_run_once(struct AmbienceDbus *d) {
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
