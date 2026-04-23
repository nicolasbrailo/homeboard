#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <systemd/sd-bus.h>

#include "drm_mgr/drm_mgr.h"
#include "jpeg_render/img_render.h"

struct Slideshow;

// Called after the image has been rendered into an off-screen compose buffer
// and before it's blitted to the scanout framebuffer. The callback may draw
// overlays (text, icons, ...) directly into `buf`. `buf` is in screen
// coordinates (image rotation is already applied) with XRGB8888 layout, sized
// `height * stride` bytes. `rot` is the screen rotation — use it if the
// overlay content needs its own orientation.
//
// The callback runs on the slideshow worker thread, not the dbus dispatch
// thread. If it reads state owned elsewhere (e.g. a dbus-updated string), the
// caller is responsible for synchronization.
// TODO: Refactor so that main drives the render loop, instead of having the slideshow provide a cb
typedef void (*slideshow_overlay_fn)(void *ud, uint32_t *buf, uint32_t width, uint32_t height, uint32_t stride,
                                     enum rotation rot);

// Creates a slideshow that renders into `fb` (borrowed; owned by the caller).
// `bus` is borrowed; owned by the caller and must outlive the slideshow.
// `overlay_cb` may be NULL for no overlay. `overlay_ud` is passed through.
// Initially stopped.
// `fallback_image` is a JPEG path rendered once during init to cover the gap
// between startup and the first photo arriving. NULL or empty leaves the
// framebuffer untouched.
struct Slideshow *slideshow_init(sd_bus *bus, uint32_t *fb, const struct fb_info *fbi, uint32_t transition_time_s,
                                 uint32_t rotation_deg, bool embed_qr, bool use_eink_for_metadata,
                                 const char *fallback_image, slideshow_overlay_fn overlay_cb, void *overlay_ud);

// Free the slideshow. Stops the worker if running.
void slideshow_free(struct Slideshow *s);

// Start / stop the worker. Safe to call repeatedly. Calling start when
// already running (or stop when already stopped) is a no-op.
void slideshow_start(struct Slideshow *s);
void slideshow_stop(struct Slideshow *s);

// Advance to the next picture immediately, cutting short the current wait.
// No-op when the worker is stopped.
void slideshow_next(struct Slideshow *s);

// Step back to the previous picture immediately. If no prior photo is held by
// photo-provider the request is dropped (the current picture stays on screen).
// No-op when the worker is stopped.
void slideshow_prev(struct Slideshow *s);

// Update the wait between pictures. The change takes effect at the next cycle;
// a sleep already in progress is not interrupted. Returns false if `seconds`
// is outside the supported range.
bool slideshow_set_transition_time_s(struct Slideshow *s, uint32_t seconds);
