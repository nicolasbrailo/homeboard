#pragma once

#include "drm_mgr/drm_mgr.h"
#include "jpeg_render/img_render.h" // enum rotation

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

// Sets a word-wrapped text announcement, rendered (via cairo) over a dimmed
// scrim on the next overlay_render() call. Unlike the SVG paths, this can show
// real text. Pass NULL/empty to clear. The announce text is an independent
// layer drawn ON TOP of any active SVG overlay. When expires, the overlay is
// shown again. `timeout_seconds` of 0 means no timeout.
// Safe to call from a different thread than overlay_render().
void overlay_set_text(struct Overlay *o, const char *text,
                      uint32_t timeout_seconds);

// Blits the active overlay (if any) onto fb, rotated to match `rot` so it lines
// up with the photo (which img_render draws with the same rotation).
void overlay_render(struct Overlay *o, uint32_t *fb, const struct fb_info *fbi,
                    enum rotation rot);
