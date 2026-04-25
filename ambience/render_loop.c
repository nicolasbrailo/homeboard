#define _GNU_SOURCE
#include "render_loop.h"
#include "photo_client.h"

#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <systemd/sd-event.h>
#include <unistd.h>

#include "jpeg_render/img_render.h"
#include "jpeg_render/jpeg_loader.h"

struct RenderLoop {
  sd_event *event; // borrowed
  sd_event_source *timer;

  uint32_t *fb;
  // Off-screen buffer: img_render writes here, the overlay composes on top,
  // then we memcpy to fb. Keeps rendering tear-free and gives the overlay a
  // hook to draw on top of the image.
  uint32_t *compose_buf;
  size_t compose_buf_size;
  struct fb_info fbi;

  uint32_t transition_time_s;
  struct img_render_cfg render_cfg;

  // Last successfully decoded photo, cached so render_loop_set_render_cfg
  // can trigger an in-place re-render without re-fetching. NULL until the
  // first successful render.
  struct jpeg_image *last_img;

  struct PhotoClient *photos; // borrowed
  render_photo_rendered_cb on_photo_rendered;
  void *on_photo_rendered_ud;
  render_overlay_cb overlay;
  void *overlay_ud;

  bool running;

  // Painted at startup and any time we fail to fetch/decode a photo so the
  // screen never goes blank. NULL when disabled.
  char *fallback_image;
};

static bool valid_rotation(uint32_t deg) {
  return deg == 0 || deg == 90 || deg == 180 || deg == 270;
}

// Render `img` into the framebuffer under the current render_cfg, then run
// the overlay and blit. Caller owns `img`.
static void render_img(struct RenderLoop *r, const struct jpeg_image *img) {
  img_render(r->compose_buf, r->fbi.width, r->fbi.height, r->fbi.stride,
             img->pixels, img->width, img->height, &r->render_cfg);
  if (r->overlay)
    r->overlay(r->overlay_ud, r->compose_buf, r->fbi.width, r->fbi.height,
               r->fbi.stride, r->render_cfg.rot);
  memcpy(r->fb, r->compose_buf, r->compose_buf_size);
}

static bool render_fd(struct RenderLoop *r, int fd) {
  struct jpeg_image *img = jpeg_load_fd(fd, r->fbi.width, r->fbi.height);
  if (!img) {
    fprintf(stderr, "jpeg decode failed\n");
    return false;
  }
  render_img(r, img);
  if (r->last_img)
    jpeg_free(r->last_img);
  r->last_img = img;
  return true;
}

static void render_fallback(struct RenderLoop *r) {
  if (!r->fallback_image)
    return;
  struct jpeg_image *img =
      jpeg_load(r->fallback_image, r->fbi.width, r->fbi.height);
  if (!img) {
    fprintf(stderr, "fallback image decode failed: %s\n", r->fallback_image);
    return;
  }
  render_img(r, img);
  jpeg_free(img);
  printf("Rendered fallback image: %s\n", r->fallback_image);
}

static int arm_timer(struct RenderLoop *r) {
  uint64_t now = 0;
  int rc = sd_event_now(r->event, CLOCK_MONOTONIC, &now);
  if (rc < 0) {
    fprintf(stderr, "sd_event_now: %s\n", strerror(-rc));
    return rc;
  }
  rc = sd_event_source_set_time(r->timer,
                                now + (uint64_t)r->transition_time_s * 1000000ULL);
  if (rc < 0) {
    fprintf(stderr, "sd_event_source_set_time: %s\n", strerror(-rc));
    return rc;
  }
  rc = sd_event_source_set_enabled(r->timer, SD_EVENT_ONESHOT);
  if (rc < 0)
    fprintf(stderr, "sd_event_source_set_enabled: %s\n", strerror(-rc));
  return rc;
}

static void disarm_timer(struct RenderLoop *r) {
  sd_event_source_set_enabled(r->timer, SD_EVENT_OFF);
}

static void start_fetch(struct RenderLoop *r, bool prev);

static void on_fetch_done(int fd, const char *meta, void *ud) {
  struct RenderLoop *r = ud;
  if (!r->running)
    return;

  if (fd >= 0 && render_fd(r, fd)) {
    printf("Displaying new picture\n");
    if (r->on_photo_rendered)
      r->on_photo_rendered(r->on_photo_rendered_ud, meta);
  } else {
    render_fallback(r);
  }
  arm_timer(r);
}

static void start_fetch(struct RenderLoop *r, bool prev) {
  disarm_timer(r);
  if (photo_client_fetch_async(r->photos, prev, on_fetch_done, r) < 0)
    arm_timer(r);
}

static int on_timer(sd_event_source *s, uint64_t usec, void *ud) {
  (void)s;
  (void)usec;
  struct RenderLoop *r = ud;
  if (!r->running)
    return 0;
  start_fetch(r, false);
  return 0;
}

struct RenderLoop *render_loop_init(
    sd_event *event, uint32_t *fb, const struct fb_info *fbi,
    uint32_t transition_time_s, const struct img_render_cfg *render_cfg,
    const char *fallback_image, struct PhotoClient *photos,
    render_photo_rendered_cb on_photo_rendered, void *on_photo_rendered_ud,
    render_overlay_cb overlay, void *overlay_ud) {
  if (!event || !fb || !fbi || !render_cfg || !photos)
    return NULL;
  if (!valid_rotation((uint32_t)render_cfg->rot)) {
    fprintf(stderr, "render_loop_init: invalid rotation %u\n",
            (uint32_t)render_cfg->rot);
    return NULL;
  }
  if (transition_time_s < 3 || transition_time_s > 300) {
    fprintf(stderr,
            "render_loop_init: invalid transition time %u, must be [3, 300]\n",
            transition_time_s);
    return NULL;
  }

  struct RenderLoop *r = calloc(1, sizeof(*r));
  if (!r)
    return NULL;
  r->event = event;
  r->fb = fb;
  r->fbi = *fbi;
  r->compose_buf_size = (size_t)fbi->height * fbi->stride;
  r->compose_buf = malloc(r->compose_buf_size);
  if (!r->compose_buf) {
    perror("malloc compose_buf");
    free(r);
    return NULL;
  }
  r->transition_time_s = transition_time_s;
  r->render_cfg = *render_cfg;
  r->photos = photos;
  r->on_photo_rendered = on_photo_rendered;
  r->on_photo_rendered_ud = on_photo_rendered_ud;
  r->overlay = overlay;
  r->overlay_ud = overlay_ud;

  // Create the transition timer disabled; arm/disarm via set_enabled.
  int rc = sd_event_add_time(event, &r->timer, CLOCK_MONOTONIC, 0, 0, on_timer, r);
  if (rc < 0) {
    fprintf(stderr, "sd_event_add_time: %s\n", strerror(-rc));
    free(r->compose_buf);
    free(r);
    return NULL;
  }
  sd_event_source_set_enabled(r->timer, SD_EVENT_OFF);

  if (fallback_image && fallback_image[0] != '\0') {
    r->fallback_image = strdup(fallback_image);
    render_fallback(r);
  }

  return r;
}

void render_loop_free(struct RenderLoop *r) {
  if (!r)
    return;
  render_loop_stop(r);
  if (r->timer)
    sd_event_source_unref(r->timer);
  if (r->last_img)
    jpeg_free(r->last_img);
  free(r->compose_buf);
  free(r->fallback_image);
  free(r);
}

void render_loop_start(struct RenderLoop *r) {
  if (r->running)
    return;
  printf("Starting slideshow\n");
  r->running = true;
  start_fetch(r, false);
}

void render_loop_stop(struct RenderLoop *r) {
  if (!r->running)
    return;
  printf("Stopping slideshow\n");
  r->running = false;
  disarm_timer(r);
  photo_client_cancel_fetch(r->photos);
}

void render_loop_next(struct RenderLoop *r) {
  if (!r->running)
    return;
  printf("User requested to advance to the next picture\n");
  start_fetch(r, false);
}

void render_loop_prev(struct RenderLoop *r) {
  if (!r->running)
    return;
  printf("User requested to step back to the previous picture\n");
  start_fetch(r, true);
}

bool render_loop_set_transition_time(struct RenderLoop *r, uint32_t seconds) {
  if (seconds < 3 || seconds > 300) {
    fprintf(stderr,
            "render_loop_set_transition_time: invalid %u, must be [3, 300]\n",
            seconds);
    return false;
  }
  r->transition_time_s = seconds;
  printf("Transition time updated to %u seconds\n", seconds);
  return true;
}

bool render_loop_set_render_cfg(struct RenderLoop *r,
                                const struct img_render_cfg *cfg) {
  if (!r || !cfg)
    return false;
  if (!valid_rotation((uint32_t)cfg->rot)) {
    fprintf(stderr, "render_loop_set_render_cfg: invalid rotation %u\n",
            (uint32_t)cfg->rot);
    return false;
  }
  r->render_cfg = *cfg;
  printf("Render config updated: rot=%u interp=%d h_align=%d v_align=%d\n",
         (uint32_t)cfg->rot, (int)cfg->interp, (int)cfg->h_align,
         (int)cfg->v_align);

  // Re-render the cached photo in place under the new config. If the loop
  // isn't running or there's nothing cached yet, the next fetch will pick
  // up the new cfg naturally.
  if (r->running && r->last_img)
    render_img(r, r->last_img);
  return true;
}
