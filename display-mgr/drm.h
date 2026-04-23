#pragma once

#include <stdbool.h>
#include <stdint.h>

struct fb_info {
  uint32_t width;
  uint32_t height;
  uint32_t stride;
  uint32_t format;
  uint32_t size;
};

struct DRM_state;

struct DRM_state *drm_init();
void drm_free(struct DRM_state *s);

void drm_display_off(struct DRM_state *s);
void drm_display_on(struct DRM_state *s);
bool drm_is_display_on(const struct DRM_state *s);

int drm_create_framebuffer(struct DRM_state *s);

const struct fb_info *drm_get_fb_info(const struct DRM_state *s);
int drm_get_dmabuf_fd(const struct DRM_state *s);
size_t drm_get_num_outputs(const struct DRM_state *s);
