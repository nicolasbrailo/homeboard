#include "overlay.h"

#include <cairo/cairo.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DATA_URL_PREFIX "data:image/png;base64,"
#define QR_SIZE_PCT 0.15
#define QR_PAD_PCT 0.02

struct Overlay {
  pthread_mutex_t mutex;
  cairo_surface_t *qr;
};

static uint8_t *base64_decode(const char *src, size_t src_len,
                              size_t *out_len) {
  uint8_t *out = malloc((src_len / 4 + 1) * 3);
  if (!out)
    return NULL;
  unsigned int val = 0;
  int bits = 0;
  size_t o = 0;
  for (size_t i = 0; i < src_len; i++) {
    char c = src[i];
    int d;
    if (c >= 'A' && c <= 'Z')
      d = c - 'A';
    else if (c >= 'a' && c <= 'z')
      d = c - 'a' + 26;
    else if (c >= '0' && c <= '9')
      d = c - '0' + 52;
    else if (c == '+')
      d = 62;
    else if (c == '/')
      d = 63;
    else if (c == '=')
      break;
    else if (c == '\n' || c == '\r' || c == ' ' || c == '\t')
      continue;
    else {
      free(out);
      return NULL;
    }
    val = (val << 6) | (unsigned)d;
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      out[o++] = (val >> bits) & 0xff;
    }
  }
  *out_len = o;
  return out;
}

struct png_reader {
  const uint8_t *data;
  size_t pos;
  size_t len;
};

static cairo_status_t png_read(void *closure, unsigned char *out,
                               unsigned int n) {
  struct png_reader *r = closure;
  if (r->pos + n > r->len)
    return CAIRO_STATUS_READ_ERROR;
  memcpy(out, r->data + r->pos, n);
  r->pos += n;
  return CAIRO_STATUS_SUCCESS;
}

static cairo_surface_t *decode_data_url(const char *url) {
  if (!url || !*url)
    return NULL;
  size_t prefix_len = strlen(DATA_URL_PREFIX);
  if (strncmp(url, DATA_URL_PREFIX, prefix_len) != 0) {
    fprintf(stderr, "overlay: unexpected qr data URL prefix\n");
    return NULL;
  }
  const char *b64 = url + prefix_len;
  size_t png_len = 0;
  uint8_t *png = base64_decode(b64, strlen(b64), &png_len);
  if (!png) {
    fprintf(stderr, "overlay: base64 decode failed\n");
    return NULL;
  }
  struct png_reader r = {.data = png, .pos = 0, .len = png_len};
  cairo_surface_t *s = cairo_image_surface_create_from_png_stream(png_read, &r);
  free(png);
  if (cairo_surface_status(s) != CAIRO_STATUS_SUCCESS) {
    fprintf(stderr, "overlay: PNG decode failed: %s\n",
            cairo_status_to_string(cairo_surface_status(s)));
    cairo_surface_destroy(s);
    return NULL;
  }
  return s;
}

struct Overlay *overlay_init(void) {
  struct Overlay *o = calloc(1, sizeof(*o));
  if (!o)
    return NULL;
  pthread_mutex_init(&o->mutex, NULL);
  return o;
}

void overlay_free(struct Overlay *o) {
  if (!o)
    return;
  if (o->qr)
    cairo_surface_destroy(o->qr);
  pthread_mutex_destroy(&o->mutex);
  free(o);
}

void overlay_set_qr(struct Overlay *o, const char *qr_data_url) {
  if (!o)
    return;
  cairo_surface_t *new_qr = decode_data_url(qr_data_url);
  pthread_mutex_lock(&o->mutex);
  cairo_surface_t *old = o->qr;
  o->qr = new_qr;
  pthread_mutex_unlock(&o->mutex);
  if (old)
    cairo_surface_destroy(old);
}

void overlay_render(struct Overlay *o, uint32_t *fb,
                    const struct fb_info *fbi) {
  if (!o || !fb || !fbi)
    return;
  pthread_mutex_lock(&o->mutex);
  if (!o->qr) {
    pthread_mutex_unlock(&o->mutex);
    return;
  }

  int qr_w = cairo_image_surface_get_width(o->qr);
  int qr_h = cairo_image_surface_get_height(o->qr);
  int qr_max = qr_w > qr_h ? qr_w : qr_h;

  uint32_t shorter = fbi->width < fbi->height ? fbi->width : fbi->height;
  double target = (double)shorter * QR_SIZE_PCT;
  double pad = (double)shorter * QR_PAD_PCT;
  double scale = target / (double)qr_max;
  double draw_w = qr_w * scale;
  double draw_h = qr_h * scale;
  double x = (double)fbi->width - draw_w - pad;
  double y = (double)fbi->height - draw_h - pad;

  cairo_surface_t *fb_surf = cairo_image_surface_create_for_data(
      (unsigned char *)fb, CAIRO_FORMAT_ARGB32, (int)fbi->width,
      (int)fbi->height, (int)fbi->stride);
  cairo_t *cr = cairo_create(fb_surf);
  cairo_translate(cr, x, y);
  cairo_scale(cr, scale, scale);
  cairo_set_source_surface(cr, o->qr, 0, 0);
  cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_NEAREST);
  cairo_paint(cr);
  cairo_destroy(cr);
  cairo_surface_flush(fb_surf);
  cairo_surface_destroy(fb_surf);

  pthread_mutex_unlock(&o->mutex);
}
