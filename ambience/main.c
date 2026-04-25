#include "config.h"
#include "dbus_listeners.h"
#include "eink_meta.h"
#include "overlay.h"
#include "photo_client.h"
#include "render_loop.h"

#include "drm_mgr/drm_mgr.h"
#include "jpeg_render/img_render.h"

#include <errno.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/signalfd.h>
#include <systemd/sd-event.h>

struct ambience_ctx {
  sd_event *event;
  struct DBusListeners *listeners;
  struct PhotoClient *photos;
  struct RenderLoop *render;
  struct Overlay *overlay;
  struct EinkMeta *eink_meta;

  const struct fb_info *fbi;
  bool embed_qr;

  // Cached so we can detect axis-swap on SetRenderConfig and re-push the
  // target size to photo-provider. Initialised from the loaded config and
  // updated on every successful render-config change.
  enum rotation current_rotation;
  uint32_t photo_target_w;
  uint32_t photo_target_h;
};

static void compute_photo_target(const struct fb_info *fbi, enum rotation rot,
                                 uint32_t *w, uint32_t *h) {
  if (rot == ROT_90 || rot == ROT_270) {
    *w = fbi->height;
    *h = fbi->width;
  } else {
    *w = fbi->width;
    *h = fbi->height;
  }
}

// ----- dbus_listeners callbacks -----

static void on_next(void *ud) {
  render_loop_next(((struct ambience_ctx *)ud)->render);
}
static void on_prev(void *ud) {
  render_loop_prev(((struct ambience_ctx *)ud)->render);
}

static bool on_set_transition_time(void *ud, uint32_t seconds) {
  return render_loop_set_transition_time(((struct ambience_ctx *)ud)->render,
                                         seconds);
}

static int on_announce(void *ud, uint32_t timeout_seconds, const char *msg) {
  return overlay_request_announce(((struct ambience_ctx *)ud)->overlay,
                                  timeout_seconds, msg);
}

static int parse_render_config(uint32_t rotation, const char *interp_s,
                               const char *h_align_s, const char *v_align_s,
                               struct img_render_cfg *out) {
  if (rotation != 0 && rotation != 90 && rotation != 180 && rotation != 270) {
    fprintf(stderr, "SetRenderConfig: invalid rotation %u\n", rotation);
    return -EINVAL;
  }
  out->rot = (enum rotation)rotation;

  if (interp_s && strcmp(interp_s, "nearest") == 0)
    out->interp = INTERP_NEAREST;
  else if (interp_s && strcmp(interp_s, "bilinear") == 0)
    out->interp = INTERP_BILINEAR;
  else {
    fprintf(stderr, "SetRenderConfig: invalid interpolation '%s'\n",
            interp_s ? interp_s : "");
    return -EINVAL;
  }

  if (h_align_s && strcmp(h_align_s, "left") == 0)
    out->h_align = HORIZONTAL_ALIGN_LEFT;
  else if (h_align_s && strcmp(h_align_s, "center") == 0)
    out->h_align = HORIZONTAL_ALIGN_CENTER;
  else if (h_align_s && strcmp(h_align_s, "right") == 0)
    out->h_align = HORIZONTAL_ALIGN_RIGHT;
  else {
    fprintf(stderr, "SetRenderConfig: invalid horizontal_align '%s'\n",
            h_align_s ? h_align_s : "");
    return -EINVAL;
  }

  if (v_align_s && strcmp(v_align_s, "top") == 0)
    out->v_align = VERTICAL_ALIGN_TOP;
  else if (v_align_s && strcmp(v_align_s, "center") == 0)
    out->v_align = VERTICAL_ALIGN_CENTER;
  else if (v_align_s && strcmp(v_align_s, "bottom") == 0)
    out->v_align = VERTICAL_ALIGN_BOTTOM;
  else {
    fprintf(stderr, "SetRenderConfig: invalid vertical_align '%s'\n",
            v_align_s ? v_align_s : "");
    return -EINVAL;
  }

  return 0;
}

static int on_set_render_config(void *ud, uint32_t rotation,
                                const char *interp_s, const char *h_align_s,
                                const char *v_align_s) {
  struct ambience_ctx *ctx = ud;
  struct img_render_cfg cfg;
  int r = parse_render_config(rotation, interp_s, h_align_s, v_align_s, &cfg);
  if (r < 0)
    return r;

  // photo-provider pre-scales to the configured target size. When rotation
  // toggles between portrait/landscape the axes swap, so we re-push the
  // target size before applying the new render config locally — that way
  // the next photo arrives with the correct aspect ratio.
  const bool axes_before =
      (ctx->current_rotation == ROT_90 || ctx->current_rotation == ROT_270);
  const bool axes_after = (cfg.rot == ROT_90 || cfg.rot == ROT_270);
  if (axes_before != axes_after) {
    compute_photo_target(ctx->fbi, cfg.rot, &ctx->photo_target_w,
                         &ctx->photo_target_h);
    if (photo_client_set_target_size(ctx->photos, ctx->photo_target_w,
                                     ctx->photo_target_h) < 0)
      fprintf(stderr,
              "WARNING: Failed to push new target size to photo-provider\n");
  }

  if (!render_loop_set_render_cfg(ctx->render, &cfg))
    return -EINVAL;
  ctx->current_rotation = cfg.rot;
  return 0;
}

static void apply_presence(struct ambience_ctx *ctx, bool present) {
  if (present) {
    render_loop_start(ctx->render);
    dbus_listeners_emit_slideshow_active(ctx->listeners, true);
  } else {
    render_loop_stop(ctx->render);
    if (ctx->eink_meta)
      eink_meta_clear(ctx->eink_meta);
    dbus_listeners_emit_slideshow_active(ctx->listeners, false);
  }
}

static void on_presence_changed(void *ud, bool present) {
  apply_presence((struct ambience_ctx *)ud, present);
}

// presence-service is the sole owner of Display.On/Off; if it disappears we
// can't trust display state and have nothing driving us, so just stop the
// slideshow.
static void on_presence_service_updown(void *ud, bool up) {
  if (up)
    return;
  fprintf(stderr, "WARNING: Presence service is down, stopping slideshow\n");
  apply_presence((struct ambience_ctx *)ud, false);
}

static void on_photo_service_updown(void *ud, bool up) {
  struct ambience_ctx *ctx = ud;
  if (!up) {
    fprintf(stderr, "WARNING: Photo service is down, slideshow will freeze on "
                    "current picture\n");
    return;
  }
  if (photo_client_push_config(ctx->photos, ctx->photo_target_w,
                               ctx->photo_target_h, ctx->embed_qr) < 0)
    fprintf(stderr, "WARNING: Failed to (re)configure photo-provider after it "
                    "came back up\n");
}

// ----- render_loop callbacks -----

static void on_photo_rendered(void *ud, const char *meta) {
  struct ambience_ctx *ctx = ud;
  if (ctx->eink_meta)
    eink_meta_render(ctx->eink_meta, meta);
  dbus_listeners_emit_displaying_photo(ctx->listeners, meta);
}

// ----- signal handler -----

static int on_signal(sd_event_source *s, const struct signalfd_siginfo *si,
                     void *ud) {
  (void)ud;
  printf("Received signal %d, shutting down\n", si->ssi_signo);
  return sd_event_exit(sd_event_source_get_event(s), 0);
}

// ----- main -----

int main(int argc, char *argv[]) {
  int ret = 0;
  sd_event *event = NULL;
  struct DRM_Mgr *drm_mgr = NULL;
  uint32_t *fb = NULL;
  struct fb_info fbi = {0};
  struct ambience_config cfg = {0};
  struct ambience_ctx ctx = {0};

  const char *config_path = (argc >= 2) ? argv[1] : "config.json";
  if (ambience_config_load(config_path, &cfg) != 0) {
    fprintf(stderr, "Failed to load config: %s\n", config_path);
    return 1;
  }

  drm_mgr = drm_mgr_init();
  if (!drm_mgr) {
    fprintf(stderr, "drm_mgr_init failed\n");
    ret = 1;
    goto end;
  }
  fb = drm_mgr_acquire_fb(drm_mgr, &fbi);
  if (!fb) {
    fprintf(stderr, "drm_mgr_acquire_fb failed\n");
    ret = 1;
    goto end;
  }

  int r = sd_event_default(&event);
  if (r < 0) {
    fprintf(stderr, "sd_event_default: %s\n", strerror(-r));
    ret = 1;
    goto end;
  }

  // Block SIGINT/SIGTERM so sd_event can deliver them via signalfd.
  sigset_t mask;
  sigemptyset(&mask);
  sigaddset(&mask, SIGINT);
  sigaddset(&mask, SIGTERM);
  sigprocmask(SIG_BLOCK, &mask, NULL);
  sd_event_add_signal(event, NULL, SIGINT, on_signal, NULL);
  sd_event_add_signal(event, NULL, SIGTERM, on_signal, NULL);

  ctx.event = event;
  ctx.fbi = &fbi;
  ctx.embed_qr = cfg.embed_qr;
  ctx.current_rotation = cfg.render.rot;
  compute_photo_target(&fbi, cfg.render.rot, &ctx.photo_target_w,
                       &ctx.photo_target_h);

  ctx.overlay = overlay_init();
  if (!ctx.overlay) {
    fprintf(stderr, "overlay_init failed\n");
    ret = 1;
    goto end;
  }

  if (cfg.use_eink_for_metadata) {
    ctx.eink_meta = eink_meta_init();
    if (!ctx.eink_meta)
      fprintf(stderr, "WARNING: eink metadata display unavailable\n");
  }

  ctx.photos = photo_client_init(event);
  if (!ctx.photos) {
    fprintf(stderr, "photo_client_init failed\n");
    ret = 1;
    goto end;
  }
  // Initial config push. If photo-provider isn't up yet we'll retry on its
  // service-up event (wired through dbus_listeners).
  if (photo_client_push_config(ctx.photos, ctx.photo_target_w,
                               ctx.photo_target_h, ctx.embed_qr) < 0)
    fprintf(stderr,
            "WARNING: Failed to push initial config to photo-provider\n");

  ctx.render = render_loop_init(event, fb, &fbi, cfg.transition_time_s,
                                &cfg.render, cfg.fallback_image, ctx.photos,
                                on_photo_rendered, &ctx, overlay_draw,
                                ctx.overlay);
  if (!ctx.render) {
    fprintf(stderr, "render_loop_init failed\n");
    ret = 1;
    goto end;
  }

  const struct dbus_listeners_cbs cbs = {
      .on_next = on_next,
      .on_prev = on_prev,
      .on_set_transition_time = on_set_transition_time,
      .on_set_render_config = on_set_render_config,
      .on_announce = on_announce,
      .on_presence_changed = on_presence_changed,
      .on_presence_service_updown = on_presence_service_updown,
      .on_photo_service_updown = on_photo_service_updown,
  };
  ctx.listeners = dbus_listeners_init(event, &cbs, &ctx);
  if (!ctx.listeners) {
    fprintf(stderr, "dbus_listeners_init failed\n");
    ret = 1;
    goto end;
  }

  r = sd_event_loop(event);
  if (r < 0) {
    fprintf(stderr, "sd_event_loop: %s\n", strerror(-r));
    ret = 1;
  }

end:
  dbus_listeners_free(ctx.listeners);
  render_loop_free(ctx.render);
  photo_client_free(ctx.photos);
  eink_meta_free(ctx.eink_meta);
  overlay_free(ctx.overlay);
  if (event)
    sd_event_unref(event);
  if (drm_mgr) {
    drm_mgr_release_fb(drm_mgr);
    drm_mgr_free(drm_mgr);
  }
  return ret;
}
