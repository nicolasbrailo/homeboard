#define _GNU_SOURCE
#include <time.h>

#include <cairo/cairo.h>

#include "cairo_helpers.h"
#include "eink.h"
#include "rpigpio/rpigpio.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

struct EInkDisplay {
  struct EInkConfig cfg;

  struct RpiGpio *gpio;
  struct RpiSpi *spi;

  size_t width;
  size_t height;
  bool invert_color;

  cairo_surface_t *surface;
  cairo_t *cr;
  int cairo_fg_color;
  int cairo_bg_color;

  size_t byte_height; // ceil(height / 8)

  // Used as a temporary buffer when rendering
  size_t render_buff_sz;
  uint8_t *render_buff;
};

#define EPD_2in13_V4_WIDTH 122
#define EPD_2in13_V4_HEIGHT 250
#define EPD_DC_PIN 25
#define EPD_CS_PIN 8
#define EPD_SCLK_PIN 11
#define EPD_MOSI_PIN 10
#define EPD_PWR_PIN 18
#define EPD_BUSY_PIN 24

// On the manual (https://www.waveshare.com/wiki/2.13inch_e-Paper_HAT_Manual),
// this is connected to GPIO 17, but 9 is nicer (all of the pins are bunched
// together) #define EPD_RST_PIN 17
#define EPD_RST_PIN 9

static void sleep_ms(size_t xms) {
  struct timespec ts, rem;

  if (xms > 0) {
    ts.tv_sec = xms / 1000;
    ts.tv_nsec = (xms - (1000 * ts.tv_sec)) * 1000000L;

    while (clock_nanosleep(CLOCK_REALTIME, 0, &ts, &rem))
      ts = rem;
  }
}

static void dev_sleep_until_ready(struct EInkDisplay *display) {
  bool ready = false;
  for (int i = 0; i < 50; i++) {
    if (rpigpio_read(display->gpio, EPD_BUSY_PIN) == 0) {
      ready = true;
      break;
    }
    sleep_ms(10);
  }
  sleep_ms(10);

  if (!ready) {
    fprintf(stderr,
            "Warning: timeout while waiting for device to become ready\n");
  }
}

enum Cmd_Or_Data {
  TX_CMD,
  TX_DATA,
};

static void dev_tx(struct EInkDisplay *display, enum Cmd_Or_Data is_cmd,
                   uint8_t d) {
  const int v = (is_cmd == TX_CMD) ? 0 : 1;
  rpigpio_write(display->gpio, EPD_DC_PIN, v);
  rpigpio_write(display->gpio, EPD_CS_PIN, 0);
  rpispi_write(display->spi, &d, 1);
  rpigpio_write(display->gpio, EPD_CS_PIN, 1);
}

static void dev_wakeup(struct EInkDisplay *display, bool partial) {
  const int d = partial ? 0xff : 0xf7; // fast:0x0c, quality:0x0f, 0xcf
  dev_tx(display, TX_CMD, 0x22);       // Display Update Control
  dev_tx(display, TX_DATA, d);
  dev_tx(display, TX_CMD, 0x20); // Activate Display Update Sequence
  dev_sleep_until_ready(display);
}

static void dev_quick_reset(struct EInkDisplay *display) {
  dev_tx(display, TX_CMD, 0x01); // Driver output control
  dev_tx(display, TX_DATA, 0xF9);
  dev_tx(display, TX_DATA, 0x00);
  dev_tx(display, TX_DATA, 0x00);

  dev_tx(display, TX_CMD, 0x11); // data entry mode
  dev_tx(display, TX_DATA, 0x03);

  // SetWindows
  dev_tx(display, TX_CMD, 0x44); // SET_RAM_X_ADDRESS_START_END_POSITION
  dev_tx(display, TX_DATA, 0x00);
  dev_tx(display, TX_DATA, ((display->height - 1) >> 3) & 0xFF);

  dev_tx(display, TX_CMD, 0x45); // SET_RAM_Y_ADDRESS_START_END_POSITION
  dev_tx(display, TX_DATA, 0x00);
  dev_tx(display, TX_DATA, 0x00);
  dev_tx(display, TX_DATA, (display->width - 1) & 0xFF);
  dev_tx(display, TX_DATA, ((display->width - 1) >> 8) & 0xFF);

  // Set cursor
  dev_tx(display, TX_CMD, 0x4E); // SET_RAM_X_ADDRESS_COUNTER
  dev_tx(display, TX_DATA, 0x00);

  dev_tx(display, TX_CMD, 0x4F); // SET_RAM_Y_ADDRESS_COUNTER
  dev_tx(display, TX_DATA, 0x00);
  dev_tx(display, TX_DATA, 0x00);
}

static void dev_init(struct EInkDisplay *display) {
  // Reset device
  rpigpio_write(display->gpio, EPD_RST_PIN, 1);
  sleep_ms(20);
  rpigpio_write(display->gpio, EPD_RST_PIN, 0);
  sleep_ms(2);
  rpigpio_write(display->gpio, EPD_RST_PIN, 1);
  sleep_ms(20);

  // Init device
  dev_sleep_until_ready(display);
  dev_tx(display, TX_CMD, 0x12); // SWRESET
  dev_sleep_until_ready(display);

  dev_quick_reset(display);

  dev_tx(display, TX_CMD, 0x3C); // BorderWavefrom
  dev_tx(display, TX_DATA, 0x05);

  dev_tx(display, TX_CMD, 0x21); //  Display update control
  dev_tx(display, TX_DATA, 0x00);
  dev_tx(display, TX_DATA, 0x80);

  dev_tx(display, TX_CMD, 0x18); // Read built-in temperature sensor
  dev_tx(display, TX_DATA, 0x80);
  dev_sleep_until_ready(display);
}

static void dev_shutdown(struct EInkDisplay *display) {
  dev_tx(display, TX_CMD, 0x10); // enter deep sleep
  dev_tx(display, TX_DATA, 0x01);
  printf("Shutdown eInk, sleep 2s to sync\n");
  sleep_ms(2000); // important, at least 2s
}

static void dev_render(struct EInkDisplay *display, uint8_t *Image,
                       bool is_partial_update) {
  if (is_partial_update) {
    // Reset
    rpigpio_write(display->gpio, EPD_RST_PIN, 0);
    sleep_ms(1);
    rpigpio_write(display->gpio, EPD_RST_PIN, 1);

    dev_tx(display, TX_CMD, 0x3C); // BorderWavefrom
    dev_tx(display, TX_DATA, 0x80);
    dev_quick_reset(display);
  } else {
    // This seems to control how each pixel is updated in the display
    dev_tx(display, TX_CMD, 0x3C); // BorderWavefrom
    dev_tx(display, TX_DATA, 0x05);
  }

  // Write Black and White image to RAM
  dev_tx(display, TX_CMD, 0x24);

  for (size_t j = 0; j < display->width; j++) {
    for (size_t i = 0; i < display->byte_height; i++) {
      dev_tx(display, TX_DATA, Image[i + j * display->byte_height]);
    }
  }

  if (is_partial_update) {
    dev_tx(display, TX_CMD, 0x26);
    for (size_t j = 0; j < display->width; j++) {
      for (size_t i = 0; i < display->byte_height; i++) {
        dev_tx(display, TX_DATA, Image[i + j * display->byte_height]);
      }
    }
  }

  dev_wakeup(display, is_partial_update);
}

struct EInkDisplay *eink_init(struct EInkConfig *cfg) {
  struct EInkDisplay *display = calloc(1, sizeof(struct EInkDisplay));
  if (!display) {
    fprintf(stderr, "bad_alloc: display\n");
    return NULL;
  }

  display->cfg.mock_display = cfg->mock_display;
  display->cfg.save_render_to_png_file =
      cfg->save_render_to_png_file ? strdup(cfg->save_render_to_png_file)
                                   : NULL;
  if (cfg->save_render_to_png_file && !display->cfg.save_render_to_png_file) {
    fprintf(stderr, "bad_alloc: save_render_to_png_file\n");
    goto err;
  }

  display->width = EPD_2in13_V4_HEIGHT;
  display->height = EPD_2in13_V4_WIDTH;

  // Select black-on-white or reverse
  display->invert_color = false;
  display->cairo_fg_color = 1;
  display->cairo_bg_color = 0;

  // Create a monochrome (1-bit) surface
  display->surface = cairo_image_surface_create(CAIRO_FORMAT_A1, display->width,
                                                display->height);

  display->byte_height = (display->height + 7) / 8;
  display->render_buff_sz = display->byte_height * display->width;
  display->render_buff = malloc(display->render_buff_sz);
  display->cr = cairo_create(display->surface);

  if (!display->surface || !display->render_buff || !display->cr) {
    fprintf(stderr, "bad_alloc\n");
    goto err;
  }

  cairo_set_operator(display->cr, CAIRO_OPERATOR_SOURCE);

  // Set background color to white (transparent in A1 format)
  cairo_set_source_rgba(display->cr, 1, 1, 1, display->cairo_bg_color);
  cairo_paint(display->cr);

  if (display->cfg.mock_display) {
    if (display->cfg.save_render_to_png_file) {
      printf("Skip eInk display render, saving to %s instead\n",
             display->cfg.save_render_to_png_file);
    } else {
      printf("Skip eInk display render\n");
    }
  } else {
    if (access("/dev/spidev0.0", F_OK) != 0) {
      fprintf(stderr, "SPI is not enabled. Run 'dtparam spi=on' to enable it, "
                      "make it persistent with 'raspi-config "
                      "nonint do_spi 0'.\n");
      goto err;
    }

    const struct RpiGpioPin pins[] = {
        {EPD_BUSY_PIN, RPIGPIO_INPUT, 0},
        {EPD_RST_PIN, RPIGPIO_OUTPUT, 0},
        {EPD_DC_PIN, RPIGPIO_OUTPUT, 0},
        // The CS pin is managed by the SPI driver, no need to claim
        //{EPD_CS_PIN, RPIGPIO_OUTPUT, 0},
        {EPD_PWR_PIN, RPIGPIO_OUTPUT, 0},
    };
    const int num_pins = sizeof(pins) / sizeof(pins[0]);

    display->gpio = rpigpio_open(0, "eink", pins, num_pins);
    if (!display->gpio) {
      display->gpio = rpigpio_open(4, "eink", pins, num_pins);
      if (!display->gpio) {
        fprintf(stderr, "Can't find /dev/gpiochip[0|4]\n");
        goto err;
      }
    }

    display->spi = rpispi_open(0, 0, 10000000);
    if (!display->spi) {
      fprintf(stderr, "Can't open SPI\n");
      goto err;
    }

    rpigpio_write(display->gpio, EPD_CS_PIN, 1);
    rpigpio_write(display->gpio, EPD_PWR_PIN, 1);

    dev_init(display);
  }

  return display;

err:
  eink_delete(display);
  return NULL;
}

void eink_delete(struct EInkDisplay *display) {
  if (!display) {
    return;
  }

  if (display->cr) {
    cairo_destroy(display->cr);
  }

  if (display->surface) {
    cairo_surface_destroy(display->surface);
  }

  free(display->render_buff);

  if (display->gpio && display->spi) {
    dev_shutdown(display);
  }

  rpigpio_close(display->gpio);
  rpispi_close(display->spi);

  free((void *)display->cfg.save_render_to_png_file);
  free(display);
}

cairo_t *eink_get_cairo(struct EInkDisplay *display) { return display->cr; }

static void eink_render_impl(struct EInkDisplay *display, bool is_partial) {
  const size_t stride = cairo_image_surface_get_stride(display->surface);
  uint8_t *img_data = cairo_image_surface_get_data(display->surface);

  memset(display->render_buff, 0, display->render_buff_sz);

  for (size_t y = 0; y < display->height; y++) {
    for (size_t x = 0; x < display->width; x++) {
      const size_t src_byte_index = x / 8 + y * stride;
      const size_t src_bit_index = x % 8;

      // Display memory is rotated; rotate coordinates
      const size_t dest_x = display->height - 1 - y;
      const size_t dest_y = x;
      const size_t dest_byte_index = dest_x / 8 + dest_y * display->byte_height;
      const size_t dest_bit_index = dest_x % 8;

      bool pixel_set = img_data[src_byte_index] & (1 << src_bit_index);
      if (pixel_set == display->invert_color) {
        display->render_buff[dest_byte_index] |= (1 << (7 - dest_bit_index));
      } else {
        display->render_buff[dest_byte_index] &= ~(1 << (7 - dest_bit_index));
      }
    }
  }

  if (display->cfg.mock_display) {
    printf("EInk: skip render, mocking display\n");
  } else {
    dev_render(display, display->render_buff, is_partial);
  }

  if (display->cfg.save_render_to_png_file) {
    cairo_surface_write_to_png(display->surface,
                               display->cfg.save_render_to_png_file);
  }
}

void eink_render(struct EInkDisplay *display) {
  eink_render_impl(display, false);
}

void eink_render_partial(struct EInkDisplay *display) {
  eink_render_impl(display, true);
}

static int cairo_get_last_set_color(struct EInkDisplay *display) {
  cairo_pattern_t *pattern = cairo_get_source(display->cr);
  double r, g, b, a;
  if (cairo_pattern_get_rgba(pattern, &r, &g, &b, &a) != CAIRO_STATUS_SUCCESS) {
    return display->cairo_fg_color;
  }
  return a < 0.1 ? 0 : 1;
}

void eink_invalidate_rect(struct EInkDisplay *display, size_t x_start,
                          size_t y_start, size_t x_end, size_t y_end) {
  const int user_set_color = cairo_get_last_set_color(display);

  // Resetting to fg color and then to bg color may have better results?
  const int invalidate_color = display->cairo_bg_color;

  // "Cover" invalidated area with a box
  cairo_set_source_rgba(display->cr, 0, 0, 0, invalidate_color);
  cairo_rectangle(display->cr, x_start, y_start, x_end, y_end);
  cairo_fill(display->cr);
  cairo_stroke(display->cr);

  // Quick-draw to screen: this will force all pixels to cycle, and the real
  // call to render_partial by the user will set the right pixels without
  // glitches
  eink_render_partial(display);

  if (invalidate_color == display->cairo_fg_color) {
    // Reset invalidated area to blank
    cairo_set_source_rgba(display->cr, 0, 0, 0, display->cairo_bg_color);
    cairo_rectangle(display->cr, x_start, y_start, x_end, y_end);
    cairo_fill(display->cr);
  }

  // Restore "pen"
  cairo_set_source_rgba(display->cr, 0, 0, 0, user_set_color);
}

void eink_clear(struct EInkDisplay *display) {
  const int user_set_color = cairo_get_last_set_color(display);
  // Clear canvas
  cairo_set_source_rgba(display->cr, 0, 0, 0, display->cairo_bg_color);
  cairo_paint(display->cr);
  // Restore "pen"
  cairo_set_source_rgba(display->cr, 0, 0, 0, user_set_color);

  if (!display->cfg.mock_display) {
    dev_tx(display, TX_CMD, 0x24);
    for (size_t c = 0; c < 2; ++c) {
      for (size_t i = 0; i < display->width; ++i) {
        for (size_t j = 0; j < display->byte_height; ++j) {
          dev_tx(display, TX_DATA, display->invert_color ? 0x00 : 0xFF);
        }
      }

      dev_tx(display, TX_CMD, 0x26);
    }

    dev_wakeup(display, false);
  }
}

void eink_quick_announce(struct EInkDisplay *display, const char *msg) {
  cairo_t *cr = eink_get_cairo(display);
  cairo_surface_t *surface = cairo_get_target(cr);
  const double height = cairo_image_surface_get_height(surface);

  cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL,
                         CAIRO_FONT_WEIGHT_BOLD);

  // Binary search for the largest font size where word-wrapped text fits.
  // cairo_render_text uses ln_offset=1, so lines render at y positions:
  //   (0+1)*LH, (1+1)*LH, ..., lines*LH
  // where LH = line_height + MARGIN(5). The bottom of the last line extends
  // by ~line_height below that, so total = lines*LH + line_height.
  const int MARGIN = 5;
  double lo = 1, hi = height;
  while (hi - lo > 1) {
    double mid = (lo + hi) / 2;
    cairo_set_font_size(cr, mid);

    cairo_text_extents_t extents;
    cairo_text_extents(cr, "HOLA", &extents);
    double lh = extents.height + MARGIN;

    size_t lines = cairo_render_text(cr, msg, 1);
    double total_h = lines * lh + extents.height;

    if (total_h <= height) {
      lo = mid;
    } else {
      hi = mid;
    }
  }

  // Clear and render at the winning size
  cairo_set_source_rgba(cr, 0, 0, 0, 0);
  cairo_paint(cr);

  cairo_set_source_rgba(cr, 0, 0, 0, 1);
  cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL,
                         CAIRO_FONT_WEIGHT_BOLD);
  cairo_set_font_size(cr, lo);
  cairo_render_text(cr, msg, 1);

  eink_render(display);
}
