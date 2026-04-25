#include "overlay.h"

#include <errno.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <cairo/cairo.h>

#include "eink/cairo_helpers.h"

struct Overlay {
  atomic_bool pending;
  char *msg;
};

struct Overlay *overlay_init(void) {
  struct Overlay *o = calloc(1, sizeof(*o));
  if (!o)
    return NULL;
  atomic_init(&o->pending, false);
  return o;
}

void overlay_free(struct Overlay *o) {
  if (!o)
    return;
  free(o->msg);
  free(o);
}

int overlay_request_announce(struct Overlay *o, uint32_t timeout_seconds,
                             const char *msg) {
  if (timeout_seconds <= 5 || timeout_seconds >= 30) {
    fprintf(
        stderr,
        "Announcement timeout '%u' invalid (must be in (5, 30); msg='%s')\n",
        timeout_seconds, msg ? msg : "");
    return -EINVAL;
  }
  if (atomic_load_explicit(&o->pending, memory_order_acquire)) {
    fprintf(
        stderr,
        "Announcement requested but one is already pending (new msg='%s')\n",
        msg ? msg : "");
    return -EBUSY;
  }
  printf("Announcement '%s' requested for %u seconds\n", msg ? msg : "",
         timeout_seconds);
  free(o->msg);
  o->msg = strdup(msg ? msg : "");
  atomic_store_explicit(&o->pending, true, memory_order_release);
  return 0;
}

void overlay_draw(void *ud, uint32_t *buf, uint32_t width, uint32_t height,
                  uint32_t stride, enum rotation rot) {
  (void)rot;
  struct Overlay *o = ud;
  if (!atomic_load_explicit(&o->pending, memory_order_acquire))
    return;

  cairo_surface_t *surface = cairo_image_surface_create_for_data(
      (unsigned char *)buf, CAIRO_FORMAT_RGB24, width, height, stride);
  if (cairo_surface_status(surface) == CAIRO_STATUS_SUCCESS) {
    cairo_t *cr = cairo_create(surface);
    // Dim the underlying picture so the text is readable.
    cairo_set_source_rgba(cr, 0, 0, 0, 0.6);
    cairo_paint(cr);

    cairo_set_source_rgba(cr, 1, 1, 1, 1);
    cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL,
                           CAIRO_FONT_WEIGHT_BOLD);
    cairo_set_font_size(cr, height / 8.0);
    cairo_render_text(cr, o->msg ? o->msg : "", 1);

    cairo_destroy(cr);
  }
  cairo_surface_destroy(surface);
  atomic_store_explicit(&o->pending, false, memory_order_release);
}
