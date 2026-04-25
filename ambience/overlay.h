#pragma once

#include <stdint.h>

#include "jpeg_render/img_render.h"

// Announcement overlay. Holds a single pending announcement string and
// renders it on top of the next composed frame, then clears the pending
// flag (so the announcement appears for one frame — i.e. roughly one
// transition_time_s window).
//
// Both overlay_request_announce and overlay_draw run on the dispatch
// thread. EBUSY (returned by dbus_listeners) rejects a new announcement
// while one is already pending.

struct Overlay;

struct Overlay *overlay_init(void);
void overlay_free(struct Overlay *o);

// Returns 0 on success, -EINVAL for a bad timeout, -EBUSY if an
// announcement is already pending.
int overlay_request_announce(struct Overlay *o, uint32_t timeout_seconds,
                             const char *msg);

// Render hook compatible with render_overlay_cb. Composes the announcement
// (if pending) on top of `buf`, then clears the pending flag.
void overlay_draw(void *ud, uint32_t *buf, uint32_t width, uint32_t height,
                  uint32_t stride, enum rotation rot);
