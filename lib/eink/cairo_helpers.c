#include <cairo/cairo.h>

#include "cairo_helpers.h"
#include <ctype.h>
#include <stdbool.h>
#include <string.h>

static bool fits(cairo_t *cr, const char *text, size_t i, size_t f) {
  const size_t sz = f - i;
  char buff[sz + 1];
  strncpy(buff, &text[i], sz);
  buff[sz] = '\0';

  cairo_text_extents_t extents;
  cairo_text_extents(cr, buff, &extents);

  cairo_surface_t *surface = cairo_get_target(cr);
  const int MARGIN = 5;
  const size_t max_width = cairo_image_surface_get_width(surface) - MARGIN;
  return (extents.width <= max_width);
}

static double get_ln_height(cairo_t *cr) {
  static double ln_height = -1;
  static cairo_t *known_cr = NULL;
  if (known_cr == cr) {
    // cache
    return ln_height;
  }

  char buff[10] = "HOLA";
  cairo_text_extents_t extents;
  cairo_text_extents(cr, buff, &extents);
  ln_height = extents.height;
  return extents.height;
}

static void cairo_println(cairo_t *cr, double render_x, double render_y, const char *text, size_t i, size_t f) {
  const size_t sz = f - i;
  char buff[sz + 1];
  strncpy(buff, &text[i], sz);
  buff[sz] = '\0';

  cairo_move_to(cr, render_x, render_y);
  cairo_show_text(cr, buff);
}

size_t cairo_render_text(cairo_t *cr, const char *text, size_t ln_offset) {
  size_t ln_start = 0;
  size_t prev_space_pos = 0;
  size_t next_space_pos = 0;

  const int MARGIN = 5;
  const double LN_HEIGHT = get_ln_height(cr) + MARGIN;
  size_t rendered_lines = 0;

  while (text[next_space_pos] != '\0') {
    while (text[next_space_pos] != '\0' && !isspace(text[next_space_pos])) {
      next_space_pos++;
    }

    if (fits(cr, text, ln_start, next_space_pos)) {
      if (text[next_space_pos] != '\0') {
        prev_space_pos = next_space_pos;
        next_space_pos += 1;
      } else {
        // End of text and it fits, print remaining
        cairo_println(cr, MARGIN, (rendered_lines + ln_offset) * LN_HEIGHT, text, ln_start, next_space_pos);
        rendered_lines++;
      }
    } else {
      // Didn't fit, we need to backtrack one word
      if (prev_space_pos == ln_start) {
        // Backtracking a word would send us to start of line, word must be
        // bigger than what we can print. Here we would break in a word, but
        // this algorithm is lazy and will just print long words as is (which
        // will probably truncate)
      } else {
        next_space_pos = prev_space_pos;
      }

      cairo_println(cr, MARGIN, (rendered_lines + ln_offset) * LN_HEIGHT, text, ln_start, next_space_pos);
      rendered_lines++;

      ln_start = next_space_pos + 1;
      prev_space_pos = next_space_pos + 1;
      next_space_pos += 1;
    }
  }

  return rendered_lines;
}
