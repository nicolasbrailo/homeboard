#pragma once

#include "drm_mgr/drm_mgr.h"
#include "jpeg_render/img_render.h"

#include <stdint.h>
#include <stdbool.h>

struct RenderCtx;
typedef void (*render_pre_commit_cb_t)(void *ud, uint32_t* fb, const struct fb_info* fbi);
struct RenderCtx* render_init(render_pre_commit_cb_t cb, void* render_pre_commit_cb_ud,
        const char* fallback_img_path, uint32_t transition_time_s,
        bool use_eink, const struct img_render_cfg *img_cfg);
void render_free(struct RenderCtx* s);

// Call when the DRM assigns an FB to this service
void render_set_fb(struct RenderCtx* s, uint32_t* fb, const struct fb_info* fbi);

void render_slideshow_next(struct RenderCtx *s);
void render_slideshow_prev(struct RenderCtx *s);
void render_slideshow_set_active(struct RenderCtx *s, bool active);

bool slideshow_set_transition_time_s(struct RenderCtx *s, uint32_t seconds);
void render_set_img_render_config(struct RenderCtx *s, const struct img_render_cfg* cfg);

