#pragma once

#include "drm_mgr/drm_mgr.h"

#include <stdint.h>

struct Overlay;

struct Overlay *overlay_init(void);
void overlay_free(struct Overlay *o);

// Decodes a "data:image/png;base64,..." URL and caches the resulting QR
// image for future overlay_render() calls. Pass NULL or "" to clear.
// Safe to call from a different thread than overlay_render().
void overlay_set_qr(struct Overlay *o, const char *qr_data_url);

// Blits the cached QR onto fb (no-op if none cached).
void overlay_render(struct Overlay *o, uint32_t *fb,
                    const struct fb_info *fbi);
