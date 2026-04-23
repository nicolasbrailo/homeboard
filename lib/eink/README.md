# libeink

C library to drive a Waveshare 2.13" e-Paper HAT (V4) from a Raspberry Pi, using the Linux GPIO v2 and spidev kernel interfaces. Drawing is done via Cairo on a 1-bit surface; the library handles the SPI command protocol, full and partial refresh, and sleep/wake lifecycle.

Hardware: https://www.waveshare.com/wiki/2.13inch_e-Paper_HAT_Manual

## Prerequisites

- SPI enabled: `sudo raspi-config nonint do_spi 0` (requires reboot) or `sudo dtparam spi=on` (no reboot)
- Cairo development headers: `libcairo2-dev`

## API overview

| Function | Description |
|---|---|
| `eink_init` | Open GPIO/SPI, initialize the display, return a handle |
| `eink_delete` | Enter deep sleep and release resources |
| `eink_clear` | Set the entire display to the background color |
| `eink_get_cairo` | Get the Cairo context for drawing |
| `eink_render` | Full refresh of the display |
| `eink_render_partial` | Partial refresh (call `eink_invalidate_rect` first) |
| `eink_invalidate_rect` | Mark a region as dirty for the next partial refresh |
| `eink_quick_announce` | Render a text string, auto-sized to fill the display |

Set `EInkConfig.mock_display = true` to skip hardware access (useful for testing rendering with `save_render_to_png_file`).

## Files

- `eink.h` / `eink.c` - display driver and public API
- `cairo_helpers.h` / `cairo_helpers.c` - word-wrapping text layout for Cairo

