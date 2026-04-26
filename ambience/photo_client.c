#include "photo_client.h"

#include <systemd/sd-bus.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <stdlib.h>

#define DBUS_PHOTO_SERVICE "io.homeboard.PhotoProvider"
#define DBUS_PHOTO_PATH "/io/homeboard/PhotoProvider"
#define DBUS_PHOTO_INTERFACE "io.homeboard.PhotoProvider1"

#define DBUS_AMBIENCE_SERVICE "io.homeboard.Ambience"
#define DBUS_AMBIENCE_PATH "/io/homeboard/Ambience"
#define DBUS_AMBIENCE_INTERFACE "io.homeboard.Ambience1"

struct PhotoClient {
  sd_bus *bus;
};

struct PhotoClient* photo_client_init() {
  struct PhotoClient *pc = calloc(1, sizeof(*pc));
  if (!pc)
    return NULL;
  int r = sd_bus_open_system(&pc->bus);
  if (r < 0) {
    fprintf(stderr, "photo_client: sd_bus_open_system: %s\n", strerror(-r));
    free(pc);
    return NULL;
  }
  return pc;
}

void photo_client_free(struct PhotoClient *pc) {
  if (!pc)
    return;
  if (pc->bus)
    sd_bus_flush_close_unref(pc->bus);
  free(pc);
}

int photo_client_fetch_one(struct PhotoClient *pc, const char *method, int *fd_out, char **meta_out) {
  printf("Fetching new photo with %s.%s\n", DBUS_PHOTO_SERVICE, method);
  sd_bus_error err = SD_BUS_ERROR_NULL;
  sd_bus_message *reply = NULL;
  int r = sd_bus_call_method(pc->bus, DBUS_PHOTO_SERVICE, DBUS_PHOTO_PATH, DBUS_PHOTO_INTERFACE, method, &err, &reply, "");
  if (r == -ENOTCONN) {
    sd_bus_error_free(&err);
    err = SD_BUS_ERROR_NULL;
    fprintf(stderr, "%s: worker bus disconnected\n", method);
    // TODO Crash here? Bubble upstream and restart?
    abort();
  }
  if (r < 0) {
    fprintf(stderr, "%s failed: %s\n", method, err.message ? err.message : strerror(-r));
    sd_bus_error_free(&err);
    return -1;
  }

  int fd = -1;
  const char *meta = NULL;
  r = sd_bus_message_read(reply, "hs", &fd, &meta);
  if (r < 0) {
    fprintf(stderr, "bad %s reply: %s\n", method, strerror(-r));
    sd_bus_message_unref(reply);
    return -1;
  }

  int dup_fd = dup(fd);
  if (dup_fd < 0) {
    perror("dup");
    sd_bus_message_unref(reply);
    return -1;
  }

  // photo-provider ships the fd over SCM_RIGHTS, so sender and receiver share
  // one open file description (and thus the file offset). GetPrevPhoto re-
  // serves a memfd we already read to EOF last time it was on screen; rewind
  // before handing to libjpeg. Harmless for forward fetches (fresh memfds
  // are at 0 already).
  if (lseek(dup_fd, 0, SEEK_SET) < 0) {
    perror("lseek");
    close(dup_fd);
    sd_bus_message_unref(reply);
    return -1;
  }

  *fd_out = dup_fd;
  *meta_out = strdup(meta ? meta : "");
  sd_bus_message_unref(reply);

  // Re-broadcast the metadata to signal we're about to render it (this may fit better in render.c, but
  // since this object already has a bus, render is already too heavy and the only reasonable thing to
  // do with a fetched picture is display it, it's the photo client that emits this)
  r = sd_bus_emit_signal(pc->bus, DBUS_AMBIENCE_PATH, DBUS_AMBIENCE_INTERFACE, "DisplayingPhoto", "s", *meta_out ? *meta_out : "");
  if (r < 0)
    fprintf(stderr, "Emit DisplayingPhoto: %s\n", strerror(-r));
  return r;

  return 0;
}

// Push initial config to photo-provider: target size matched to the physical
// screen (axes swapped for 90/270 rotation so the server renders at the
// correct aspect ratio) and embed_qr.
int push_initial_config(struct PhotoClient *pc, uint32_t w, uint32_t h, bool embed_qr) {
  sd_bus_error err = SD_BUS_ERROR_NULL;
  int r = sd_bus_call_method(pc->bus, DBUS_PHOTO_SERVICE, DBUS_PHOTO_PATH, DBUS_PHOTO_INTERFACE, "SetTargetSize", &err, NULL, "uu", w, h);
  if (r < 0) {
    fprintf(stderr, "SetTargetSize failed: %s\n", err.message ? err.message : strerror(-r));
    sd_bus_error_free(&err);
    return -1;
  }
  sd_bus_error_free(&err);

  r = sd_bus_call_method(pc->bus, DBUS_PHOTO_SERVICE, DBUS_PHOTO_PATH, DBUS_PHOTO_INTERFACE, "SetEmbedQr", &err, NULL, "b", (int)(embed_qr ? 1 : 0));
  if (r < 0) {
    fprintf(stderr, "SetEmbedQr failed: %s\n", err.message ? err.message : strerror(-r));
    sd_bus_error_free(&err);
    return -1;
  }
  sd_bus_error_free(&err);
  printf("photo-provider configured: %ux%u embed_qr=%d\n", w, h, embed_qr);
  return 0;
}
