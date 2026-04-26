#pragma once

#include <stdint.h>

struct fb_info {
  uint32_t width;
  uint32_t height;
  uint32_t stride;
  uint32_t format;
  uint32_t size;
};

struct DRM_Mgr;

struct DRM_Mgr *drm_mgr_init();

struct DRM_Mgr *drm_mgr_init_acquire_fb(uint32_t**, struct fb_info *info);

// Automatically calls release_fb if DRM is held
void drm_mgr_free(struct DRM_Mgr *self);

// Acquire the framebuffer from the daemon. On success, returns mmap'd pixels
// and fills *info. Returns NULL on failure.
uint32_t *drm_mgr_acquire_fb(struct DRM_Mgr *self, struct fb_info *info);

// Release the framebuffer, unmapping memory and notifying the daemon.
void drm_mgr_release_fb(struct DRM_Mgr *self);
