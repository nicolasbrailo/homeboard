#include "eink/eink.h"

#include <cairo/cairo.h>

#include <stdio.h>
#include <string.h>
#include <unistd.h>

static void usage(const char *prog) {
  fprintf(stderr, "Usage: %s [-w | -b | -d] [text...]\n", prog);
  fprintf(stderr, "  -w        Clear display to white\n");
  fprintf(stderr, "  -b        Clear display to black\n");
  fprintf(stderr, "  -d        Demo: show build timestamp with border\n");
  fprintf(stderr, "  text...   Render the given text on screen\n");
}

static void show_demo(struct EInkDisplay *display) {
  cairo_t *cr = eink_get_cairo(display);
  cairo_surface_t *surface = cairo_get_target(cr);
  const size_t width = cairo_image_surface_get_width(surface);
  const size_t height = cairo_image_surface_get_height(surface);

  cairo_set_source_rgba(cr, 0, 0, 0, 1);
  cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
  cairo_set_font_size(cr, 20);

  cairo_text_extents_t extents;
  const char *text = "Built @ " __TIME__;
  cairo_text_extents(cr, text, &extents);
  double x = (width - extents.width) / 2 - extents.x_bearing;
  double y = (height - extents.height) / 2 - extents.y_bearing;

  cairo_move_to(cr, x, y);
  cairo_show_text(cr, text);

  cairo_set_line_width(cr, 2);
  cairo_rectangle(cr, x + extents.x_bearing - 10, y + extents.y_bearing - 10, extents.width + 20, extents.height + 20);
  cairo_stroke(cr);

  eink_render(display);
}

int main(int argc, char **argv) {
  enum { MODE_TEXT, MODE_WHITE, MODE_BLACK, MODE_DEMO } mode = MODE_TEXT;
  int opt;

  while ((opt = getopt(argc, argv, "wbdh")) != -1) {
    switch (opt) {
    case 'w':
      mode = MODE_WHITE;
      break;
    case 'b':
      mode = MODE_BLACK;
      break;
    case 'd':
      mode = MODE_DEMO;
      break;
    default:
      usage(argv[0]);
      return (opt == 'h') ? 0 : 1;
    }
  }

  if (mode == MODE_TEXT && optind >= argc) {
    usage(argv[0]);
    return 1;
  }

  struct EInkConfig cfg = {0};
  struct EInkDisplay *display = eink_init(&cfg);
  if (!display)
    return 1;

  switch (mode) {
  case MODE_WHITE:
    eink_clear(display);
    break;

  case MODE_BLACK: {
    cairo_t *cr = eink_get_cairo(display);
    cairo_set_source_rgba(cr, 0, 0, 0, 1);
    cairo_paint(cr);
    eink_render(display);
    break;
  }

  case MODE_DEMO:
    show_demo(display);
    break;

  case MODE_TEXT: {
    // Join remaining argv into a single string
    char text[1024] = {0};
    for (int i = optind; i < argc; i++) {
      if (i > optind)
        strncat(text, " ", sizeof(text) - strlen(text) - 1);
      strncat(text, argv[i], sizeof(text) - strlen(text) - 1);
    }
    eink_quick_announce(display, text);
    break;
  }
  }

  eink_delete(display);
  return 0;
}
