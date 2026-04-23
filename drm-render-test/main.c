#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <unistd.h>

#include "drm_mgr.h"

static volatile sig_atomic_t g_quit;

static void sig_handler(int sig) {
  (void)sig;
  g_quit = 1;
}

static void fill_color(uint32_t *pixels, uint32_t width, uint32_t height, uint32_t stride, uint32_t color) {
  for (uint32_t y = 0; y < height; y++) {
    uint32_t *row = (uint32_t *)((uint8_t *)pixels + y * stride);
    for (uint32_t x = 0; x < width; x++)
      row[x] = color;
  }
}

int main(void) {
  signal(SIGTERM, sig_handler);
  signal(SIGINT, sig_handler);

  struct DRM_Mgr *mgr = drm_mgr_init();
  if (!mgr)
    return 1;

  struct fb_info info;
  uint32_t *fb = drm_mgr_acquire_fb(mgr, &info);
  if (!fb) {
    drm_mgr_free(mgr);
    return 1;
  }

  printf("acquired fb: %ux%u stride=%u size=%u\n", info.width, info.height, info.stride, info.size);

  const uint32_t colors[] = {
      0x00FF0000, // red
      0x0000FF00, // green
      0x000000FF, // blue
      0x00FFFF00, // yellow
      0x00FF00FF, // magenta
      0x0000FFFF, // cyan
  };
  const size_t ncolors = sizeof(colors) / sizeof(colors[0]);

  size_t i = 0;
  while (!g_quit) {
    uint32_t color = colors[i++ % ncolors];
    printf("frame %d: color 0x%08x\n", i, color);
    fill_color(fb, info.width, info.height, info.stride, color);
    sleep(1);
  }

  drm_mgr_release_fb(mgr);
  drm_mgr_free(mgr);

  return 0;
}
