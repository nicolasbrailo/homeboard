#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <systemd/sd-event.h>

#include "drm_mgr/drm_mgr.h"
#include "jpeg_render/img_render.h"

// Event-driven slideshow renderer. Runs entirely on the dispatch thread:
// transitions are an sd_event timer, fetches are async via photo_client.
// No worker thread, no synchronization.
//
// State is implicit: when running, either a fetch is in flight or the
// transition timer is armed. start() kicks off the first fetch; stop()
// cancels both. set_render_cfg synchronously re-renders the cached photo
// in place (if any) under the new config.

struct PhotoClient;
struct RenderLoop;

// Called immediately after a freshly fetched photo has been blitted to the
// framebuffer. `meta_json` is borrowed for the duration of the call. Use
// this to drive eink metadata rendering and to emit DisplayingPhoto.
typedef void (*render_photo_rendered_cb)(void *ud, const char *meta_json);

// Called after img_render but before the blit so the caller can compose on
// top. `buf` is screen-coord XRGB8888 sized `height * stride` bytes. `rot`
// is the screen rotation if the overlay content needs its own orientation.
// May be NULL (no overlay).
typedef void (*render_overlay_cb)(void *ud, uint32_t *buf, uint32_t width,
                                  uint32_t height, uint32_t stride,
                                  enum rotation rot);

// `event` is borrowed; the caller owns it and must outlive the loop.
// `fb` is borrowed (the DRM scanout buffer). `fbi` is copied. `render_cfg`
// is copied. `fallback_image` may be NULL or empty: when set it is rendered
// once during init and again any time a fetch/decode fails so the screen
// never goes blank. `photos` is borrowed. `on_photo_rendered` and `overlay`
// are copied; either or both may be NULL. Initially stopped.
struct RenderLoop *render_loop_init(
    sd_event *event, uint32_t *fb, const struct fb_info *fbi,
    uint32_t transition_time_s, const struct img_render_cfg *render_cfg,
    const char *fallback_image, struct PhotoClient *photos,
    render_photo_rendered_cb on_photo_rendered, void *on_photo_rendered_ud,
    render_overlay_cb overlay, void *overlay_ud);

void render_loop_free(struct RenderLoop *r);

// Start / stop the slideshow. Safe to call repeatedly.
void render_loop_start(struct RenderLoop *r);
void render_loop_stop(struct RenderLoop *r);

// Cancel any pending wait/fetch and step forward / backward.
void render_loop_next(struct RenderLoop *r);
void render_loop_prev(struct RenderLoop *r);

// Update the wait between pictures. Applies to subsequent timer arms.
// Returns false if `seconds` is outside the supported range.
bool render_loop_set_transition_time(struct RenderLoop *r, uint32_t seconds);

// Update render config. If a previously rendered photo is cached and the
// loop is running, it is re-rendered in place with the new config (the
// caller does not need to call render_loop_next). Returns false if `cfg`
// fails validation.
bool render_loop_set_render_cfg(struct RenderLoop *r,
                                const struct img_render_cfg *cfg);
