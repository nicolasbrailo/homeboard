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

// Dispatch is single-threaded, so this also bounds how long every other
// caller waits behind a GetPhoto. Must stay well below client call timeouts
// (sd-bus defaults to 25s, ambience uses 10s) so an empty cache is answered
// with Unavailable instead of the client timing out.
#define GET_PHOTO_TIMEOUT_MS 5000

static sd_bus *g_bus;
static sd_bus_slot *g_vtable_slot;
static struct pp_cache *g_cache;
static struct pp_backend *g_backend;
static struct pp_config *g_cfg;
static const char *g_cfg_path;

// The memfd is handed to the client via SCM_RIGHTS, which shares the open
// file description (and thus the file offset) between sender and receiver.
// A client that read the fd to EOF on a previous delivery leaves it there;
// GetPrevPhoto re-serves that same memfd, and forward cases can in principle
// hit the same issue if we ever reuse memfds. Rewind to 0 on the way out so
// the API contract is "returned fd is ready to read from the top."
static int reply_with_fd(sd_bus_message *m, sd_bus_error *err, int fd,
                         const char *meta) {
  if (lseek(fd, 0, SEEK_SET) < 0) {
    int e = errno;
    fprintf(stderr, "lseek(memfd): %s\n", strerror(e));
    return sd_bus_error_set_errno(err, e);
  }
  return sd_bus_reply_method_return(m, "hs", fd, meta ? meta : "");
}

// Turns an empty cache into an error reply. A generic "no photo available"
// leaves a user who mistyped a filter pattern looking at a blank screen with
// no reason for it, so when the backend can name the cause, say it under its
// own error name: consumers show the message.
static int no_photo_error(sd_bus_error *err) {
  const char *reason = pp_backend_unavailable_reason(g_backend);
  if (reason)
    return sd_bus_error_set(
        err, "io.homeboard.PhotoProvider.Error.NoAlbumMatchesFilter", reason);
  return sd_bus_error_set(err, "io.homeboard.PhotoProvider.Error.Unavailable",
                          "no photo available");
}

static int method_get_photo(sd_bus_message *m, void *ud, sd_bus_error *err) {
  (void)ud;
  int fd = -1;
  char *meta = NULL;
  if (pp_cache_pop(g_cache, &fd, &meta, GET_PHOTO_TIMEOUT_MS) < 0)
    return no_photo_error(err);

  int r = reply_with_fd(m, err, fd, meta);
  close(fd); // dbus dup'd it; we drop ours
  free(meta);
  return r;
}

static int method_get_prev_photo(sd_bus_message *m, void *ud,
                                 sd_bus_error *err) {
  (void)ud;
  int fd = -1;
  char *meta = NULL;
  if (pp_cache_pop_prev(g_cache, &fd, &meta) < 0)
    return sd_bus_error_set(err, "io.homeboard.PhotoProvider.Error.Unavailable",
                            "no previous photo available");

  int r = reply_with_fd(m, err, fd, meta);
  close(fd);
  free(meta);
  return r;
}

static int method_set_target_size(sd_bus_message *m, void *ud,
                                  sd_bus_error *err) {
  (void)ud;
  uint32_t w, h;
  int r = sd_bus_message_read(m, "uu", &w, &h);
  if (r < 0)
    return sd_bus_error_set_errno(err, -r);
  r = pp_backend_set_target_size(g_backend, w, h);
  if (r < 0)
    return sd_bus_error_setf(err,
                             "io.homeboard.PhotoProvider.Error.InvalidArgs",
                             "target size %ux%u out of range", w, h);
  // Photos in the cache were rendered for the old size. The backend applies
  // the new one before its next fetch.
  if (r > 0)
    pp_cache_invalidate(g_cache);
  printf("PhotoProvider requested target size %ux%u\n", w, h);
  return sd_bus_reply_method_return(m, NULL);
}

static int method_set_embed_qr(sd_bus_message *m, void *ud, sd_bus_error *err) {
  (void)ud;
  int v;
  int r = sd_bus_message_read(m, "b", &v);
  if (r < 0)
    return sd_bus_error_set_errno(err, -r);
  if (pp_backend_set_embed_qr(g_backend, v != 0) > 0)
    pp_cache_invalidate(g_cache);
  printf("PhotoProvider requested QR=%s\n", v ? "True" : "False");
  return sd_bus_reply_method_return(m, NULL);
}

static int method_set_album_filter(sd_bus_message *m, void *ud,
                                   sd_bus_error *err) {
  (void)ud;
  const char *name = NULL;
  const char *exclude = NULL;
  uint32_t from_year = 0;
  uint32_t to_year = 0;
  int r = sd_bus_message_read(m, "ssuu", &name, &exclude, &from_year, &to_year);
  if (r < 0)
    return sd_bus_error_set_errno(err, -r);

  struct pp_album_filter_config f = {
      .from_year = from_year,
      .to_year = to_year,
  };
  if (from_year > 9999 || to_year > 9999)
    return sd_bus_error_setf(
        err, "io.homeboard.PhotoProvider.Error.InvalidArgs",
        "years must be 0..9999, got %u..%u", from_year, to_year);
  // The year test is an overlap test, so a reversed range selects the albums
  // straddling the boundary rather than nothing -- which looks like the filter
  // being ignored, because pictures keep appearing.
  if (from_year != 0 && to_year != 0 && from_year > to_year)
    return sd_bus_error_setf(
        err, "io.homeboard.PhotoProvider.Error.InvalidArgs",
        "from_year %u is after to_year %u", from_year, to_year);
  // Truncating would apply a filter the caller didn't ask for
  if (name && strlen(name) >= sizeof(f.name))
    return sd_bus_error_setf(
        err, "io.homeboard.PhotoProvider.Error.InvalidArgs",
        "name is longer than %zu bytes", sizeof(f.name) - 1);
  if (exclude && strlen(exclude) >= sizeof(f.exclude))
    return sd_bus_error_setf(
        err, "io.homeboard.PhotoProvider.Error.InvalidArgs",
        "exclude is longer than %zu bytes", sizeof(f.exclude) - 1);
  if (name)
    strncpy(f.name, name, sizeof(f.name) - 1);
  if (exclude)
    strncpy(f.exclude, exclude, sizeof(f.exclude) - 1);

  r = pp_backend_set_album_filter(g_backend, &f);
  if (r < 0)
    return sd_bus_error_set(err, "io.homeboard.PhotoProvider.Error.InvalidArgs",
                            "the backend rejected the album filter");
  if (r > 0) {
    // Photos already in the cache may come from an album the new filter
    // excludes
    pp_cache_invalidate(g_cache);
    g_cfg->immich.album_filter = f;
    // A filter that only changed memory would silently revert on the next
    // restart, and the config file would disagree with what's on screen
    pp_config_save(g_cfg_path, g_cfg);
  }
  return sd_bus_reply_method_return(m, NULL);
}

static const sd_bus_vtable g_vtable[] = {
    SD_BUS_VTABLE_START(0),
    SD_BUS_METHOD("GetPhoto", "", "hs", method_get_photo,
                  SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("GetPrevPhoto", "", "hs", method_get_prev_photo,
                  SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("SetTargetSize", "uu", "", method_set_target_size,
                  SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("SetEmbedQr", "b", "", method_set_embed_qr,
                  SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("SetAlbumFilter", "ssuu", "", method_set_album_filter,
                  SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_VTABLE_END,
};

int pp_dbus_init(struct pp_backend *backend, struct pp_cache *cache,
                 struct pp_config *cfg, const char *cfg_path) {
  g_backend = backend;
  g_cache = cache;
  g_cfg = cfg;
  g_cfg_path = cfg_path;

  int r = sd_bus_open_system(&g_bus);
  if (r < 0) {
    fprintf(stderr, "sd_bus_open_system: %s\n", strerror(-r));
    return -1;
  }
  r = sd_bus_add_object_vtable(g_bus, &g_vtable_slot, DBUS_PATH, DBUS_INTERFACE,
                               g_vtable, NULL);
  if (r < 0) {
    fprintf(stderr, "sd_bus_add_object_vtable: %s\n", strerror(-r));
    return -1;
  }
  r = sd_bus_request_name(g_bus, DBUS_SERVICE, 0);
  if (r < 0) {
    fprintf(stderr, "sd_bus_request_name(%s): %s\n", DBUS_SERVICE,
            strerror(-r));
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
