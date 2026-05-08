#include "config.h"
#include "dbus_client.h"
#include "mqtt.h"

#include <jpeg_render/img_render.h>

#include <errno.h>
#include <json-c/json.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <systemd/sd-bus.h>
#include <time.h>
#include <unistd.h>

// Hard cap on a single inbound MQTT command payload. The biggest legitimate
// command is set_svg_overlay; SVG content is capped at 128 KB and JSON
// envelope/escaping fits comfortably below this.
#define CMD_PAYLOAD_HARD_CAP ((1024 * 1024) + (64 * 1024))
#define SVG_CONTENT_CAP (1024 * 1024)

static volatile sig_atomic_t g_quit;

static void sig_handler(int sig) {
  (void)sig;
  g_quit = 1;
}

struct app_ctx {
  struct rc_dbus *dbus;
  struct rc_mqtt *mqtt;
  struct img_render_cfg last_render_cfg;
  bool have_last_render_cfg;
  uint32_t display_w_px;
  uint32_t display_h_px;
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

static void on_displayed_photo_changed(const char *meta,
                                       const struct img_render_cfg *cfg,
                                       uint32_t display_w_px,
                                       uint32_t display_h_px, void *ud) {
  struct app_ctx *ctx = ud;
  rc_mqtt_publish(ctx->mqtt, "state/displayed_photo", meta, strlen(meta), true);
  if (!ctx->have_last_render_cfg || (display_w_px != ctx->display_w_px) ||
      (display_h_px != ctx->display_h_px) ||
      memcmp(&ctx->last_render_cfg, cfg, sizeof(*cfg)) != 0) {
    ctx->last_render_cfg = *cfg;
    ctx->have_last_render_cfg = true;
    ctx->display_w_px = display_w_px;
    ctx->display_h_px = display_h_px;
    rc_mqtt_set_render_cfg(ctx->mqtt, cfg, display_w_px, display_h_px);
  }
}

static void on_slideshow_active_changed(bool active, void *ud) {
  struct app_ctx *ctx = ud;
  const char *payload = active ? "{\"active\":true}" : "{\"active\":false}";
  rc_mqtt_publish(ctx->mqtt, "state/slideshow_active", payload, strlen(payload),
                  true);
}

// Parses payload as a JSON object. Returns NULL and logs on failure (not an
// object, malformed JSON, or empty). Caller owns the returned object and must
// json_object_put() it.
static struct json_object *parse_obj(const char *suffix, const char *payload,
                                     size_t len) {
  if (len == 0) {
    fprintf(stderr, "%s: empty payload\n", suffix);
    return NULL;
  }
  struct json_tokener *tok = json_tokener_new();
  if (!tok)
    return NULL;
  struct json_object *obj = json_tokener_parse_ex(tok, payload, (int)len);
  enum json_tokener_error err = json_tokener_get_error(tok);
  json_tokener_free(tok);
  if (!obj || err != json_tokener_success) {
    fprintf(stderr, "%s: invalid JSON: %s\n", suffix,
            json_tokener_error_desc(err));
    if (obj)
      json_object_put(obj);
    return NULL;
  }
  if (!json_object_is_type(obj, json_type_object)) {
    fprintf(stderr, "%s: payload is not a JSON object\n", suffix);
    json_object_put(obj);
    return NULL;
  }
  return obj;
}

static int get_u32(struct json_object *o, const char *key, uint32_t *out) {
  struct json_object *v;
  if (!json_object_object_get_ex(o, key, &v))
    return -1;
  if (!json_object_is_type(v, json_type_int))
    return -1;
  int64_t n = json_object_get_int64(v);
  if (n < 0 || n > UINT32_MAX)
    return -1;
  *out = (uint32_t)n;
  return 0;
}

static int get_string(struct json_object *o, const char *key,
                      const char **out) {
  struct json_object *v;
  if (!json_object_object_get_ex(o, key, &v))
    return -1;
  if (!json_object_is_type(v, json_type_string))
    return -1;
  *out = json_object_get_string(v);
  return 0;
}

static int get_bool(struct json_object *o, const char *key, bool *out) {
  struct json_object *v;
  if (!json_object_object_get_ex(o, key, &v))
    return -1;
  if (!json_object_is_type(v, json_type_boolean))
    return -1;
  *out = json_object_get_boolean(v) ? true : false;
  return 0;
}

static void cmd_set_transition_time(struct app_ctx *ctx, const char *suffix,
                                    struct json_object *o) {
  uint32_t secs;
  if (get_u32(o, "secs", &secs) < 0) {
    fprintf(stderr, "%s: missing/invalid 'secs'\n", suffix);
    return;
  }
  rc_dbus_ambience_set_transition_time(ctx->dbus, secs);
}

static void cmd_set_svg_overlay(struct app_ctx *ctx, const char *suffix,
                                struct json_object *o) {
  uint32_t timeout;
  const char *svg;
  if (get_u32(o, "timeout", &timeout) < 0 || get_string(o, "svg", &svg) < 0) {
    fprintf(stderr, "%s: missing/invalid 'timeout' or 'svg'\n", suffix);
    return;
  }
  if (strlen(svg) > SVG_CONTENT_CAP) {
    fprintf(stderr, "%s: svg exceeds %d bytes\n", suffix, SVG_CONTENT_CAP);
    return;
  }
  rc_dbus_ambience_set_svg_overlay(ctx->dbus, timeout, svg);
}

static void cmd_set_render_config(struct app_ctx *ctx, const char *suffix,
                                  struct json_object *o) {
  uint32_t rot;
  const char *interp, *h_align, *v_align;
  if (get_u32(o, "rotation", &rot) < 0 ||
      get_string(o, "interp", &interp) < 0 ||
      get_string(o, "h_align", &h_align) < 0 ||
      get_string(o, "v_align", &v_align) < 0) {
    fprintf(stderr,
            "%s: need {rotation:uint, interp:str, h_align:str, v_align:str}\n",
            suffix);
    return;
  }
  rc_dbus_ambience_set_render_config(ctx->dbus, rot, interp, h_align, v_align);
}

static void cmd_set_embed_qr(struct app_ctx *ctx, const char *suffix,
                             struct json_object *o) {
  bool on;
  if (get_bool(o, "on", &on) < 0) {
    fprintf(stderr, "%s: missing/invalid 'on'\n", suffix);
    return;
  }
  rc_dbus_photo_set_embed_qr(ctx->dbus, on);
}

static void cmd_set_target_size(struct app_ctx *ctx, const char *suffix,
                                struct json_object *o) {
  uint32_t w, h;
  if (get_u32(o, "w", &w) < 0 || get_u32(o, "h", &h) < 0) {
    fprintf(stderr, "%s: need {w:uint, h:uint}\n", suffix);
    return;
  }
  if (w == 0 || h == 0 || w > 10000 || h > 10000) {
    fprintf(stderr, "%s: dimensions out of range (1..10000)\n", suffix);
    return;
  }
  rc_dbus_photo_set_target_size(ctx->dbus, w, h);
}

static void on_cmd(const char *suffix, const char *payload, size_t len,
                   void *ud) {
  struct app_ctx *ctx = ud;
  printf("cmd: %s (%zu bytes)\n", suffix, len);

  // Void commands ignore payload entirely — no JSON parse needed.
  if (strcmp(suffix, "ambience/next") == 0) {
    rc_dbus_ambience_call_void(ctx->dbus, "Next");
    return;
  } else if (strcmp(suffix, "ambience/prev") == 0) {
    rc_dbus_ambience_call_void(ctx->dbus, "Prev");
    return;
  } else if (strcmp(suffix, "presence/force_on") == 0) {
    rc_dbus_presence_call_void(ctx->dbus, "ForceOn");
    return;
  } else if (strcmp(suffix, "presence/force_off") == 0) {
    rc_dbus_presence_call_void(ctx->dbus, "ForceOff");
    return;
  }

  if (len > CMD_PAYLOAD_HARD_CAP) {
    fprintf(stderr, "%s: payload too large (%zu bytes, cap %d)\n", suffix, len,
            CMD_PAYLOAD_HARD_CAP);
    return;
  }

  struct json_object *o = parse_obj(suffix, payload, len);
  if (!o)
    return;

  if (strcmp(suffix, "ambience/set_transition_time_secs") == 0)
    cmd_set_transition_time(ctx, suffix, o);
  else if (strcmp(suffix, "ambience/set_svg_overlay") == 0)
    cmd_set_svg_overlay(ctx, suffix, o);
  else if (strcmp(suffix, "ambience/set_render_config") == 0)
    cmd_set_render_config(ctx, suffix, o);
  else if (strcmp(suffix, "photo_provider/set_embed_qr") == 0)
    cmd_set_embed_qr(ctx, suffix, o);
  else if (strcmp(suffix, "photo_provider/set_target_size") == 0)
    cmd_set_target_size(ctx, suffix, o);
  else
    fprintf(stderr, "Unknown cmd: %s\n", suffix);

  json_object_put(o);
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
  ctx.mqtt = rc_mqtt_init(&cfg, on_cmd, &ctx);
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
