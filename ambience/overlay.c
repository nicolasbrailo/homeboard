#include "overlay.h"

#include <cairo/cairo.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// NanoSVG triggers a few warnings the project treats as errors (anonymous
// unions under -std=gnu99 and float-equal comparisons). Suppress them just
// for these headers — we don't want to modify the upstream library.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wfloat-equal"
#ifdef __clang__
#pragma clang diagnostic ignored "-Wc11-extensions"
#endif
#define NANOSVG_IMPLEMENTATION
#include "nanosvg/src/nanosvg.h"
#define NANOSVGRAST_IMPLEMENTATION
#include "nanosvg/src/nanosvgrast.h"
#pragma GCC diagnostic pop

struct Overlay {
  pthread_mutex_t mutex;
  NSVGimage *svg;
  NSVGrasterizer *rast;
};

struct Overlay *overlay_init(void) {
  struct Overlay *o = calloc(1, sizeof(*o));
  if (!o)
    return NULL;
  o->rast = nsvgCreateRasterizer();
  if (!o->rast) {
    fprintf(stderr, "overlay: nsvgCreateRasterizer failed\n");
    free(o);
    return NULL;
  }
  pthread_mutex_init(&o->mutex, NULL);
  return o;
}

void overlay_free(struct Overlay *o) {
  if (!o)
    return;
  if (o->svg)
    nsvgDelete(o->svg);
  if (o->rast)
    nsvgDeleteRasterizer(o->rast);
  pthread_mutex_destroy(&o->mutex);
  free(o);
}

void overlay_set_from_file(struct Overlay *o, const char *filename) {
  NSVGimage *new_svg = NULL;
  if (filename) {
    new_svg = nsvgParseFromFile(filename, "px", 96.0f);
    if (!new_svg) {
      fprintf(stderr, "overlay: failed to parse SVG: %s\n", filename);
      return;
    }
    if (new_svg->width <= 0 || new_svg->height <= 0) {
      fprintf(stderr, "overlay: SVG has no usable dimensions: %s\n", filename);
      nsvgDelete(new_svg);
      return;
    }
  }
  pthread_mutex_lock(&o->mutex);
  NSVGimage *old = o->svg;
  o->svg = new_svg;
  pthread_mutex_unlock(&o->mutex);
  if (old)
    nsvgDelete(old);
}

void overlay_set_from_svg_data(struct Overlay *o, const char *data,
                               size_t len) {
  if (!o)
    return;
  NSVGimage *new_svg = NULL;
  if (data && len > 0) {
    // nsvgParse mutates the input buffer and requires NUL termination.
    char *buf = malloc(len + 1);
    if (!buf) {
      fprintf(stderr, "overlay: out of memory parsing SVG\n");
      return;
    }
    memcpy(buf, data, len);
    buf[len] = '\0';
    new_svg = nsvgParse(buf, "px", 96.0f);
    free(buf);
    if (!new_svg) {
      fprintf(stderr, "overlay: failed to parse SVG (%zu bytes)\n", len);
      return;
    }
    if (new_svg->width <= 0 || new_svg->height <= 0) {
      fprintf(stderr, "overlay: SVG has no usable dimensions\n");
      nsvgDelete(new_svg);
      return;
    }
  }
  pthread_mutex_lock(&o->mutex);
  NSVGimage *old = o->svg;
  o->svg = new_svg;
  pthread_mutex_unlock(&o->mutex);
  if (old)
    nsvgDelete(old);
}

// Rasterize SVG to fb dimensions and alpha-blend over the framebuffer.
// Cheap-and-cheerful prototype: rasterize on every render. Cache later.
static void render_svg(struct Overlay *o, uint32_t *fb,
                       const struct fb_info *fbi) {
  float scale_x = (float)fbi->width / o->svg->width;
  float scale_y = (float)fbi->height / o->svg->height;
  float scale = scale_x < scale_y ? scale_x : scale_y;

  unsigned char *rgba = malloc((size_t)fbi->width * fbi->height * 4);
  if (!rgba) {
    fprintf(stderr, "overlay: failed to alloc SVG raster buffer\n");
    return;
  }
  memset(rgba, 0, (size_t)fbi->width * fbi->height * 4);
  nsvgRasterize(o->rast, o->svg, 0, 0, scale, rgba, (int)fbi->width,
                (int)fbi->height, (int)fbi->width * 4);

  for (uint32_t y = 0; y < fbi->height; y++) {
    uint32_t *fb_row = (uint32_t *)((uint8_t *)fb + (size_t)y * fbi->stride);
    const uint8_t *src_row = rgba + (size_t)y * fbi->width * 4;
    for (uint32_t x = 0; x < fbi->width; x++) {
      uint8_t r = src_row[x * 4 + 0];
      uint8_t g = src_row[x * 4 + 1];
      uint8_t b = src_row[x * 4 + 2];
      uint8_t a = src_row[x * 4 + 3];
      if (a == 0)
        continue;
      if (a == 255) {
        fb_row[x] =
            (0xffu << 24) | ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
        continue;
      }
      uint32_t fbpx = fb_row[x];
      uint8_t fr = (fbpx >> 16) & 0xff;
      uint8_t fg = (fbpx >> 8) & 0xff;
      uint8_t fbb = fbpx & 0xff;
      uint8_t inv = 255 - a;
      uint8_t nr = (uint8_t)((r * a + fr * inv) / 255);
      uint8_t ng = (uint8_t)((g * a + fg * inv) / 255);
      uint8_t nb = (uint8_t)((b * a + fbb * inv) / 255);
      fb_row[x] =
          (0xffu << 24) | ((uint32_t)nr << 16) | ((uint32_t)ng << 8) | nb;
    }
  }

  free(rgba);
}

void overlay_render(struct Overlay *o, uint32_t *fb,
                    const struct fb_info *fbi) {
  pthread_mutex_lock(&o->mutex);
  if (o->svg)
    render_svg(o, fb, fbi);
  pthread_mutex_unlock(&o->mutex);
}
