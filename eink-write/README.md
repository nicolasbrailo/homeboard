# eink-write

CLI tool to write text or patterns to a Waveshare 2.13" e-Paper display using libeink.

## Usage

```
eink-write [options] [text...]
```

### Modes

| Flag | Description |
|---|---|
| (none) | Render the given text, auto-sized to fill the display |
| `-w` | Clear the display to white |
| `-b` | Fill the display black |
| `-d` | Demo: show build timestamp with a border |
| `-h` | Print usage |

### Examples

```bash
# Display a message
eink-write Hello World

# Clear to white
eink-write -w

# Run the demo pattern
eink-write -d
```

## Building

```bash
make
```

## Requirements

- SPI must be enabled (`sudo raspi-config nonint do_spi 0`, reboot required)
- Sufficient permissions to access `/dev/gpiochip*` and `/dev/spidev0.0`

## Installation

The wiring follows the one from the manual, but puts RST in pin 21 of the GPIO (GPIO 9/MISO). This means all of the pins are neatly bunched together instead of all over the place.

For completeness, from https://www.waveshare.com/wiki/2.13inch_e-Paper_HAT_Manual:

e-Paper          Raspberry Pi
            BCM2835         Board
VCC         3.3V            3.3V
GND         GND             GND
DIN         MOSI            19
CLK         SCLK            23
CS          CE0             24
DC          25              22
RST         ~17~ MISO/9     ~11~ 21
BUSY        24              18

Pinout: https://images.theengineeringprojects.com/image/webp/2021/03/raspberry-pi-zero-5.png.webp

