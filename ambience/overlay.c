#include "overlay.h"

#include <cairo/cairo.h>
// cairo_helpers.h declares functions taking cairo_t*, so it must follow cairo.
#include "eink/cairo_helpers.h"
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

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

// One independent overlay layer. At most one of {svg, text} is set; both NULL
// means the layer is inactive. The rasterized result is cached in *logical*
// (upright) orientation as straight-alpha RGBA; cache_w/cache_h are the logical
// dims and cache_rot is the rotation it was built for (it is blitted into the
// fb through that rotation).
struct OverlayLayer {
  NSVGimage *svg;
  char *text;
  unsigned char *cache_rgba;
  uint32_t cache_w;
  uint32_t cache_h;
  enum rotation cache_rot;
  // 0 = no timeout; otherwise CLOCK_MONOTONIC nanoseconds at which to drop it.
  uint64_t deadline_ns;
};

// Two stacked layers: the SVG overlay is the base; the announce text is drawn
// on top of it (and takes precedence). When the announce expires, the base SVG
// overlay is shown again.
struct Overlay {
  pthread_mutex_t mutex;
  NSVGrasterizer *rast; // shared by any SVG layer
  struct OverlayLayer svg_layer;
  struct OverlayLayer text_layer;
};

// Logical (upright) canvas size for a given fb + rotation. For 90/270 the fb's
// width and height are swapped relative to what the viewer sees upright.
static void logical_dims(const struct fb_info *fbi, enum rotation rot, int *lw,
                         int *lh) {
  if (rot == ROT_90 || rot == ROT_270) {
    *lw = (int)fbi->height;
    *lh = (int)fbi->width;
  } else {
    *lw = (int)fbi->width;
    *lh = (int)fbi->height;
  }
}

static uint64_t monotonic_ns(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

// Install a new source into a layer (svg XOR text; pass both NULL to clear) and
// reset its deadline + raster cache. The previous text is freed inline (a cheap
// free); the previous SVG and raster cache are returned so the caller can free
// them outside the mutex (nsvgDelete can be slow).
static void layer_swap_locked(struct OverlayLayer *l, NSVGimage *new_svg,
                              char *new_text, uint32_t timeout_seconds,
                              NSVGimage **out_old_svg,
                              unsigned char **out_old_cache) {
  *out_old_svg = l->svg;
  l->svg = new_svg;
  free(l->text);
  l->text = new_text;
  *out_old_cache = l->cache_rgba;
  l->cache_rgba = NULL;
  l->cache_w = 0;
  l->cache_h = 0;
  const int have_src = (new_svg != NULL) || (new_text != NULL);
  l->deadline_ns =
      (have_src && timeout_seconds > 0)
          ? monotonic_ns() + (uint64_t)timeout_seconds * 1000000000ull
          : 0;
}

static bool layer_active(const struct OverlayLayer *l) {
  return l->svg != NULL || l->text != NULL;
}

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
  if (o->svg_layer.svg)
    nsvgDelete(o->svg_layer.svg);
  free(o->svg_layer.text);
  free(o->svg_layer.cache_rgba);
  if (o->text_layer.svg)
    nsvgDelete(o->text_layer.svg);
  free(o->text_layer.text);
  free(o->text_layer.cache_rgba);
  if (o->rast)
    nsvgDeleteRasterizer(o->rast);
  pthread_mutex_destroy(&o->mutex);
  free(o);
}

// Set the base SVG layer, leaving any announce text layer untouched.
static void set_svg_layer(struct Overlay *o, NSVGimage *new_svg,
                          uint32_t timeout_seconds) {
  NSVGimage *old;
  unsigned char *old_cache;
  pthread_mutex_lock(&o->mutex);
  layer_swap_locked(&o->svg_layer, new_svg, NULL, timeout_seconds, &old,
                    &old_cache);
  pthread_mutex_unlock(&o->mutex);
  if (old)
    nsvgDelete(old);
  free(old_cache);
}

void overlay_set_from_file(struct Overlay *o, const char *filename,
                           uint32_t timeout_seconds) {
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
  set_svg_layer(o, new_svg, timeout_seconds);
}

void overlay_set_from_svg_data(struct Overlay *o, const char *data, size_t len,
                               uint32_t timeout_seconds) {
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
  set_svg_layer(o, new_svg, timeout_seconds);
}

void overlay_set_text(struct Overlay *o, const char *text,
                      uint32_t timeout_seconds) {
  if (!o)
    return;
  char *new_text = NULL;
  if (text && *text) {
    new_text = strdup(text);
    if (!new_text) {
      fprintf(stderr, "overlay: out of memory copying announce text\n");
      return;
    }
  }
  NSVGimage *old;
  unsigned char *old_cache;
  pthread_mutex_lock(&o->mutex);
  layer_swap_locked(&o->text_layer, NULL, new_text, timeout_seconds, &old,
                    &old_cache);
  pthread_mutex_unlock(&o->mutex);
  if (old)
    nsvgDelete(old); // always NULL for the text layer, but keep the discipline
  free(old_cache);
}

// Rasterize a layer's SVG to a logical-sized (lw x lh) straight-alpha RGBA
// buffer in l->cache_rgba. Byte order per pixel is R,G,B,A (matching
// blit_cache()).
static void fill_cache_svg(struct Overlay *o, struct OverlayLayer *l, int lw,
                           int lh) {
  size_t bytes = (size_t)lw * lh * 4;
  unsigned char *rgba = malloc(bytes);
  if (!rgba) {
    fprintf(stderr, "overlay: failed to alloc SVG raster buffer\n");
    return;
  }
  memset(rgba, 0, bytes);

  float scale_x = (float)lw / l->svg->width;
  float scale_y = (float)lh / l->svg->height;
  float scale = scale_x < scale_y ? scale_x : scale_y;
  nsvgRasterize(o->rast, l->svg, 0, 0, scale, rgba, lw, lh, lw * 4);

  l->cache_rgba = rgba;
  l->cache_w = (uint32_t)lw;
  l->cache_h = (uint32_t)lh;
}

// Paint a layer's text over a dimmed scrim with cairo, then convert cairo's
// premultiplied-ARGB32 output into a logical-sized (lw x lh) straight-alpha
// R,G,B,A buffer in l->cache_rgba (matching blit_cache()).
static void fill_cache_text(struct OverlayLayer *l, int lw, int lh) {
  const int W = lw;
  const int H = lh;
  cairo_surface_t *surface =
      cairo_image_surface_create(CAIRO_FORMAT_ARGB32, W, H);
  if (cairo_surface_status(surface) != CAIRO_STATUS_SUCCESS) {
    fprintf(stderr, "overlay: cairo surface create failed\n");
    cairo_surface_destroy(surface);
    return;
  }
  cairo_t *cr = cairo_create(surface);

  // Dim scrim + red header band so the message reads over any photo (or the
  // base SVG overlay underneath it).
  const double band_h = H * 0.14;
  cairo_set_source_rgba(cr, 0, 0, 0, 0.60);
  cairo_paint(cr);
  cairo_set_source_rgba(cr, 0.75, 0.10, 0.10, 0.85);
  cairo_rectangle(cr, 0, 0, W, band_h);
  cairo_fill(cr);

  // Binary-search the largest bold font that word-wraps within the screen,
  // capped so short messages don't balloon. cairo_render_text() both measures
  // and draws, so intermediate draws are discarded by the repaint below.
  cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL,
                         CAIRO_FONT_WEIGHT_BOLD);
  const int MARGIN = 5;
  double lo = 8, hi = H * 0.5;
  if (hi > 140)
    hi = 140;
  while (hi - lo > 1) {
    double mid = (lo + hi) / 2;
    cairo_set_font_size(cr, mid);
    cairo_text_extents_t ext;
    cairo_text_extents(cr, "HOLA", &ext);
    double lh_px = ext.height + MARGIN;
    size_t lines = cairo_render_text(cr, l->text, 1);
    double total = lines * lh_px + ext.height;
    if (total <= H)
      lo = mid;
    else
      hi = mid;
  }

  // Repaint the background to erase measurement draws, then draw the final
  // white text at the winning size.
  cairo_set_source_rgba(cr, 0, 0, 0, 0.60);
  cairo_paint(cr);
  cairo_set_source_rgba(cr, 0.75, 0.10, 0.10, 0.85);
  cairo_rectangle(cr, 0, 0, W, band_h);
  cairo_fill(cr);
  cairo_set_font_size(cr, lo);
  cairo_set_source_rgba(cr, 1, 1, 1, 1);
  cairo_render_text(cr, l->text, 1);
  cairo_surface_flush(surface);

  size_t bytes = (size_t)W * H * 4;
  unsigned char *rgba = malloc(bytes);
  if (!rgba) {
    fprintf(stderr, "overlay: failed to alloc text raster buffer\n");
    cairo_destroy(cr);
    cairo_surface_destroy(surface);
    return;
  }
  const unsigned char *data = cairo_image_surface_get_data(surface);
  const int cstride = cairo_image_surface_get_stride(surface);
  for (int y = 0; y < H; y++) {
    const uint32_t *srow = (const uint32_t *)(data + (size_t)y * cstride);
    unsigned char *drow = rgba + (size_t)y * W * 4;
    for (int x = 0; x < W; x++) {
      // cairo ARGB32 is premultiplied, native-endian: 0xAARRGGBB.
      uint32_t p = srow[x];
      uint8_t a = (p >> 24) & 0xff;
      uint8_t r = (p >> 16) & 0xff;
      uint8_t g = (p >> 8) & 0xff;
      uint8_t b = p & 0xff;
      if (a != 0 && a != 255) {
        // Un-premultiply back to straight alpha (r <= a, so no clamp needed).
        r = (uint8_t)((r * 255) / a);
        g = (uint8_t)((g * 255) / a);
        b = (uint8_t)((b * 255) / a);
      }
      drow[x * 4 + 0] = r;
      drow[x * 4 + 1] = g;
      drow[x * 4 + 2] = b;
      drow[x * 4 + 3] = a;
    }
  }
  cairo_destroy(cr);
  cairo_surface_destroy(surface);

  l->cache_rgba = rgba;
  l->cache_w = (uint32_t)W;
  l->cache_h = (uint32_t)H;
}

// Alpha-blend a layer's cached (logical-orientation) overlay over the
// framebuffer, mapping each fb pixel back to a logical cache pixel with the
// same rotation convention img_render uses for the photo (see rotate_coords()
// there), so the overlay lines up with the picture under any rotation.
static void blit_cache(struct OverlayLayer *l, uint32_t *fb,
                       const struct fb_info *fbi, enum rotation rot) {
  const unsigned char *rgba = l->cache_rgba;
  const uint32_t lw = l->cache_w;
  const uint32_t lh = l->cache_h;
  for (uint32_t dy = 0; dy < fbi->height; dy++) {
    uint32_t *fb_row = (uint32_t *)((uint8_t *)fb + (size_t)dy * fbi->stride);
    for (uint32_t dx = 0; dx < fbi->width; dx++) {
      uint32_t lx, ly;
      switch (rot) {
      case ROT_90:
        lx = dy;
        ly = lh - 1 - dx;
        break;
      case ROT_180:
        lx = lw - 1 - dx;
        ly = lh - 1 - dy;
        break;
      case ROT_270:
        lx = lw - 1 - dy;
        ly = dx;
        break;
      default:
        lx = dx;
        ly = dy;
        break;
      }
      if (lx >= lw || ly >= lh)
        continue; // defensive; logical dims are derived from fb dims
      const uint8_t *src = rgba + ((size_t)ly * lw + lx) * 4;
      uint8_t r = src[0];
      uint8_t g = src[1];
      uint8_t b = src[2];
      uint8_t a = src[3];
      if (a == 0)
        continue;
      if (a == 255) {
        fb_row[dx] =
            (0xffu << 24) | ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
        continue;
      }
      uint32_t fbpx = fb_row[dx];
      uint8_t fr = (fbpx >> 16) & 0xff;
      uint8_t fg = (fbpx >> 8) & 0xff;
      uint8_t fbb = fbpx & 0xff;
      uint8_t inv = 255 - a;
      uint8_t nr = (uint8_t)((r * a + fr * inv) / 255);
      uint8_t ng = (uint8_t)((g * a + fg * inv) / 255);
      uint8_t nb = (uint8_t)((b * a + fbb * inv) / 255);
      fb_row[dx] =
          (0xffu << 24) | ((uint32_t)nr << 16) | ((uint32_t)ng << 8) | nb;
    }
  }
}

// Rasterize a layer to its raster cache if needed, then blit it. The cache is
// logical-orientation and reused until the source, the logical dims, or the
// rotation change.
static void render_layer(struct Overlay *o, struct OverlayLayer *l,
                         uint32_t *fb, const struct fb_info *fbi,
                         enum rotation rot) {
  int lw, lh;
  logical_dims(fbi, rot, &lw, &lh);
  if (!l->cache_rgba || l->cache_w != (uint32_t)lw ||
      l->cache_h != (uint32_t)lh || l->cache_rot != rot) {
    free(l->cache_rgba);
    l->cache_rgba = NULL;
    l->cache_w = 0;
    l->cache_h = 0;
    if (l->text)
      fill_cache_text(l, lw, lh);
    else if (l->svg)
      fill_cache_svg(o, l, lw, lh);
    if (!l->cache_rgba)
      return;
    l->cache_rot = rot;
  }
  blit_cache(l, fb, fbi, rot);
}

// Drop a layer if its deadline has passed, collecting freeable resources to
// release outside the mutex.
static void layer_expire_locked(struct OverlayLayer *l, uint64_t now,
                                NSVGimage **out_old_svg,
                                unsigned char **out_old_cache) {
  if (layer_active(l) && l->deadline_ns != 0 && now >= l->deadline_ns)
    layer_swap_locked(l, NULL, NULL, 0, out_old_svg, out_old_cache);
}

void overlay_render(struct Overlay *o, uint32_t *fb, const struct fb_info *fbi,
                    enum rotation rot) {
  NSVGimage *old_svg_base = NULL, *old_svg_top = NULL;
  unsigned char *old_cache_base = NULL, *old_cache_top = NULL;
  const uint64_t now = monotonic_ns();

  pthread_mutex_lock(&o->mutex);
  layer_expire_locked(&o->svg_layer, now, &old_svg_base, &old_cache_base);
  layer_expire_locked(&o->text_layer, now, &old_svg_top, &old_cache_top);

  // Base SVG overlay first, then the announce text on top of it.
  if (layer_active(&o->svg_layer))
    render_layer(o, &o->svg_layer, fb, fbi, rot);
  if (layer_active(&o->text_layer))
    render_layer(o, &o->text_layer, fb, fbi, rot);
  pthread_mutex_unlock(&o->mutex);

  if (old_svg_base)
    nsvgDelete(old_svg_base);
  if (old_svg_top)
    nsvgDelete(old_svg_top);
  free(old_cache_base);
  free(old_cache_top);
}
