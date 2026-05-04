#pragma once

#include "drm_mgr/drm_mgr.h"

#include <stddef.h>
#include <stdint.h>

struct Overlay;

struct Overlay *overlay_init(void);
void overlay_free(struct Overlay *o);

// Decodes a "data:image/png;base64,..." URL and caches the resulting QR
// image for future overlay_render() calls. Pass NULL or "" to clear.
// Safe to call from a different thread than overlay_render().
void overlay_set_qr(struct Overlay *o, const char *qr_data_url);

// Parses an in-memory SVG document and caches it for future overlay_render()
// calls. `data` need not be NUL-terminated; `len` is the number of bytes.
// Pass NULL/0 to clear. On parse failure the previous SVG (if any) is left
// in place.
// Safe to call from a different thread than overlay_render().
void overlay_set_from_svg_data(struct Overlay *o, const char *data, size_t len);

// Loads an SVG file from disk and caches the parsed image for future
// overlay_render() calls. Pass NULL to clear. On parse failure the previous
// SVG (if any) is left in place.
// Safe to call from a different thread than overlay_render().
void overlay_set_from_file(struct Overlay *o, const char *filename);

// Blits the cached QR and SVG (if any) onto fb.
void overlay_render(struct Overlay *o, uint32_t *fb, const struct fb_info *fbi);
