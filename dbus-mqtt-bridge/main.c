#include "config.h"
#include "dbus_client.h"
#include "mqtt.h"

#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <systemd/sd-bus.h>
#include <time.h>
#include <unistd.h>

static volatile sig_atomic_t g_quit;

static void sig_handler(int sig) {
  (void)sig;
  g_quit = 1;
}

struct app_ctx {
  struct rc_dbus *dbus;
  struct rc_mqtt *mqtt;
};

static void on_occupancy(bool occupied, uint32_t distance, void *ud) {
  struct app_ctx *ctx = ud;
  char payload[128];
  int n =
      snprintf(payload, sizeof(payload),
               "{\"occupied\":%s,\"distance_cm\":%u,\"ts\":%lld}",
               occupied ? "true" : "false", distance, (long long)time(NULL));
  if (n > 0 && (size_t)n < sizeof(payload))
    rc_mqtt_publish(ctx->mqtt, "state/occupancy", payload, (size_t)n, true);
}

static void on_displayed_photo_changed(const char *meta, void *ud) {
  struct app_ctx *ctx = ud;
  rc_mqtt_publish(ctx->mqtt, "state/displayed_photo", meta, strlen(meta), true);
}

static void on_slideshow_active_changed(bool active, void *ud) {
  struct app_ctx *ctx = ud;
  const char *payload = active ? "true" : "false";
  rc_mqtt_publish(ctx->mqtt, "state/slideshow_active", payload, strlen(payload),
                  true);
}

static void on_active_server(const char *url, const char *qr_img, void *ud) {
  struct app_ctx *ctx = ud;
  printf("active_server: %s\n", url);
  rc_dbus_ambience_set_remote_control_server(ctx->dbus, url, qr_img);
}

static int parse_bool(const char *p, size_t n, bool *out) {
  if (n == 1 && (p[0] == '0' || p[0] == '1')) {
    *out = (p[0] == '1');
    return 0;
  }
  if (n == 4 && strncasecmp(p, "true", 4) == 0) {
    *out = true;
    return 0;
  }
  if (n == 5 && strncasecmp(p, "false", 5) == 0) {
    *out = false;
    return 0;
  }
  return -1;
}

static int parse_u32(const char *p, size_t n, uint32_t *out) {
  if (n == 0 || n > 10)
    return -1;
  char buf[11];
  memcpy(buf, p, n);
  buf[n] = '\0';
  char *end;
  unsigned long v = strtoul(buf, &end, 10);
  if (*end != '\0' || v > UINT32_MAX)
    return -1;
  *out = (uint32_t)v;
  return 0;
}

static int parse_size(const char *p, size_t n, uint32_t *w, uint32_t *h) {
  if (n == 0 || n > 20)
    return -1;
  char buf[21];
  memcpy(buf, p, n);
  buf[n] = '\0';
  char *x = strchr(buf, 'x');
  if (!x)
    return -1;
  *x = '\0';
  char *e1, *e2;
  unsigned long ww = strtoul(buf, &e1, 10);
  unsigned long hh = strtoul(x + 1, &e2, 10);
  if (*e1 != '\0' || *e2 != '\0')
    return -1;
  if (ww == 0 || hh == 0 || ww > 10000 || hh > 10000)
    return -1;
  *w = (uint32_t)ww;
  *h = (uint32_t)hh;
  return 0;
}

static void on_cmd(const char *suffix, const char *payload, size_t len,
                   void *ud) {
  struct app_ctx *ctx = ud;
  printf("cmd: %s (%zu bytes)\n", suffix, len);

  if (strcmp(suffix, "ambience/next") == 0) {
    rc_dbus_ambience_call_void(ctx->dbus, "Next");
  } else if (strcmp(suffix, "ambience/prev") == 0) {
    rc_dbus_ambience_call_void(ctx->dbus, "Prev");
  } else if (strcmp(suffix, "presence/force_on") == 0) {
    rc_dbus_presence_call_void(ctx->dbus, "ForceOn");
  } else if (strcmp(suffix, "presence/force_off") == 0) {
    rc_dbus_presence_call_void(ctx->dbus, "ForceOff");
  } else if (strcmp(suffix, "ambience/set_transition_time_secs") == 0) {
    uint32_t s;
    if (parse_u32(payload, len, &s) == 0)
      rc_dbus_ambience_set_transition_time(ctx->dbus, s);
    else
      fprintf(stderr, "set_transition_time_secs: invalid payload\n");
  } else if (strcmp(suffix, "photo_provider/set_embed_qr") == 0) {
    bool b;
    if (parse_bool(payload, len, &b) == 0)
      rc_dbus_photo_set_embed_qr(ctx->dbus, b);
    else
      fprintf(stderr, "set_embed_qr: invalid payload\n");
  } else if (strcmp(suffix, "photo_provider/set_target_size") == 0) {
    uint32_t w, h;
    if (parse_size(payload, len, &w, &h) == 0)
      rc_dbus_photo_set_target_size(ctx->dbus, w, h);
    else
      fprintf(stderr, "set_target_size: invalid payload\n");
  } else {
    fprintf(stderr, "Unknown cmd: %s\n", suffix);
  }
}

int main(int argc, char *argv[]) {
  if (argc < 2) {
    fprintf(stderr, "Usage: %s <config.json>\n", argv[0]);
    return 1;
  }

  struct rc_config cfg;
  if (rc_config_load(argv[1], &cfg) < 0)
    return 1;

  signal(SIGTERM, sig_handler);
  signal(SIGINT, sig_handler);

  struct app_ctx ctx = {0};
  ctx.dbus = rc_dbus_init(on_occupancy, on_displayed_photo_changed,
                          on_slideshow_active_changed, &ctx);
  if (!ctx.dbus)
    return 1;
  ctx.mqtt = rc_mqtt_init(&cfg, on_cmd, on_active_server, &ctx);
  if (!ctx.mqtt) {
    rc_dbus_free(ctx.dbus);
    return 1;
  }

  printf("Bridge running\n");

  while (!g_quit) {
    struct pollfd fds[2];
    int nfds = 0;

    int dbus_fd = sd_bus_get_fd(rc_dbus_bus(ctx.dbus));
    if (dbus_fd >= 0) {
      fds[nfds].fd = dbus_fd;
      fds[nfds].events = POLLIN;
      fds[nfds].revents = 0;
      nfds++;
    }

    int dbus_idx = nfds - 1;
    int mqtt_idx = -1;
    int mqtt_fd = rc_mqtt_socket(ctx.mqtt);
    if (mqtt_fd >= 0) {
      fds[nfds].fd = mqtt_fd;
      fds[nfds].events = POLLIN | (rc_mqtt_want_write(ctx.mqtt) ? POLLOUT : 0);
      fds[nfds].revents = 0;
      mqtt_idx = nfds;
      nfds++;
    }

    int r = poll(fds, nfds, 1000);
    if (r < 0) {
      if (errno == EINTR)
        continue;
      fprintf(stderr, "poll: %s\n", strerror(errno));
      break;
    }

    if (dbus_idx >= 0 &&
        (fds[dbus_idx].revents & (POLLIN | POLLHUP | POLLERR))) {
      while (sd_bus_process(rc_dbus_bus(ctx.dbus), NULL) > 0)
        ;
    }

    if (mqtt_idx >= 0) {
      if (fds[mqtt_idx].revents & POLLIN)
        rc_mqtt_loop_read(ctx.mqtt);
      if (fds[mqtt_idx].revents & POLLOUT)
        rc_mqtt_loop_write(ctx.mqtt);
    }
    rc_mqtt_loop_misc(ctx.mqtt);
  }

  printf("Shutting down\n");
  rc_mqtt_free(ctx.mqtt);
  rc_dbus_free(ctx.dbus);
  return 0;
}
