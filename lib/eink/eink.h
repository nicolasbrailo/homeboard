#pragma once

#include <stdbool.h>
#include <stddef.h>

// Controller for https://www.waveshare.com/wiki/2.13inch_e-Paper_HAT_Manual
//
// Pixel wire format:
// * Looking at the display with the wires coming off the top and the ribbon
// cable to the right
// * There are 250*122 = 30500 pixels
// * 0,0 is at the bottom left of the display
// * Each pixel is "vertical" (ie 0XFF will draw 8 vertical pixels at 0,0)
// * Pixels are sent scanning from 0x0 to 0x122, 1x0 to 1x122...
// * Pixels are sent 8 at a time (so the first column is an array of 16 bytes)
// * Last drawable colum seems to be 237, even though display says 250. I'm sure
// it's a bug in my code.
//

// fwd-decls
struct EInkDisplay;
typedef struct _cairo cairo_t;

struct EInkConfig {
  bool mock_display;
  const char *save_render_to_png_file;
};

// Init device and wait until it's ready to operate
struct EInkDisplay *eink_init(struct EInkConfig *cfg);

// Disconnects from device after setting it to sleep/low power mode
// Takes a few seconds to complete
void eink_delete(struct EInkDisplay *display);

// Set display to background color
void eink_clear(struct EInkDisplay *display);

// Returns a drawing area. Use eink_render to display once complete
cairo_t *eink_get_cairo(struct EInkDisplay *display);

// Refresh the entire display
void eink_render(struct EInkDisplay *display);

// Quick (partial) refresh. Must call eink_invalidate_rect to mark the areas to
// redraw; without invalidating changed areas, the rendered image will be
// glitchy.
void eink_render_partial(struct EInkDisplay *display);

// Mark an area from the Cairo surface which has changed, and must be re-drawn
// on the next call to eink_render_partial
void eink_invalidate_rect(struct EInkDisplay *display, size_t x_start, size_t y_start, size_t x_end, size_t y_end);

void eink_quick_announce(struct EInkDisplay *display, const char *text);
