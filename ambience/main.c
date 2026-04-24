#include "config.h"
#include "dbus.h"
#include "display.h"
#include "slideshow.h"

#include "drm_mgr/drm_mgr.h"

#include <cairo/cairo.h>

#include "eink/cairo_helpers.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <stdatomic.h>
#include <systemd/sd-bus.h>

static volatile sig_atomic_t g_quit;

static void sig_handler(int sig) {
  (void)sig;
  g_quit = 1;
}

struct ambience_ctx {
  sd_bus *bus;
  struct Slideshow *slideshow;
  struct Display *display;
  atomic_bool announcement_pending;
  char* announcement;
};

static void on_display_turned_on(void *ud) {
  struct ambience_ctx *ctx = ud;
  slideshow_start(ctx->slideshow);
  ambience_dbus_emit_slideshow_active(ctx->bus, true);
}

static void on_display_turned_off(void *ud) {
  struct ambience_ctx *ctx = ud;
  slideshow_stop(ctx->slideshow);
  ambience_dbus_emit_slideshow_active(ctx->bus, false);
}

static void on_slideshow_next(void *ud) { slideshow_next(((struct ambience_ctx *)ud)->slideshow); }

static void on_slideshow_prev(void *ud) { slideshow_prev(((struct ambience_ctx *)ud)->slideshow); }

static void on_force_on(void *ud) { display_force_on(((struct ambience_ctx *)ud)->display); }

static void on_force_off(void *ud) { display_force_off(((struct ambience_ctx *)ud)->display); }

static bool on_set_transition_time(void *ud, uint32_t seconds) {
  return slideshow_set_transition_time_s(((struct ambience_ctx *)ud)->slideshow, seconds);
}

static int on_announce_requested_cb(void *ud, uint32_t timeout_seconds, const char* msg) {
  if (timeout_seconds <= 5 || timeout_seconds >= 30) {
    fprintf(stderr, "User requested announcement, but timeout '%d' is not valid. Must be [5, 30] (new announcement: '%s')\n", timeout_seconds, msg);
    return -EINVAL;
  }

  struct ambience_ctx *ctx = ud;
  if (atomic_load_explicit(&ctx->announcement_pending, memory_order_acquire)) {
    fprintf(stderr, "User requested announcement, but one is already pending (new announcement: '%s')\n", msg);
    return -EBUSY;
  }

  printf("Announcement '%s' requested for %d seconds\n", msg, timeout_seconds);
  free(ctx->announcement);
  ctx->announcement = strdup(msg);
  atomic_store_explicit(&ctx->announcement_pending, true, memory_order_release);
  return 0;
}

static void on_overlay(void *ud, uint32_t *buf, uint32_t width, uint32_t height, uint32_t stride, enum rotation rot) {
  struct ambience_ctx *ctx = ud;
  (void)rot;

  // XXX TODO
  return;

  if (atomic_load_explicit(&ctx->announcement_pending, memory_order_acquire)) {
    cairo_surface_t *surface =
        cairo_image_surface_create_for_data((unsigned char *)buf, CAIRO_FORMAT_RGB24, width, height, stride);
    if (cairo_surface_status(surface) == CAIRO_STATUS_SUCCESS) {
      cairo_t *cr = cairo_create(surface);
      // Dim the underlying picture so the text is readable.
      cairo_set_source_rgba(cr, 0, 0, 0, 0.6);
      cairo_paint(cr);

      cairo_set_source_rgba(cr, 1, 1, 1, 1);
      cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
      cairo_set_font_size(cr, height / 8.0);
      cairo_render_text(cr, ctx->announcement, 1);

      cairo_destroy(cr);
    }
    cairo_surface_destroy(surface);
    atomic_store_explicit(&ctx->announcement_pending, false, memory_order_release);
  }

  const uint32_t box = 100;
  const uint32_t color = 0x00FF00FF; // magenta (XRGB8888)
  const uint32_t w = box < width ? box : width;
  const uint32_t h = box < height ? box : height;
  for (uint32_t y = 0; y < h; y++) {
    uint32_t *row = (uint32_t *)((uint8_t *)buf + y * stride);
    for (uint32_t x = 0; x < w; x++)
      row[x] = color;
  }
}

int main(int argc, char *argv[]) {
  if (argc < 2) {
    fprintf(stderr, "Usage: %s <config.json>\n", argv[0]);
    return 1;
  }

  struct ambience_config cfg;
  if (ambience_config_load(argv[1], &cfg) < 0)
    return 1;

  signal(SIGTERM, sig_handler);
  signal(SIGINT, sig_handler);

  int ret = 0;
  sd_bus *bus = NULL;
  struct fb_info fbi;
  struct DRM_Mgr *drm_mgr = NULL;
  uint32_t *fb = NULL;
  struct Slideshow *slideshow = NULL;
  struct Display *display = NULL;
  struct AmbienceDbus *dbus_mgr = NULL;

  int r = sd_bus_open_system(&bus);
  if (r < 0 || !bus) {
    fprintf(stderr, "sd_bus_open_system: %s\n", strerror(-r));
    ret = 1;
    goto end;
  }

  drm_mgr = drm_mgr_init();
  fb = drm_mgr_acquire_fb(drm_mgr, &fbi);
  if (!drm_mgr || !fb) {
    ret = 1;
    goto end;
  }

  struct ambience_ctx ctx = {.bus = bus};
  atomic_init(&ctx.announcement_pending, false);
  slideshow = slideshow_init(bus, fb, &fbi, cfg.transition_time_s, &cfg.render, cfg.embed_qr,
                             cfg.use_eink_for_metadata, cfg.fallback_image, on_overlay, &ctx);
  ctx.slideshow = slideshow;
  display = display_init(bus, on_display_turned_on, on_display_turned_off, &ctx);
  ctx.display = display;
  dbus_mgr = ambience_dbus_init(bus, on_slideshow_next, on_slideshow_prev, on_force_on, on_force_off,
                                on_set_transition_time, on_announce_requested_cb, &ctx);
  if (!slideshow || !display || !dbus_mgr) {
    ret = 1;
    goto end;
  }

  printf("Running ambience service\n");
  while (!g_quit) {
    if (ambience_dbus_run_once(dbus_mgr) < 0)
      break;
  }

end:
  slideshow_free(slideshow);
  display_free(display);
  ambience_dbus_free(dbus_mgr);
  if (bus)
    sd_bus_flush_close_unref(bus);
  drm_mgr_release_fb(drm_mgr);
  drm_mgr_free(drm_mgr);
  return ret;
}
