#include "rpigpio.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/gpio.h>
#include <linux/spi/spidev.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

// GPIO -----------------------------------------------------------------

#define MAX_LINES 64

struct RpiGpioLine {
  int fd;    // line request fd, -1 if unclaimed
  int flags; // GPIO_V2_LINE_FLAG_INPUT or _OUTPUT
};

struct RpiGpio {
  int chip_fd;
  struct RpiGpioLine lines[MAX_LINES];
  int num_lines;
};

static int claim_line(struct RpiGpio *gpio, const char *consumer, int pin,
                      uint64_t flags, int *value) {
  if (pin < 0 || pin >= gpio->num_lines) {
    return -1;
  }

  // Release if already claimed
  if (gpio->lines[pin].fd >= 0) {
    close(gpio->lines[pin].fd);
    gpio->lines[pin].fd = -1;
  }

  struct gpio_v2_line_request req = {0};
  req.offsets[0] = pin;
  req.num_lines = 1;
  req.config.flags = flags;
  strncpy(req.consumer, consumer, sizeof(req.consumer));

  if (value) {
    req.config.num_attrs = 1;
    req.config.attrs[0].attr.id = GPIO_V2_LINE_ATTR_ID_OUTPUT_VALUES;
    req.config.attrs[0].mask = 1;
    req.config.attrs[0].attr.values = *value ? 1 : 0;
  }

  if (ioctl(gpio->chip_fd, GPIO_V2_GET_LINE_IOCTL, &req)) {
    return -1;
  }

  gpio->lines[pin].fd = req.fd;
  gpio->lines[pin].flags = flags;
  return 0;
}

struct RpiGpio *rpigpio_open(int chip_num, const char *consumer,
                             const struct RpiGpioPin *pins, int num_pins) {
  char path[64];
  sprintf(path, "/dev/gpiochip%d", chip_num);

  int fd = open(path, O_RDWR | O_CLOEXEC);
  if (fd < 0) {
    perror(path);
    return NULL;
  }

  struct gpiochip_info info = {0};
  if (ioctl(fd, GPIO_GET_CHIPINFO_IOCTL, &info)) {
    perror("GPIO_GET_CHIPINFO_IOCTL");
    close(fd);
    return NULL;
  }

  struct RpiGpio *gpio = calloc(1, sizeof(*gpio));
  if (!gpio) {
    close(fd);
    return NULL;
  }

  gpio->chip_fd = fd;
  gpio->num_lines = info.lines < MAX_LINES ? (int)info.lines : MAX_LINES;

  for (int i = 0; i < MAX_LINES; i++) {
    gpio->lines[i].fd = -1;
  }

  for (int i = 0; i < num_pins; i++) {
    uint64_t flags = (pins[i].direction == RPIGPIO_INPUT)
                         ? GPIO_V2_LINE_FLAG_INPUT
                         : GPIO_V2_LINE_FLAG_OUTPUT;
    int val = pins[i].initial_value;
    int *val_p = (pins[i].direction == RPIGPIO_OUTPUT) ? &val : NULL;
    if (claim_line(gpio, consumer, pins[i].pin, flags, val_p)) {
      fprintf(stderr, "%s: failed to claim pin %d: %s\n", path, pins[i].pin,
              strerror(errno));
      rpigpio_close(gpio);
      return NULL;
    }
  }

  return gpio;
}

void rpigpio_close(struct RpiGpio *gpio) {
  if (!gpio) {
    return;
  }

  for (int i = 0; i < MAX_LINES; i++) {
    if (gpio->lines[i].fd >= 0) {
      close(gpio->lines[i].fd);
    }
  }

  close(gpio->chip_fd);
  free(gpio);
}

int rpigpio_read(struct RpiGpio *gpio, int pin) {
  if (pin < 0 || pin >= MAX_LINES || gpio->lines[pin].fd < 0) {
    return -1;
  }

  struct gpio_v2_line_values lv = {.mask = 1};

  if (ioctl(gpio->lines[pin].fd, GPIO_V2_LINE_GET_VALUES_IOCTL, &lv)) {
    perror("GPIO read");
    return -1;
  }

  return lv.bits & 1;
}

int rpigpio_write(struct RpiGpio *gpio, int pin, int value) {
  if (pin < 0 || pin >= MAX_LINES || gpio->lines[pin].fd < 0) {
    return -1;
  }

  struct gpio_v2_line_values lv = {
      .mask = 1,
      .bits = value ? 1 : 0,
  };

  if (ioctl(gpio->lines[pin].fd, GPIO_V2_LINE_SET_VALUES_IOCTL, &lv)) {
    perror("GPIO write");
    return -1;
  }

  return 0;
}

// SPI ------------------------------------------------------------------

struct RpiSpi {
  int fd;
  int speed;
};

struct RpiSpi *rpispi_open(int dev, int channel, int baud) {
  char path[64];
  sprintf(path, "/dev/spidev%d.%d", dev, channel);

  int fd = open(path, O_RDWR);
  if (fd < 0) {
    perror(path);
    return NULL;
  }

  char mode = 0;
  char bits = 8;

  if (ioctl(fd, SPI_IOC_WR_MODE, &mode) < 0 ||
      ioctl(fd, SPI_IOC_WR_BITS_PER_WORD, &bits) < 0 ||
      ioctl(fd, SPI_IOC_WR_MAX_SPEED_HZ, &baud) < 0) {
    perror("SPI ioctl setup");
    close(fd);
    return NULL;
  }

  struct RpiSpi *spi = malloc(sizeof(*spi));
  if (!spi) {
    close(fd);
    return NULL;
  }

  spi->fd = fd;
  spi->speed = baud;
  return spi;
}

void rpispi_close(struct RpiSpi *spi) {
  if (!spi) {
    return;
  }

  close(spi->fd);
  free(spi);
}

int rpispi_write(struct RpiSpi *spi, const uint8_t *buf, int count) {
  struct spi_ioc_transfer xfer = {0};

  xfer.tx_buf = (uintptr_t)buf;
  xfer.len = count;
  xfer.speed_hz = spi->speed;
  xfer.bits_per_word = 8;

  if (ioctl(spi->fd, SPI_IOC_MESSAGE(1), &xfer) < 0) {
    perror("SPI write");
    return -1;
  }

  return count;
}
