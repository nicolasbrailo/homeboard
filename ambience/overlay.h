#pragma once

#include "drm_mgr/drm_mgr.h"

#include <stddef.h>
#include <stdint.h>

struct Overlay;

struct Overlay *overlay_init(void);
void overlay_free(struct Overlay *o);

// Parses an in-memory SVG document and caches it for future overlay_render()
// calls. `data` need not be NUL-terminated; `len` is the number of bytes.
// Pass NULL/0 to clear. On parse failure the previous SVG (if any) is left
// in place. `timeout_seconds` of 0 means no timeout; otherwise the overlay
// is dropped on the first overlay_render() call after the deadline.
// Safe to call from a different thread than overlay_render().
void overlay_set_from_svg_data(struct Overlay *o, const char *data, size_t len,
                               uint32_t timeout_seconds);

// Loads an SVG file from disk and caches the parsed image for future
// overlay_render() calls. Pass NULL to clear. On parse failure the previous
// SVG (if any) is left in place. `timeout_seconds` of 0 means no timeout;
// otherwise the overlay is dropped on the first overlay_render() call after
// the deadline.
// Safe to call from a different thread than overlay_render().
void overlay_set_from_file(struct Overlay *o, const char *filename,
                           uint32_t timeout_seconds);

// Blits the cached SVG (if any) onto fb.
void overlay_render(struct Overlay *o, uint32_t *fb, const struct fb_info *fbi);
