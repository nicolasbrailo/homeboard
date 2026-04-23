#include "dbus.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <systemd/sd-bus.h>
#include <unistd.h>

#define DBUS_SERVICE "io.homeboard.PhotoProvider"
#define DBUS_PATH "/io/homeboard/PhotoProvider"
#define DBUS_INTERFACE "io.homeboard.PhotoProvider1"

#define GET_PHOTO_TIMEOUT_MS 30000

static sd_bus *g_bus;
static sd_bus_slot *g_vtable_slot;
static struct pp_cache *g_cache;
static struct pp_www_session *g_ws;

// The memfd is handed to the client via SCM_RIGHTS, which shares the open
// file description (and thus the file offset) between sender and receiver.
// A client that read the fd to EOF on a previous delivery leaves it there;
// GetPrevPhoto re-serves that same memfd, and forward cases can in principle
// hit the same issue if we ever reuse memfds. Rewind to 0 on the way out so
// the API contract is "returned fd is ready to read from the top."
static int reply_with_fd(sd_bus_message *m, sd_bus_error *err, int fd, const char *meta) {
  if (lseek(fd, 0, SEEK_SET) < 0) {
    int e = errno;
    fprintf(stderr, "lseek(memfd): %s\n", strerror(e));
    return sd_bus_error_set_errno(err, e);
  }
  return sd_bus_reply_method_return(m, "hs", fd, meta ? meta : "");
}

static int method_get_photo(sd_bus_message *m, void *ud, sd_bus_error *err) {
  (void)ud;
  int fd = -1;
  char *meta = NULL;
  if (pp_cache_pop(g_cache, &fd, &meta, GET_PHOTO_TIMEOUT_MS) < 0)
    return sd_bus_error_set(err, "io.homeboard.PhotoProvider.Error.Unavailable", "no photo available");

  int r = reply_with_fd(m, err, fd, meta);
  close(fd); // dbus dup'd it; we drop ours
  free(meta);
  return r;
}

static int method_get_prev_photo(sd_bus_message *m, void *ud, sd_bus_error *err) {
  (void)ud;
  int fd = -1;
  char *meta = NULL;
  if (pp_cache_pop_prev(g_cache, &fd, &meta) < 0)
    return sd_bus_error_set(err, "io.homeboard.PhotoProvider.Error.Unavailable", "no previous photo available");

  int r = reply_with_fd(m, err, fd, meta);
  close(fd);
  free(meta);
  return r;
}

static int method_set_target_size(sd_bus_message *m, void *ud, sd_bus_error *err) {
  (void)ud;
  uint32_t w, h;
  int r = sd_bus_message_read(m, "uu", &w, &h);
  if (r < 0)
    return sd_bus_error_set_errno(err, -r);
  if (pp_www_session_set_target_size(g_ws, w, h) < 0)
    return sd_bus_error_set(err, "io.homeboard.PhotoProvider.Error.ReregisterFailed", "re-register failed");
  printf("PhotoProvider requested target size %dx%d\n", w, h);
  return sd_bus_reply_method_return(m, NULL);
}

static int method_set_embed_qr(sd_bus_message *m, void *ud, sd_bus_error *err) {
  (void)ud;
  int v;
  int r = sd_bus_message_read(m, "b", &v);
  if (r < 0)
    return sd_bus_error_set_errno(err, -r);
  if (pp_www_session_set_embed_qr(g_ws, v != 0) < 0)
    return sd_bus_error_set(err, "io.homeboard.PhotoProvider.Error.ReregisterFailed", "re-register failed");
  printf("PhotoProvider requested QR=%s\n", v ? "True" : "False");
  return sd_bus_reply_method_return(m, NULL);
}

static const sd_bus_vtable g_vtable[] = {
    SD_BUS_VTABLE_START(0),
    SD_BUS_METHOD("GetPhoto", "", "hs", method_get_photo, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("GetPrevPhoto", "", "hs", method_get_prev_photo, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("SetTargetSize", "uu", "", method_set_target_size, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("SetEmbedQr", "b", "", method_set_embed_qr, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_VTABLE_END,
};

int pp_dbus_init(struct pp_www_session *ws, struct pp_cache *cache) {
  g_ws = ws;
  g_cache = cache;

  int r = sd_bus_open_system(&g_bus);
  if (r < 0) {
    fprintf(stderr, "sd_bus_open_system: %s\n", strerror(-r));
    return -1;
  }
  r = sd_bus_add_object_vtable(g_bus, &g_vtable_slot, DBUS_PATH, DBUS_INTERFACE, g_vtable, NULL);
  if (r < 0) {
    fprintf(stderr, "sd_bus_add_object_vtable: %s\n", strerror(-r));
    return -1;
  }
  r = sd_bus_request_name(g_bus, DBUS_SERVICE, 0);
  if (r < 0) {
    fprintf(stderr, "sd_bus_request_name(%s): %s\n", DBUS_SERVICE, strerror(-r));
    return -1;
  }
  return 0;
}

void pp_dbus_free(void) {
  sd_bus_slot_unref(g_vtable_slot);
  g_vtable_slot = NULL;
  if (g_bus) {
    sd_bus_flush_close_unref(g_bus);
    g_bus = NULL;
  }
}

int pp_dbus_run_once(int timeout_ms) {
  int r = sd_bus_process(g_bus, NULL);
  if (r < 0)
    return r;
  if (r > 0)
    return 0;
  r = sd_bus_wait(g_bus, (uint64_t)timeout_ms * 1000ULL);
  if (r < 0 && -r != EINTR)
    return r;
  return 0;
}
