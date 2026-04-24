#define _GNU_SOURCE
#include "slideshow.h"
#include "dbus.h"
#include "dbus_helpers.h"
#include "eink_meta.h"

#include <errno.h>
#include <pthread.h>
#include <semaphore.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <systemd/sd-bus.h>
#include <time.h>
#include <unistd.h>

#include "jpeg_render/img_render.h"
#include "jpeg_render/jpeg_loader.h"

#define DBUS_PHOTO_SERVICE "io.homeboard.PhotoProvider"
#define DBUS_PHOTO_PATH "/io/homeboard/PhotoProvider"
#define DBUS_PHOTO_INTERFACE "io.homeboard.PhotoProvider1"

// Lifecycle: start/stop/next/prev are only ever called from the main (dbus
// dispatch) thread, so worker_running needs no synchronization. The worker
// waits on wake_sem: stop(), next() and prev() all post it to cut the wait
// short. stop_requested disambiguates stop; skip_count disambiguates
// next-vs-prev: next() does +1, prev() does -1, and the worker reads the sign
// at the top of each iteration to pick the fetch direction. sem_post is
// durable — if it fires while the worker is mid-fetch, the next
// sem_timedwait returns immediately.
struct Slideshow {
  uint32_t *fb;
  // Off-screen buffer: img_render writes here, overlay_cb composes on top,
  // then we memcpy to fb. Keeps rendering tear-free and gives main() a hook
  // to draw on top of the image.
  uint32_t *compose_buf;
  size_t compose_buf_size;
  slideshow_overlay_fn overlay_cb;
  void *overlay_ud;
  struct fb_info fbi;
  atomic_uint transition_time_s;
  struct img_render_cfg render_cfg;
  uint32_t target_w;
  uint32_t target_h;
  bool embed_qr;

  // `bus` is shared with the main dispatch loop; only touched on the main
  // thread (monitor setup, push_initial_config, teardown). `worker_bus` is
  // private to the worker thread and used for blocking GetPhoto calls, so the
  // shared bus stays free of cross-thread access.
  sd_bus *bus;
  sd_bus *worker_bus;
  sd_bus_slot *photo_svc_monitor;
  pthread_t worker;
  bool worker_running;
  sem_t wake_sem;
  bool stop_requested;
  atomic_int skip_count;

  struct EinkMeta *eink_meta;

  // Shown at startup and any time we fail to fetch/decode a photo so the
  // screen never goes blank. NULL when disabled in config.
  char *fallback_image;
};

// (Re)open the worker's private system-bus connection. Closes any existing
// connection first so callers can use this both for initial connect and for
// recovery after the bus drops (e.g. dbus-daemon restart → ENOTCONN).
static int connect_worker_bus(struct Slideshow *s) {
  if (s->worker_bus) {
    sd_bus_flush_close_unref(s->worker_bus);
    s->worker_bus = NULL;
  }
  int r = sd_bus_open_system(&s->worker_bus);
  if (r < 0) {
    fprintf(stderr, "slideshow: sd_bus_open_system: %s\n", strerror(-r));
    return -1;
  }
  return 0;
}

static int fetch_one(struct Slideshow *s, const char *method, int *fd_out, char **meta_out) {
  sd_bus_error err = SD_BUS_ERROR_NULL;
  sd_bus_message *reply = NULL;
  int r = sd_bus_call_method(s->worker_bus, DBUS_PHOTO_SERVICE, DBUS_PHOTO_PATH, DBUS_PHOTO_INTERFACE, method, &err,
                             &reply, "");
  if (r == -ENOTCONN) {
    sd_bus_error_free(&err);
    err = SD_BUS_ERROR_NULL;
    fprintf(stderr, "%s: worker bus disconnected, reconnecting\n", method);
    if (connect_worker_bus(s) < 0)
      return -1;
    r = sd_bus_call_method(s->worker_bus, DBUS_PHOTO_SERVICE, DBUS_PHOTO_PATH, DBUS_PHOTO_INTERFACE, method, &err,
                           &reply, "");
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
  return 0;
}

static void render_meta(struct Slideshow *s, char *meta) {
  if (s->eink_meta)
    eink_meta_render(s->eink_meta, meta);
}

static bool render_fd(struct Slideshow *s, int fd) {
  struct jpeg_image *img = jpeg_load_fd(fd, s->fbi.width, s->fbi.height);
  if (!img) {
    fprintf(stderr, "jpeg decode failed\n");
    return false;
  }
  img_render(s->compose_buf, s->fbi.width, s->fbi.height, s->fbi.stride, img->pixels, img->width, img->height,
             &s->render_cfg);
  jpeg_free(img);
  if (s->overlay_cb)
    s->overlay_cb(s->overlay_ud, s->compose_buf, s->fbi.width, s->fbi.height, s->fbi.stride, s->render_cfg.rot);
  memcpy(s->fb, s->compose_buf, s->compose_buf_size);
  return true;
}

static void render_fallback(struct Slideshow *s) {
  if (!s->fallback_image)
    return;
  struct jpeg_image *img = jpeg_load(s->fallback_image, s->fbi.width, s->fbi.height);
  if (!img) {
    fprintf(stderr, "fallback image decode failed: %s\n", s->fallback_image);
    return;
  }
  img_render(s->compose_buf, s->fbi.width, s->fbi.height, s->fbi.stride, img->pixels, img->width, img->height,
             &s->render_cfg);
  jpeg_free(img);
  memcpy(s->fb, s->compose_buf, s->compose_buf_size);
  printf("Rendered fallback image: %s\n", s->fallback_image);
}

// Returns true if stop was requested, false on timeout or next-request.
static bool wait_or_stop(struct Slideshow *s, uint32_t secs) {
  struct timespec ts;
  clock_gettime(CLOCK_REALTIME, &ts);
  ts.tv_sec += secs;
  for (;;) {
    if (sem_timedwait(&s->wake_sem, &ts) == 0)
      return s->stop_requested;
    if (errno == ETIMEDOUT)
      return false;
    if (errno == EINTR)
      continue;
    perror("sem_timedwait");
    return false;
  }
}

static void *worker_main(void *ud) {
  struct Slideshow *s = ud;
  for (;;) {
    // Collapse any queued next/prev presses into a single step. Drain the sem FIRST so that
    // every post we observe (including the one that woke us) has already
    // committed its skip_count update, then read the counter.
    while (sem_trywait(&s->wake_sem) == 0) {
    }
    int skip = atomic_exchange(&s->skip_count, 0);
    const char *method = (skip < 0) ? "GetPrevPhoto" : "GetPhoto";

    int fd = -1;
    char *meta = NULL;
    if (fetch_one(s, method, &fd, &meta) == 0) {
      printf("Displaying new picture\n");
      render_meta(s, meta);
      if (render_fd(s, fd)) {
        ambience_dbus_emit_displaying_photo(s->worker_bus, meta);
      } else {
        render_fallback(s);
      }
      close(fd);
      free(meta);
    } else {
      render_fallback(s);
    }
    if (wait_or_stop(s, atomic_load(&s->transition_time_s)))
      break;
  }
  return NULL;
}

// Push initial config to photo-provider: target size matched to the physical
// screen (axes swapped for 90/270 rotation so the server renders at the
// correct aspect ratio) and embed_qr.
static int push_initial_config(sd_bus *bus, uint32_t w, uint32_t h, bool embed_qr) {
  sd_bus_error err = SD_BUS_ERROR_NULL;
  int r = sd_bus_call_method(bus, DBUS_PHOTO_SERVICE, DBUS_PHOTO_PATH, DBUS_PHOTO_INTERFACE, "SetTargetSize", &err,
                             NULL, "uu", w, h);
  if (r < 0) {
    fprintf(stderr, "SetTargetSize failed: %s\n", err.message ? err.message : strerror(-r));
    sd_bus_error_free(&err);
    return -1;
  }
  sd_bus_error_free(&err);

  r = sd_bus_call_method(bus, DBUS_PHOTO_SERVICE, DBUS_PHOTO_PATH, DBUS_PHOTO_INTERFACE, "SetEmbedQr", &err, NULL, "b",
                         (int)(embed_qr ? 1 : 0));
  if (r < 0) {
    fprintf(stderr, "SetEmbedQr failed: %s\n", err.message ? err.message : strerror(-r));
    sd_bus_error_free(&err);
    return -1;
  }
  sd_bus_error_free(&err);
  printf("photo-provider configured: %ux%u embed_qr=%d\n", w, h, embed_qr);
  return 0;
}

static void on_photo_svc_updown(void *ud, bool up) {
  if (!up) {
    fprintf(stderr, "WARNING: Photo service is down, slideshow will freeze on current picture\n");
    return;
  }

  struct Slideshow *s = ud;
  if (push_initial_config(s->bus, s->target_w, s->target_h, s->embed_qr) < 0) {
    fprintf(stderr, "WARNING: Failed to config photo provider service '%s'.\n", DBUS_PHOTO_SERVICE);
  }
}

struct Slideshow *slideshow_init(sd_bus *bus, uint32_t *fb, const struct fb_info *fbi, uint32_t transition_time_s,
                                 const struct img_render_cfg *render_cfg, bool embed_qr, bool use_eink_for_metadata,
                                 const char *fallback_image, slideshow_overlay_fn overlay_cb, void *overlay_ud) {
  if (!bus || !fb || !fbi || !render_cfg || transition_time_s == 0)
    return NULL;
  const uint32_t rotation_deg = (uint32_t)render_cfg->rot;
  if (rotation_deg != 0 && rotation_deg != 90 && rotation_deg != 180 && rotation_deg != 270) {
    fprintf(stderr, "slideshow_init: invalid rotation %u\n", rotation_deg);
    return NULL;
  }
  if (transition_time_s < 3 || transition_time_s > 300) {
    fprintf(stderr, "slideshow_init: invalid transition time %d, must be (3, 300]\n", transition_time_s);
  }
  struct Slideshow *s = calloc(1, sizeof(*s));
  if (!s)
    return NULL;
  s->fb = fb;
  s->fbi = *fbi;
  s->compose_buf_size = (size_t)fbi->height * fbi->stride;
  s->compose_buf = malloc(s->compose_buf_size);
  if (!s->compose_buf) {
    perror("malloc compose_buf");
    free(s);
    return NULL;
  }
  s->overlay_cb = overlay_cb;
  s->overlay_ud = overlay_ud;
  atomic_init(&s->transition_time_s, transition_time_s);
  s->render_cfg = *render_cfg;
  if (sem_init(&s->wake_sem, 0, 0) != 0) {
    perror("sem_init");
    free(s->compose_buf);
    free(s);
    return NULL;
  }
  atomic_init(&s->skip_count, 0);
  if (connect_worker_bus(s) < 0) {
    sem_destroy(&s->wake_sem);
    free(s->compose_buf);
    free(s);
    return NULL;
  }
  s->bus = bus;

  s->embed_qr = embed_qr;
  s->target_w = fbi->width;
  s->target_h = fbi->height;
  if (rotation_deg == 90 || rotation_deg == 270) {
    s->target_w = fbi->height;
    s->target_h = fbi->width;
  }

  if (use_eink_for_metadata) {
    s->eink_meta = eink_meta_init();
    if (!s->eink_meta)
      fprintf(stderr, "WARNING: eink metadata display unavailable\n");
  }

  if (fallback_image && fallback_image[0] != '\0') {
    s->fallback_image = strdup(fallback_image);
    render_fallback(s);
  }

  s->photo_svc_monitor = on_service_updown(s->bus, DBUS_PHOTO_SERVICE, on_photo_svc_updown, s);
  if (!is_service_up(s->bus, DBUS_PHOTO_SERVICE)) {
    fprintf(stderr, "WARNING: %s is not running; photos can't be displayed until it starts.\n", DBUS_PHOTO_SERVICE);
  }

  if (push_initial_config(s->bus, s->target_w, s->target_h, s->embed_qr) < 0) {
    // We'll retry if the service comes up later, but warn the user
    fprintf(stderr, "WARNING: Failed to config photo provider service '%s'.\n", DBUS_PHOTO_SERVICE);
  }

  return s;
}

void slideshow_free(struct Slideshow *s) {
  if (!s)
    return;
  slideshow_stop(s);
  sem_destroy(&s->wake_sem);
  if (s->photo_svc_monitor)
    sd_bus_slot_unref(s->photo_svc_monitor);
  if (s->worker_bus)
    sd_bus_flush_close_unref(s->worker_bus);
  eink_meta_free(s->eink_meta);
  free(s->compose_buf);
  free(s->fallback_image);
  free(s);
}

void slideshow_start(struct Slideshow *s) {
  if (s->worker_running)
    return;
  printf("Starting slideshow\n");
  s->worker_running = true;
  if (pthread_create(&s->worker, NULL, worker_main, s) != 0) {
    perror("pthread_create");
    s->worker_running = false;
  }
}

void slideshow_stop(struct Slideshow *s) {
  if (!s->worker_running)
    return;
  printf("Stopping slideshow\n");
  s->stop_requested = true;
  sem_post(&s->wake_sem);
  pthread_join(s->worker, NULL);
  s->worker_running = false;
  s->stop_requested = false;
  // Drain any leftover posts (e.g. a next() queued just before stop) so the
  // next start() begins with a clean wait.
  while (sem_trywait(&s->wake_sem) == 0) {
  }
  atomic_store(&s->skip_count, 0);
  if (s->eink_meta)
    eink_meta_clear(s->eink_meta);
}

void slideshow_next(struct Slideshow *s) {
  if (!s->worker_running)
    return;
  printf("User requested to advance to the next picture\n");
  atomic_fetch_add(&s->skip_count, 1);
  sem_post(&s->wake_sem);
}

void slideshow_prev(struct Slideshow *s) {
  if (!s->worker_running)
    return;
  printf("User requested to step back to the previous picture\n");
  atomic_fetch_sub(&s->skip_count, 1);
  sem_post(&s->wake_sem);
}

bool slideshow_set_transition_time_s(struct Slideshow *s, uint32_t seconds) {
  if (seconds < 3 || seconds > 300) {
    fprintf(stderr, "slideshow_set_transition_time_s: invalid transition time %u, must be [3, 300]\n", seconds);
    return false;
  }
  atomic_store(&s->transition_time_s, seconds);
  printf("Transition time updated to %u seconds\n", seconds);
  return true;
}
