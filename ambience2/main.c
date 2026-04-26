#include "config.h"
#include "dbus_listeners.h"
#include "drm_mgr/drm_mgr.h"
#include "render.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static volatile sig_atomic_t g_quit;

static void sig_handler(int sig) {
  (void)sig;
  g_quit = 1;
}

struct AmbienceCtx {
  // DRM
  struct DRM_Mgr *drm_mgr;
  uint32_t* fb;
  struct fb_info fbi;

  // Render service
  struct RenderCtx* render;
} g_ambience_ctx;

void on_next(void *ud) {
  struct AmbienceCtx* s = ud;
  render_slideshow_next(s->render);
}

void on_prev(void *ud) {
  struct AmbienceCtx* s = ud;
  render_slideshow_prev(s->render);
}

bool on_set_transition_time(void *ud, uint32_t seconds) {
  struct AmbienceCtx* s = ud;
  return slideshow_set_transition_time_s(s->render, seconds);
}

int on_set_render_config(void *ud, const struct img_render_cfg* cfg) {
  struct AmbienceCtx* s = ud;
  render_set_img_render_config(s->render, cfg);
  return 0;
}
int on_announce(void *ud, uint32_t timeout_seconds, const char *msg) {
  // TODO
  printf("Announcement requested [%d seconds]: '%s'\n", timeout_seconds, msg);
  return 0;
}
void on_presence_changed(void *ud, bool present) {
  struct AmbienceCtx* s = ud;
  render_slideshow_set_active(s->render, present);
}
void on_set_remote_control_server(void *ud, const char *url, const char *qr_img) {
  // TODO
  printf("New remote control available @ %s\n", url);
}
void on_presence_service_updown(void *ud, bool up) {
  struct AmbienceCtx* s = ud;
  if (!up) {
    render_slideshow_set_active(s->render, false);
  }
}
void on_photo_service_updown(void *ud, bool up) {
  struct AmbienceCtx* s = ud;
  render_slideshow_set_active(s->render, up);
}

void on_drm_mgr_updown(void *ud, bool up) {
  struct AmbienceCtx *ctx = ud;
  // This doesn't need a drm mutex because the mutex is held in the render object
  if (up) {
    if (ctx->drm_mgr) {
      fprintf(stderr, "drm_mgr service reported up, but we were already connected. Will renew connection.\n");
      drm_mgr_free(ctx->drm_mgr);
      ctx->drm_mgr = NULL;
      ctx->fb = NULL;
      memset(&ctx->fbi, 0, sizeof(ctx->fbi));
    }

    ctx->drm_mgr = drm_mgr_init_acquire_fb(&ctx->fb, &ctx->fbi);
    if (!ctx->drm_mgr)
      fprintf(stderr, "drm_mgr service reported up, but dbus connection failed\n");
  } else {
    fprintf(stderr, "drm_mgr went away, display won't work\n");
    drm_mgr_free(ctx->drm_mgr);
    ctx->drm_mgr = NULL;
    ctx->fb = NULL;
    memset(&ctx->fbi, 0, sizeof(ctx->fbi));
  }

  // Notify then render thread it has a new fb (or that the old fb went away)
  render_set_fb(ctx->render, ctx->fb, &ctx->fbi);
}

static const struct dbus_listeners_cbs cbs = {
    .on_next = on_next,
    .on_prev = on_prev,
    .on_set_transition_time = on_set_transition_time,
    .on_set_render_config = on_set_render_config,
    .on_announce = on_announce,
    .on_presence_changed = on_presence_changed,
    .on_set_remote_control_server = on_set_remote_control_server,
    .on_presence_service_updown = on_presence_service_updown,
    .on_photo_service_updown = on_photo_service_updown,
    .on_drm_mgr_updown = on_drm_mgr_updown,
};

int main(int argc, char *argv[]) {
  if (argc <= 1) {
    fprintf(stderr, "Usage: %s config.json\n", argv[0]);
    return 1;
  }

  struct ambience_config cfg = {0};
  if (ambience_config_load(argv[1], &cfg) != 0) {
    fprintf(stderr, "Failed to load config\n");
    return 1;
  }

  g_ambience_ctx.render = render_init(cfg.fallback_image, cfg.transition_time_s, cfg.use_eink_for_metadata, &cfg.render);
  if (!g_ambience_ctx.render) {
    fprintf(stderr, "Failed to start display render service\n");
    return 1;
  }

  bool all_deps_ready = false;
  struct DBusListeners *listeners = dbus_listeners_init(&cbs, &g_ambience_ctx, &all_deps_ready);
  render_slideshow_set_active(g_ambience_ctx.render, all_deps_ready);

  // Simulate service up callback to connect to DRM. If it fails, we'll connect later
  on_drm_mgr_updown(&g_ambience_ctx, true);

  signal(SIGTERM, sig_handler);
  signal(SIGINT, sig_handler);
  printf("Ambience running\n");
  while (!g_quit) {
    if (dbus_listeners_run_once(listeners, 1000) < 0)
      break;
  }

  dbus_listeners_free(listeners);
  render_free(g_ambience_ctx.render);
  drm_mgr_free(g_ambience_ctx.drm_mgr);
  return 0;
}
