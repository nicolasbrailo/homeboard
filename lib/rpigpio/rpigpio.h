#pragma once

#include <stddef.h>
#include <stdint.h>

// Minimal GPIO + SPI interface for Raspberry Pi, using the Linux GPIO v2 and
// spidev kernel interfaces.

// fwd-decls
struct RpiGpio;
struct RpiSpi;

// GPIO -----------------------------------------------------------------

enum RpiGpioDir {
  RPIGPIO_INPUT,
  RPIGPIO_OUTPUT,
};

struct RpiGpioPin {
  int pin;
  enum RpiGpioDir direction;
  int initial_value; // only meaningful for RPIGPIO_OUTPUT
};

// Open /dev/gpiochipN and claim all pins. Returns NULL on failure.
// consumer is a label for the lines (visible in gpioinfo).
struct RpiGpio *rpigpio_open(int chip_num, const char *consumer, const struct RpiGpioPin *pins, int num_pins);

// Close the chip and release all claimed lines.
void rpigpio_close(struct RpiGpio *gpio);

// Read the current value of a claimed line. Returns 0 or 1, or -1 on error.
int rpigpio_read(struct RpiGpio *gpio, int pin);

// Write a value (0 or 1) to a claimed output line. Returns 0 on success.
int rpigpio_write(struct RpiGpio *gpio, int pin, int value);

// SPI ------------------------------------------------------------------

// Open /dev/spidevN.M at the given baud rate. Returns NULL on failure.
struct RpiSpi *rpispi_open(int dev, int channel, int baud);

// Close the SPI device.
void rpispi_close(struct RpiSpi *spi);

// Write count bytes. Returns count on success, -1 on error.
int rpispi_write(struct RpiSpi *spi, const uint8_t *buf, int count);
