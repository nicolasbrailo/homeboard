#include "photo_client.h"

#include <errno.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <systemd/sd-bus.h>
#include <unistd.h>

#include "jpeg_render/img_render.h"

#define DBUS_PHOTO_SERVICE "io.homeboard.PhotoProvider"
#define DBUS_PHOTO_PATH "/io/homeboard/PhotoProvider"
#define DBUS_PHOTO_INTERFACE "io.homeboard.PhotoProvider1"
#define DBUS_PHOTO_ERROR_PREFIX "io.homeboard.PhotoProvider.Error."

#define DBUS_AMBIENCE_SERVICE "io.homeboard.Ambience"
#define DBUS_AMBIENCE_PATH "/io/homeboard/Ambience"
#define DBUS_AMBIENCE_INTERFACE "io.homeboard.Ambience1"

// Explicit, instead of sd-bus' implicit 25s default. photo-provider bounds its
// own GetPhoto wait well below this, so hitting it means the provider or the
// bus is wedged, not that the network is slow.
#define PHOTO_CALL_TIMEOUT_USEC (10ULL * 1000 * 1000)

struct PhotoClient {
  // NULL while disconnected; the next call reopens it.
  sd_bus *bus;
  uint32_t requested_w;
  uint32_t requested_h;
  // What photo-provider said about the last call that failed with an error
  // reply of its own, or "" if the last call succeeded or failed some other
  // way. Only meaningful until the next call.
  char last_error[192];
};

static sd_bus *get_bus(struct PhotoClient *pc) {
  if (pc->bus)
    return pc->bus;
  sd_bus *bus = NULL;
  int r = sd_bus_open_system(&bus);
  if (r < 0) {
    fprintf(stderr, "photo_client: sd_bus_open_system: %s\n", strerror(-r));
    return NULL;
  }
  pc->bus = bus;
  return bus;
}

// Called after a failed call. An error reply sent by photo-provider itself
// means the connection is fine. Anything else (timeout, disconnect, a read
// buffer sd-bus can't parse) means the connection can't be trusted: a late
// reply to a timed-out call, or unparseable bytes, would stay queued and
// poison every later call. Drop it; the next call opens a fresh one.
static void drop_bus_unless_provider_error(struct PhotoClient *pc,
                                           const sd_bus_error *err) {
  if (err->name && strncmp(err->name, DBUS_PHOTO_ERROR_PREFIX,
                           strlen(DBUS_PHOTO_ERROR_PREFIX)) == 0)
    return;
  fprintf(stderr, "photo_client: dropping bus connection, will reconnect on "
                  "next call\n");
  sd_bus_close_unref(pc->bus);
  pc->bus = NULL;
}

// Calls `method` on photo-provider, with arguments described by `types`.
// Returns >= 0 on success; if reply is non-NULL, it holds the reply (caller
// unrefs).
static int call_method(struct PhotoClient *pc, const char *method,
                       sd_bus_message **reply, const char *types, ...) {
  sd_bus *bus = get_bus(pc);
  if (!bus) {
    fprintf(stderr, "%s failed: no bus connection\n", method);
    return -ENOTCONN;
  }

  sd_bus_message *call = NULL;
  int r = sd_bus_message_new_method_call(bus, &call, DBUS_PHOTO_SERVICE,
                                         DBUS_PHOTO_PATH, DBUS_PHOTO_INTERFACE,
                                         method);
  if (r >= 0) {
    va_list ap;
    va_start(ap, types);
    r = sd_bus_message_appendv(call, types, ap);
    va_end(ap);
  }
  if (r < 0) {
    fprintf(stderr, "%s: can't build call: %s\n", method, strerror(-r));
    sd_bus_message_unref(call);
    return r;
  }

  sd_bus_error err = SD_BUS_ERROR_NULL;
  r = sd_bus_call(bus, call, PHOTO_CALL_TIMEOUT_USEC, &err, reply);
  sd_bus_message_unref(call);
  pc->last_error[0] = '\0';
  if (r < 0) {
    fprintf(stderr, "%s failed: %s\n", method,
            err.message ? err.message : strerror(-r));
    // Only photo-provider's own message is worth showing a user: a timeout or
    // a dropped bus says nothing about the photos.
    if (err.name && err.message &&
        strncmp(err.name, DBUS_PHOTO_ERROR_PREFIX,
                strlen(DBUS_PHOTO_ERROR_PREFIX)) == 0)
      snprintf(pc->last_error, sizeof(pc->last_error), "%s", err.message);
    drop_bus_unless_provider_error(pc, &err);
  }
  sd_bus_error_free(&err);
  return r;
}

struct PhotoClient *photo_client_init() {
  struct PhotoClient *pc = calloc(1, sizeof(*pc));
  if (!pc)
    return NULL;
  if (!get_bus(pc)) {
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

int photo_client_fetch_one(struct PhotoClient *pc, const char *method,
                           int *fd_out, char **meta_out,
                           const struct img_render_cfg *render_cfg,
                           char *err_out, size_t err_sz) {
  printf("Fetching new photo with %s.%s\n", DBUS_PHOTO_SERVICE, method);
  if (err_out && err_sz > 0)
    err_out[0] = '\0';
  sd_bus_message *reply = NULL;
  if (call_method(pc, method, &reply, "") < 0) {
    if (err_out && err_sz > 0)
      snprintf(err_out, err_sz, "%s", pc->last_error);
    return -1;
  }

  int fd = -1;
  const char *meta = NULL;
  int r = sd_bus_message_read(reply, "hs", &fd, &meta);
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

  // Re-broadcast the metadata to signal we're about to render it (this may fit
  // better in render.c, but since this object already has a bus, render is
  // already too heavy and the only reasonable thing to do with a fetched
  // picture is display it, it's the photo client that emits this)
  static const struct img_render_cfg default_cfg = {
      ROT_0, INTERP_BILINEAR, HORIZONTAL_ALIGN_CENTER, VERTICAL_ALIGN_CENTER};
  const struct img_render_cfg *cfg = render_cfg ? render_cfg : &default_cfg;
  r = sd_bus_emit_signal(pc->bus, DBUS_AMBIENCE_PATH, DBUS_AMBIENCE_INTERFACE,
                         "DisplayingPhoto", "susssuu",
                         *meta_out ? *meta_out : "", (uint32_t)cfg->rot,
                         img_render_cfg_interpolation_name(cfg->interp),
                         img_render_cfg_horizontal_align_name(cfg->h_align),
                         img_render_cfg_vertical_align_name(cfg->v_align),
                         pc->requested_w, pc->requested_h);
  if (r < 0)
    fprintf(stderr, "Emit DisplayingPhoto: %s\n", strerror(-r));
  // The photo was delivered even if the signal wasn't; callers own *fd_out.
  return 0;
}

// Push initial config to photo-provider: target size matched to the physical
// screen (axes swapped for 90/270 rotation so the server renders at the
// correct aspect ratio) and embed_qr.
int push_initial_config(struct PhotoClient *pc, uint32_t w, uint32_t h,
                        bool embed_qr) {
  pc->requested_w = w;
  pc->requested_h = h;

  if (call_method(pc, "SetTargetSize", NULL, "uu", w, h) < 0)
    return -1;
  if (call_method(pc, "SetEmbedQr", NULL, "b", (int)(embed_qr ? 1 : 0)) < 0)
    return -1;
  printf("photo-provider configured: %ux%u embed_qr=%d\n", w, h, embed_qr);
  return 0;
}
