#define _GNU_SOURCE
#include "photo_client.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <systemd/sd-bus.h>
#include <systemd/sd-event.h>
#include <unistd.h>

#define DBUS_PHOTO_SERVICE "io.homeboard.PhotoProvider"
#define DBUS_PHOTO_PATH "/io/homeboard/PhotoProvider"
#define DBUS_PHOTO_INTERFACE "io.homeboard.PhotoProvider1"

struct PhotoClient {
  sd_bus *bus; // owned
  sd_bus_slot *active_slot;
  photo_client_fetch_cb pending_cb;
  void *pending_ud;
};

struct PhotoClient *photo_client_init(sd_event *event) {
  if (!event)
    return NULL;
  struct PhotoClient *pc = calloc(1, sizeof(*pc));
  if (!pc)
    return NULL;
  int r = sd_bus_open_system(&pc->bus);
  if (r < 0) {
    fprintf(stderr, "photo_client: sd_bus_open_system: %s\n", strerror(-r));
    free(pc);
    return NULL;
  }
  r = sd_bus_attach_event(pc->bus, event, SD_EVENT_PRIORITY_NORMAL);
  if (r < 0) {
    fprintf(stderr, "photo_client: sd_bus_attach_event: %s\n", strerror(-r));
    sd_bus_flush_close_unref(pc->bus);
    free(pc);
    return NULL;
  }
  return pc;
}

void photo_client_free(struct PhotoClient *pc) {
  if (!pc)
    return;
  photo_client_cancel_fetch(pc);
  if (pc->bus)
    sd_bus_flush_close_unref(pc->bus);
  free(pc);
}

void photo_client_cancel_fetch(struct PhotoClient *pc) {
  if (!pc->active_slot)
    return;
  sd_bus_slot_unref(pc->active_slot);
  pc->active_slot = NULL;
  pc->pending_cb = NULL;
  pc->pending_ud = NULL;
}

static int on_fetch_reply(sd_bus_message *m, void *userdata, sd_bus_error *err) {
  (void)err;
  struct PhotoClient *pc = userdata;

  // Snapshot and clear before invoking the user callback: the callback may
  // re-issue a fetch (overwriting pending_cb / active_slot) and we still
  // need to release this slot afterwards without stomping the new one.
  sd_bus_slot *slot = pc->active_slot;
  photo_client_fetch_cb cb = pc->pending_cb;
  void *ud = pc->pending_ud;
  pc->active_slot = NULL;
  pc->pending_cb = NULL;
  pc->pending_ud = NULL;

  if (sd_bus_message_is_method_error(m, NULL)) {
    const sd_bus_error *e = sd_bus_message_get_error(m);
    fprintf(stderr, "GetPhoto failed: %s\n", e && e->message ? e->message : "(no error)");
    if (cb)
      cb(-1, NULL, ud);
    sd_bus_slot_unref(slot);
    return 0;
  }

  int fd = -1;
  const char *meta = NULL;
  int r = sd_bus_message_read(m, "hs", &fd, &meta);
  if (r < 0) {
    fprintf(stderr, "bad GetPhoto reply: %s\n", strerror(-r));
    if (cb)
      cb(-1, NULL, ud);
    sd_bus_slot_unref(slot);
    return 0;
  }

  // photo-provider ships the fd over SCM_RIGHTS; sender and receiver share
  // one open file description (and thus the file offset). GetPrevPhoto
  // re-serves a memfd we already read to EOF last time it was on screen;
  // rewind before handing to libjpeg. Harmless for fresh memfds.
  if (lseek(fd, 0, SEEK_SET) < 0)
    perror("lseek");

  if (cb)
    cb(fd, meta, ud);

  sd_bus_slot_unref(slot);
  return 0;
}

int photo_client_fetch_async(struct PhotoClient *pc, bool prev,
                             photo_client_fetch_cb cb, void *ud) {
  photo_client_cancel_fetch(pc);

  pc->pending_cb = cb;
  pc->pending_ud = ud;

  const char *method = prev ? "GetPrevPhoto" : "GetPhoto";
  int r = sd_bus_call_method_async(pc->bus, &pc->active_slot, DBUS_PHOTO_SERVICE,
                                   DBUS_PHOTO_PATH, DBUS_PHOTO_INTERFACE, method,
                                   on_fetch_reply, pc, "");
  if (r < 0) {
    fprintf(stderr, "%s dispatch failed: %s\n", method, strerror(-r));
    pc->active_slot = NULL;
    pc->pending_cb = NULL;
    pc->pending_ud = NULL;
    return -1;
  }
  return 0;
}

int photo_client_set_target_size(struct PhotoClient *pc, uint32_t w,
                                 uint32_t h) {
  sd_bus_error err = SD_BUS_ERROR_NULL;
  int r = sd_bus_call_method(pc->bus, DBUS_PHOTO_SERVICE, DBUS_PHOTO_PATH,
                             DBUS_PHOTO_INTERFACE, "SetTargetSize", &err, NULL,
                             "uu", w, h);
  if (r < 0) {
    fprintf(stderr, "SetTargetSize failed: %s\n",
            err.message ? err.message : strerror(-r));
    sd_bus_error_free(&err);
    return -1;
  }
  sd_bus_error_free(&err);
  return 0;
}

int photo_client_set_embed_qr(struct PhotoClient *pc, bool embed_qr) {
  sd_bus_error err = SD_BUS_ERROR_NULL;
  int r = sd_bus_call_method(pc->bus, DBUS_PHOTO_SERVICE, DBUS_PHOTO_PATH,
                             DBUS_PHOTO_INTERFACE, "SetEmbedQr", &err, NULL, "b",
                             (int)(embed_qr ? 1 : 0));
  if (r < 0) {
    fprintf(stderr, "SetEmbedQr failed: %s\n",
            err.message ? err.message : strerror(-r));
    sd_bus_error_free(&err);
    return -1;
  }
  sd_bus_error_free(&err);
  return 0;
}

int photo_client_push_config(struct PhotoClient *pc, uint32_t w, uint32_t h,
                             bool embed_qr) {
  if (photo_client_set_target_size(pc, w, h) < 0)
    return -1;
  if (photo_client_set_embed_qr(pc, embed_qr) < 0)
    return -1;
  printf("photo-provider configured: %ux%u embed_qr=%d\n", w, h, embed_qr);
  return 0;
}
