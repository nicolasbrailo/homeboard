#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "drm_mgr/drm_mgr.h"
#include "jpeg_render/img_render.h"
#include "jpeg_render/jpeg_loader.h"

static volatile sig_atomic_t g_quit;

static void sig_handler(int sig) {
  (void)sig;
  g_quit = 1;
}

static void usage(const char *prog) {
  fprintf(stderr, "Usage: %s [options] <image.jpg>\n", prog);
  fprintf(stderr, "  -r <0|90|180|270>      rotation (default: 0)\n");
  fprintf(stderr, "  -i <nearest|bilinear>   interpolation (default: bilinear)\n");
}

int main(int argc, char *argv[]) {
  enum rotation rot = ROT_0;
  enum interpolation interp = INTERP_BILINEAR;

  int opt;
  while ((opt = getopt(argc, argv, "r:i:")) != -1) {
    switch (opt) {
    case 'r': {
      int deg = atoi(optarg);
      if (deg != 0 && deg != 90 && deg != 180 && deg != 270) {
        fprintf(stderr, "invalid rotation: %s\n", optarg);
        return 1;
      }
      rot = (enum rotation)deg;
      break;
    }
    case 'i':
      if (strcmp(optarg, "nearest") == 0)
        interp = INTERP_NEAREST;
      else if (strcmp(optarg, "bilinear") == 0)
        interp = INTERP_BILINEAR;
      else {
        fprintf(stderr, "invalid interpolation: %s\n", optarg);
        return 1;
      }
      break;
    default:
      usage(argv[0]);
      return 1;
    }
  }

  if (optind >= argc) {
    usage(argv[0]);
    return 1;
  }
  const char *image_path = argv[optind];

  signal(SIGTERM, sig_handler);
  signal(SIGINT, sig_handler);

  struct DRM_Mgr *mgr = drm_mgr_init();
  if (!mgr)
    return 1;

  struct fb_info fbi;
  uint32_t *fb = drm_mgr_acquire_fb(mgr, &fbi);
  if (!fb) {
    drm_mgr_free(mgr);
    return 1;
  }

  printf("display: %ux%u\n", fbi.width, fbi.height);

  struct jpeg_image *img = jpeg_load(image_path, fbi.width, fbi.height);
  if (!img) {
    drm_mgr_release_fb(mgr);
    drm_mgr_free(mgr);
    return 1;
  }

  printf("loaded %s: %ux%u\n", image_path, img->width, img->height);

  img_render(fb, fbi.width, fbi.height, fbi.stride, img->pixels, img->width, img->height, rot, interp);
  jpeg_free(img);

  while (!g_quit)
    sleep(1);

  drm_mgr_release_fb(mgr);
  drm_mgr_free(mgr);
  return 0;
}
