#include "ld2410s.h"
#include "rpigpio/rpigpio.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#define VACANT_REPORTS_ROOM_EMPTY_THRESHOLD 30

struct LD2410S {
  struct LD2410S_uart *uart;
  struct RpiGpio *gpio;
  int gpio_pin;

  atomic_bool uart_occupied;
  atomic_uint uart_distance;

  unsigned startup_delay_secs;
  unsigned hysteresis_occupied;
  unsigned hysteresis_vacant;

  ld2410s_state_change_cb state_cb;
  void *state_cb_ud;

  pthread_t poller;
  atomic_bool stop;
  bool poller_running;
};

static void on_uart_report(const struct LD2410S_report *r, void *ud) {
  struct LD2410S *s = ud;
  atomic_store_explicit(&s->uart_occupied, r->occupied, memory_order_relaxed);
  atomic_store_explicit(&s->uart_distance, r->distance, memory_order_relaxed);
}

static void on_uart_calibration_progress(uint16_t progress, void *ud) {
  (void)ud;
  printf("Calibration progress: %u%%\n", progress);
}

static void *poller_thread(void *arg) {
  struct LD2410S *s = arg;
  bool last_emitted_occupied = false;
  uint16_t last_emitted_distance = 0;
  bool emitted_first = false;
  bool pending_state = false;
  unsigned pending_count = 0;
  unsigned ticks = 0;

  while (!atomic_load_explicit(&s->stop, memory_order_relaxed)) {
    bool gpio_occ = false;
    if (s->gpio)
      gpio_occ = rpigpio_read(s->gpio, s->gpio_pin);

    bool uart_occ = atomic_load_explicit(&s->uart_occupied, memory_order_relaxed);
    uint16_t distance = (uint16_t)atomic_load_explicit(&s->uart_distance, memory_order_relaxed);
    bool occupied = gpio_occ || uart_occ;

    if (!emitted_first) {
      if (ticks >= s->startup_delay_secs) {
        emitted_first = true;
        last_emitted_occupied = occupied;
        last_emitted_distance = distance;
        pending_state = occupied;
        pending_count = 1;
        s->state_cb(occupied, distance, s->state_cb_ud);
      }
    } else {
      if (occupied == pending_state) {
        pending_count++;
      } else {
        pending_state = occupied;
        pending_count = 1;
      }

      unsigned threshold = occupied ? s->hysteresis_occupied : s->hysteresis_vacant;
      if (threshold == 0)
        threshold = 1;

      if (occupied != last_emitted_occupied && pending_count >= threshold) {
        // There's been an occupancy state change, announce
        last_emitted_occupied = occupied;
        last_emitted_distance = distance;
        s->state_cb(occupied, distance, s->state_cb_ud);
      } else if (occupied == last_emitted_occupied && distance != last_emitted_distance) {
        // There's been no occupancy state change, but there is a distance change. Only announce
        // if we are in a stable state, ie not waiting for the hysteresis period
        last_emitted_distance = distance;
        s->state_cb(occupied, distance, s->state_cb_ud);
      }
    }

    ticks++;
    sleep(1);
  }
  return NULL;
}

struct LD2410S *ld2410s_init(const struct LD2410S_config *cfg, ld2410s_state_change_cb cb, void *user_data) {
  if (cfg->sensor_report_gpio < 0 && !cfg->enable_uart) {
    fprintf(stderr, "GPIO and UART interfaces disabled, nothing to monitor\n");
    return NULL;
  }

  struct LD2410S *s = calloc(1, sizeof(*s));
  if (!s)
    return NULL;
  if (!cb) {
    fprintf(stderr, "No occupancy callback, nothing to do\n");
    return NULL;
  }

  s->gpio_pin = cfg->sensor_report_gpio;
  s->startup_delay_secs = cfg->startup_delay_secs;
  s->hysteresis_occupied = cfg->hysteresis_occupied ? cfg->hysteresis_occupied : 1;
  s->hysteresis_vacant = cfg->hysteresis_vacant ? cfg->hysteresis_vacant : 1;
  s->state_cb = cb;
  s->state_cb_ud = user_data;
  atomic_init(&s->uart_occupied, false);
  atomic_init(&s->uart_distance, 0);
  atomic_init(&s->stop, false);

  if (cfg->sensor_report_gpio >= 0) {
    const struct RpiGpioPin pins[] = {
        {cfg->sensor_report_gpio, RPIGPIO_INPUT, 0},
    };
    s->gpio = rpigpio_open(0, "ld2410s_occupancy", pins, 1);
    if (!s->gpio) {
      fprintf(stderr, "Failed to enable GPIO pin %d monitoring.\n", cfg->sensor_report_gpio);
      ld2410s_free(s);
      return NULL;
    }
    printf("Enabled GPIO pin %d monitoring.\n", cfg->sensor_report_gpio);
  }

  if (cfg->enable_uart) {
    s->uart = ld2410s_uart_init(cfg->device, cfg->debug, on_uart_report, s, on_uart_calibration_progress, s);
    if (!s->uart || ld2410s_uart_start(s->uart) < 0) {
      ld2410s_free(s);
      return NULL;
    }
    printf("Enabled UART monitoring.\n");
  }

  if (pthread_create(&s->poller, NULL, poller_thread, s) != 0) {
    fprintf(stderr, "Failed to spawn UART poller thread\n");
    ld2410s_free(s);
    return NULL;
  }
  s->poller_running = true;
  return s;
}

void ld2410s_free(struct LD2410S *s) {
  if (!s)
    return;
  if (s->poller_running) {
    atomic_store_explicit(&s->stop, true, memory_order_relaxed);
    pthread_join(s->poller, NULL);
  }
  ld2410s_uart_free(s->uart);
  rpigpio_close(s->gpio);
  free(s);
}

int ld2410s_get_common_params(struct LD2410S *s, struct LD2410S_common_params *out) {
  if (!s || !s->uart)
    return -1;
  return ld2410s_uart_get_common_params(s->uart, out);
}

int ld2410s_set_param(struct LD2410S *s, const char *name, uint32_t value) {
  if (!s || !s->uart)
    return -1;
  return ld2410s_uart_set_param(s->uart, name, value);
}

int ld2410s_start_calibration(struct LD2410S *s, uint16_t trigger, uint16_t retention, uint16_t duration_secs) {
  if (!s || !s->uart) {
    // Return a dummy OK - we don't have UART, so calibration requests get signaled as OK
    return 0;
  }

  bool room_empty = ld2410s_uart_get_vacant_reports_count(s->uart) >= VACANT_REPORTS_ROOM_EMPTY_THRESHOLD;
  if (!room_empty) {
    printf("Skip calibration request, room isn't empty\n");
    return -1;
  }

  printf("Starting calibration (trigger=%u retention=%u duration=%us)\n", trigger, retention, duration_secs);
  return ld2410s_uart_start_calibration(s->uart, trigger, retention, duration_secs);
}
