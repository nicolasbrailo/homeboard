#include "dbus_client.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define AMBIENCE_SERVICE "io.homeboard.Ambience"
#define AMBIENCE_PATH "/io/homeboard/Ambience"
#define AMBIENCE_INTERFACE "io.homeboard.Ambience1"

#define PHOTO_SERVICE "io.homeboard.PhotoProvider"
#define PHOTO_PATH "/io/homeboard/PhotoProvider"
#define PHOTO_INTERFACE "io.homeboard.PhotoProvider1"

#define OCCUPANCY_SERVICE "io.homeboard.Occupancy"
#define OCCUPANCY_PATH "/io/homeboard/Occupancy"
#define OCCUPANCY_INTERFACE "io.homeboard.Occupancy1"

#define PRESENCE_SERVICE "io.homeboard.Presence"
#define PRESENCE_PATH "/io/homeboard/Presence"
#define PRESENCE_INTERFACE "io.homeboard.Presence1"

struct rc_dbus {
  sd_bus *bus;
  sd_bus_slot *occupancy_slot;
  sd_bus_slot *displayed_photo_slot;
  sd_bus_slot *slideshow_active_slot;
  rc_dbus_occupancy_cb on_occupancy;
  rc_dbus_displayed_photo_cb on_displayed_photo;
  rc_dbus_slideshow_active_cb on_slideshow_active;
  void *ud;
};

static int on_occupancy_report(sd_bus_message *m, void *userdata,
                               sd_bus_error *err) {
  (void)err;
  struct rc_dbus *d = userdata;
  int occupied = 0;
  uint32_t distance = 0;
  int r = sd_bus_message_read(m, "bu", &occupied, &distance);
  if (r < 0) {
    fprintf(stderr, "Report: parse failed: %s\n", strerror(-r));
    return 0;
  }
  d->on_occupancy(occupied != 0, distance, d->ud);
  return 0;
}

static int on_displaying_photo(sd_bus_message *m, void *userdata,
                               sd_bus_error *err) {
  (void)err;
  struct rc_dbus *d = userdata;
  const char *meta = NULL;
  int r = sd_bus_message_read(m, "s", &meta);
  if (r < 0) {
    fprintf(stderr, "DisplayingPhoto: parse failed: %s\n", strerror(-r));
    return 0;
  }
  d->on_displayed_photo(meta ? meta : "", d->ud);
  return 0;
}

static int on_slideshow_active_signal(sd_bus_message *m, void *userdata,
                                      sd_bus_error *err) {
  (void)err;
  struct rc_dbus *d = userdata;
  int active = 0;
  int r = sd_bus_message_read(m, "b", &active);
  if (r < 0) {
    fprintf(stderr, "SlideshowActive: parse failed: %s\n", strerror(-r));
    return 0;
  }
  d->on_slideshow_active(active != 0, d->ud);
  return 0;
}

struct rc_dbus *rc_dbus_init(rc_dbus_occupancy_cb on_occupancy,
                             rc_dbus_displayed_photo_cb on_displayed_photo,
                             rc_dbus_slideshow_active_cb on_slideshow_active,
                             void *ud) {
  struct rc_dbus *d = calloc(1, sizeof(*d));
  if (!d)
    return NULL;
  d->on_occupancy = on_occupancy;
  d->on_displayed_photo = on_displayed_photo;
  d->on_slideshow_active = on_slideshow_active;
  d->ud = ud;

  int r = sd_bus_open_system(&d->bus);
  if (r < 0) {
    fprintf(stderr, "sd_bus_open_system: %s\n", strerror(-r));
    free(d);
    return NULL;
  }

  r = sd_bus_match_signal(d->bus, &d->occupancy_slot, OCCUPANCY_SERVICE,
                          OCCUPANCY_PATH, OCCUPANCY_INTERFACE, "Report",
                          on_occupancy_report, d);
  if (r < 0) {
    fprintf(stderr, "sd_bus_match_signal(Report): %s\n", strerror(-r));
    rc_dbus_free(d);
    return NULL;
  }

  // NULL sender: DisplayingPhoto is emitted from the ambience slideshow's
  // worker-thread private bus, whose unique name does NOT own
  // io.homeboard.Ambience (the main dispatch bus does). dbus-daemon resolves
  // a well-known sender in a match rule to the current owner's unique name,
  // so passing AMBIENCE_SERVICE here would filter out the worker's signals.
  // We rely on path+interface+member to identify the signal instead.
  r = sd_bus_match_signal(d->bus, &d->displayed_photo_slot, NULL, AMBIENCE_PATH,
                          AMBIENCE_INTERFACE, "DisplayingPhoto",
                          on_displaying_photo, d);
  if (r < 0) {
    fprintf(stderr, "sd_bus_match_signal(DisplayingPhoto): %s\n", strerror(-r));
    rc_dbus_free(d);
    return NULL;
  }

  // NULL sender for uniformity with DisplayingPhoto. SlideshowActive is
  // emitted from Ambience's main (name-owning) bus, so a well-known sender
  // filter would work here too — but keeping both subscriptions on the same
  // rule shape avoids a subtle footgun if the emit side ever moves.
  r = sd_bus_match_signal(d->bus, &d->slideshow_active_slot, NULL,
                          AMBIENCE_PATH, AMBIENCE_INTERFACE, "SlideshowActive",
                          on_slideshow_active_signal, d);
  if (r < 0) {
    fprintf(stderr, "sd_bus_match_signal(SlideshowActive): %s\n", strerror(-r));
    rc_dbus_free(d);
    return NULL;
  }

  printf("D-Bus client ready; listening for %s Report and %s DisplayingPhoto, "
         "SlideshowActive\n",
         OCCUPANCY_SERVICE, AMBIENCE_SERVICE);
  return d;
}

void rc_dbus_free(struct rc_dbus *d) {
  if (!d)
    return;
  if (d->occupancy_slot)
    sd_bus_slot_unref(d->occupancy_slot);
  if (d->displayed_photo_slot)
    sd_bus_slot_unref(d->displayed_photo_slot);
  if (d->slideshow_active_slot)
    sd_bus_slot_unref(d->slideshow_active_slot);
  if (d->bus)
    sd_bus_flush_close_unref(d->bus);
  free(d);
}

sd_bus *rc_dbus_bus(struct rc_dbus *d) { return d->bus; }

static int log_err(const char *method, int r, sd_bus_error *err) {
  fprintf(stderr, "%s failed: %s\n", method,
          err->message ? err->message : strerror(-r));
  return -1;
}

int rc_dbus_ambience_call_void(struct rc_dbus *d, const char *method) {
  sd_bus_error err = SD_BUS_ERROR_NULL;
  int r = sd_bus_call_method(d->bus, AMBIENCE_SERVICE, AMBIENCE_PATH,
                             AMBIENCE_INTERFACE, method, &err, NULL, "");
  int ret = (r < 0) ? log_err(method, r, &err) : 0;
  sd_bus_error_free(&err);
  return ret;
}

int rc_dbus_presence_call_void(struct rc_dbus *d, const char *method) {
  sd_bus_error err = SD_BUS_ERROR_NULL;
  int r = sd_bus_call_method(d->bus, PRESENCE_SERVICE, PRESENCE_PATH,
                             PRESENCE_INTERFACE, method, &err, NULL, "");
  int ret = (r < 0) ? log_err(method, r, &err) : 0;
  sd_bus_error_free(&err);
  return ret;
}

int rc_dbus_ambience_set_transition_time(struct rc_dbus *d, uint32_t secs) {
  sd_bus_error err = SD_BUS_ERROR_NULL;
  int r = sd_bus_call_method(d->bus, AMBIENCE_SERVICE, AMBIENCE_PATH,
                             AMBIENCE_INTERFACE, "SetTransitionTimeSecs", &err,
                             NULL, "u", secs);
  int ret = (r < 0) ? log_err("SetTransitionTimeSecs", r, &err) : 0;
  sd_bus_error_free(&err);
  return ret;
}

int rc_dbus_photo_set_embed_qr(struct rc_dbus *d, bool on) {
  sd_bus_error err = SD_BUS_ERROR_NULL;
  int r = sd_bus_call_method(d->bus, PHOTO_SERVICE, PHOTO_PATH, PHOTO_INTERFACE,
                             "SetEmbedQr", &err, NULL, "b", (int)on);
  int ret = (r < 0) ? log_err("SetEmbedQr", r, &err) : 0;
  sd_bus_error_free(&err);
  return ret;
}

int rc_dbus_photo_set_target_size(struct rc_dbus *d, uint32_t w, uint32_t h) {
  sd_bus_error err = SD_BUS_ERROR_NULL;
  int r = sd_bus_call_method(d->bus, PHOTO_SERVICE, PHOTO_PATH, PHOTO_INTERFACE,
                             "SetTargetSize", &err, NULL, "uu", w, h);
  int ret = (r < 0) ? log_err("SetTargetSize", r, &err) : 0;
  sd_bus_error_free(&err);
  return ret;
}
