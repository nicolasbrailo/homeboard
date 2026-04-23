#pragma once

#include <stdint.h>

enum rotation {
  ROT_0 = 0,
  ROT_90 = 90,
  ROT_180 = 180,
  ROT_270 = 270,
};

enum interpolation {
  INTERP_NEAREST,
  INTERP_BILINEAR,
};

// Render an RGB image into an XRGB8888 destination buffer.
// Scales to fit (preserving aspect ratio on the smallest axis),
// centers, and applies rotation. Pixels outside the scaled image
// are cleared to black.
//
// `dst` must be at least `fb_height * fb_stride` bytes. It may be
// the scanout framebuffer (expect tearing) or a caller-owned scratch
// buffer that's memcpy'd to the fb after any further composition.
void img_render(uint32_t *dst, uint32_t fb_width, uint32_t fb_height, uint32_t fb_stride, const uint8_t *img_pixels,
                uint32_t img_width, uint32_t img_height, enum rotation rot, enum interpolation interp);
