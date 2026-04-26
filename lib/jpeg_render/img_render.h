#pragma once

#include <stdint.h>
#include <string.h>

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

enum horizontal_align {
  HORIZONTAL_ALIGN_LEFT,
  HORIZONTAL_ALIGN_CENTER,
  HORIZONTAL_ALIGN_RIGHT,
};

enum vertical_align {
  VERTICAL_ALIGN_TOP,
  VERTICAL_ALIGN_CENTER,
  VERTICAL_ALIGN_BOTTOM,
};

struct img_render_cfg {
  enum rotation rot;
  enum interpolation interp;
  enum horizontal_align h_align;
  enum vertical_align v_align;
};

static enum rotation img_render_cfg_parse_rot(uint32_t r) {
  switch (r) {
    case 90: return ROT_90;
    case 180: return ROT_180;
    case 270: return ROT_270;
    default: return ROT_0;
  }
}

static enum interpolation img_render_cfg_parse_interpolation(const char* x) {
  if (strcmp(x, "nearest") == 0) {
    return INTERP_NEAREST;
  } else {
    return INTERP_BILINEAR;
  }
}

static enum horizontal_align img_render_cfg_parse_horizontal_align(const char* x) {
  if (strcmp(x, "center") == 0) {
    return HORIZONTAL_ALIGN_CENTER;
  } else if (strcmp(x, "left") == 0) {
    return HORIZONTAL_ALIGN_LEFT;
  } else if (strcmp(x, "right") == 0) {
    return HORIZONTAL_ALIGN_RIGHT;
  } else {
    return HORIZONTAL_ALIGN_CENTER;
  }
}

static enum vertical_align img_render_cfg_parse_vertical_align(const char* x) {
  if (strcmp(x, "center") == 0) {
    return VERTICAL_ALIGN_CENTER;
  } else if (strcmp(x, "top") == 0) {
    return VERTICAL_ALIGN_TOP;
  } else if (strcmp(x, "bottom") == 0) {
    return VERTICAL_ALIGN_BOTTOM;
  } else {
    return VERTICAL_ALIGN_CENTER;
  }
}

// Render an RGB image into an XRGB8888 destination buffer.
// Scales to fit (preserving aspect ratio on the smallest axis),
// aligns per `cfg`, and applies rotation. Pixels outside the scaled
// image are cleared to black.
//
// `dst` must be at least `fb_height * fb_stride` bytes. It may be
// the scanout framebuffer (expect tearing) or a caller-owned scratch
// buffer that's memcpy'd to the fb after any further composition.
void img_render(uint32_t *dst, uint32_t fb_width, uint32_t fb_height, uint32_t fb_stride, const uint8_t *img_pixels,
                uint32_t img_width, uint32_t img_height, const struct img_render_cfg *cfg);
