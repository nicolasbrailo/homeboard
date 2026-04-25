#include <string.h>

#include "img_render.h"

// Map rotated coordinates back to original image coordinates.
static inline void rotate_coords(uint32_t sx, uint32_t sy, uint32_t w,
                                 uint32_t h, enum rotation rot, uint32_t *ix,
                                 uint32_t *iy) {
  // For ROT_90/ROT_270 the rotated canvas is h × w (dims swapped), so
  // sx ranges [0, h) and sy ranges [0, w). The subtract term must use the
  // original-axis length, not the rotated-axis length, or the uint32_t
  // underflows for non-square images.
  switch (rot) {
  case ROT_90:
    *ix = sy;
    *iy = h - 1 - sx;
    break;
  case ROT_180:
    *ix = w - 1 - sx;
    *iy = h - 1 - sy;
    break;
  case ROT_270:
    *ix = w - 1 - sy;
    *iy = sx;
    break;
  default:
    *ix = sx;
    *iy = sy;
    break;
  }
}

static inline uint32_t pixel_nearest(const uint8_t *pixels, uint32_t w,
                                     uint32_t h, float fx, float fy,
                                     enum rotation rot) {
  uint32_t sx = (uint32_t)fx;
  uint32_t sy = (uint32_t)fy;
  uint32_t src_w = (rot == ROT_90 || rot == ROT_270) ? h : w;
  uint32_t src_h = (rot == ROT_90 || rot == ROT_270) ? w : h;
  if (sx >= src_w)
    sx = src_w - 1;
  if (sy >= src_h)
    sy = src_h - 1;

  uint32_t ix, iy;
  rotate_coords(sx, sy, w, h, rot, &ix, &iy);
  const uint8_t *p = pixels + (iy * w + ix) * 3;
  return ((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | p[2];
}

static inline uint8_t lerp(uint8_t a, uint8_t b, float t) {
  return (uint8_t)(a + (b - a) * t);
}

static inline uint32_t pixel_bilinear(const uint8_t *pixels, uint32_t w,
                                      uint32_t h, float fx, float fy,
                                      enum rotation rot) {
  const uint32_t src_w = (rot == ROT_90 || rot == ROT_270) ? h : w;
  const uint32_t src_h = (rot == ROT_90 || rot == ROT_270) ? w : h;

  const uint32_t x0 = (uint32_t)fx;
  const uint32_t y0 = (uint32_t)fy;
  const uint32_t x1 = x0 + 1 < src_w ? x0 + 1 : x0;
  const uint32_t y1 = y0 + 1 < src_h ? y0 + 1 : y0;
  const float tx = fx - x0;
  const float ty = fy - y0;

  // Sample 4 corners in rotated space, map back to original
  uint32_t coords[4][2];
  rotate_coords(x0, y0, w, h, rot, &coords[0][0], &coords[0][1]);
  rotate_coords(x1, y0, w, h, rot, &coords[1][0], &coords[1][1]);
  rotate_coords(x0, y1, w, h, rot, &coords[2][0], &coords[2][1]);
  rotate_coords(x1, y1, w, h, rot, &coords[3][0], &coords[3][1]);

  const uint8_t *p00 = pixels + (coords[0][1] * w + coords[0][0]) * 3;
  const uint8_t *p10 = pixels + (coords[1][1] * w + coords[1][0]) * 3;
  const uint8_t *p01 = pixels + (coords[2][1] * w + coords[2][0]) * 3;
  const uint8_t *p11 = pixels + (coords[3][1] * w + coords[3][0]) * 3;

  const uint8_t r =
      lerp(lerp(p00[0], p10[0], tx), lerp(p01[0], p11[0], tx), ty);
  const uint8_t g =
      lerp(lerp(p00[1], p10[1], tx), lerp(p01[1], p11[1], tx), ty);
  const uint8_t b =
      lerp(lerp(p00[2], p10[2], tx), lerp(p01[2], p11[2], tx), ty);

  return ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
}

void img_render(uint32_t *dst, uint32_t fb_w, uint32_t fb_h, uint32_t fb_stride,
                const uint8_t *img_pixels, uint32_t img_w, uint32_t img_h,
                const struct img_render_cfg *cfg) {
  const enum rotation rot = cfg->rot;
  const enum interpolation interp = cfg->interp;

  // Rotated image dimensions
  uint32_t src_w = (rot == ROT_90 || rot == ROT_270) ? img_h : img_w;
  uint32_t src_h = (rot == ROT_90 || rot == ROT_270) ? img_w : img_h;

  // Scale to fit, preserving aspect ratio
  float scale_x = (float)fb_w / src_w;
  float scale_y = (float)fb_h / src_h;
  float scale = scale_x < scale_y ? scale_x : scale_y;

  uint32_t dst_w = (uint32_t)(src_w * scale);
  uint32_t dst_h = (uint32_t)(src_h * scale);

  uint32_t off_x;
  switch (cfg->h_align) {
  case HORIZONTAL_ALIGN_LEFT:
    off_x = 0;
    break;
  case HORIZONTAL_ALIGN_RIGHT:
    off_x = fb_w - dst_w;
    break;
  case HORIZONTAL_ALIGN_CENTER:
  default:
    off_x = (fb_w - dst_w) / 2;
    break;
  }

  uint32_t off_y;
  switch (cfg->v_align) {
  case VERTICAL_ALIGN_TOP:
    off_y = 0;
    break;
  case VERTICAL_ALIGN_BOTTOM:
    off_y = fb_h - dst_h;
    break;
  case VERTICAL_ALIGN_CENTER:
  default:
    off_y = (fb_h - dst_h) / 2;
    break;
  }

  // Clear dst to black
  for (uint32_t y = 0; y < fb_h; y++) {
    uint32_t *row = (uint32_t *)((uint8_t *)dst + y * fb_stride);
    memset(row, 0, fb_w * 4);
  }

  for (uint32_t y = 0; y < dst_h; y++) {
    uint32_t *row = (uint32_t *)((uint8_t *)dst + (y + off_y) * fb_stride);
    float fy = (float)y / scale;
    for (uint32_t x = 0; x < dst_w; x++) {
      float fx = (float)x / scale;
      if (interp == INTERP_BILINEAR)
        row[x + off_x] = pixel_bilinear(img_pixels, img_w, img_h, fx, fy, rot);
      else
        row[x + off_x] = pixel_nearest(img_pixels, img_w, img_h, fx, fy, rot);
    }
  }
}
