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
  fprintf(stderr, "  -r <0|90|180|270>           rotation (default: 0)\n");
  fprintf(stderr, "  -i <nearest|bilinear>       interpolation (default: bilinear)\n");
  fprintf(stderr, "  -h <left|center|right>      horizontal alignment (default: center)\n");
  fprintf(stderr, "  -v <top|center|bottom>      vertical alignment (default: center)\n");
}

int main(int argc, char *argv[]) {
  struct img_render_cfg cfg = {
      .rot = ROT_0,
      .interp = INTERP_BILINEAR,
      .h_align = HORIZONTAL_ALIGN_CENTER,
      .v_align = VERTICAL_ALIGN_CENTER,
  };

  int opt;
  while ((opt = getopt(argc, argv, "r:i:h:v:")) != -1) {
    switch (opt) {
    case 'r': {
      int deg = atoi(optarg);
      if (deg != 0 && deg != 90 && deg != 180 && deg != 270) {
        fprintf(stderr, "invalid rotation: %s\n", optarg);
        return 1;
      }
      cfg.rot = (enum rotation)deg;
      break;
    }
    case 'i':
      if (strcmp(optarg, "nearest") == 0)
        cfg.interp = INTERP_NEAREST;
      else if (strcmp(optarg, "bilinear") == 0)
        cfg.interp = INTERP_BILINEAR;
      else {
        fprintf(stderr, "invalid interpolation: %s\n", optarg);
        return 1;
      }
      break;
    case 'h':
      if (strcmp(optarg, "left") == 0)
        cfg.h_align = HORIZONTAL_ALIGN_LEFT;
      else if (strcmp(optarg, "center") == 0)
        cfg.h_align = HORIZONTAL_ALIGN_CENTER;
      else if (strcmp(optarg, "right") == 0)
        cfg.h_align = HORIZONTAL_ALIGN_RIGHT;
      else {
        fprintf(stderr, "invalid horizontal alignment: %s\n", optarg);
        return 1;
      }
      break;
    case 'v':
      if (strcmp(optarg, "top") == 0)
        cfg.v_align = VERTICAL_ALIGN_TOP;
      else if (strcmp(optarg, "center") == 0)
        cfg.v_align = VERTICAL_ALIGN_CENTER;
      else if (strcmp(optarg, "bottom") == 0)
        cfg.v_align = VERTICAL_ALIGN_BOTTOM;
      else {
        fprintf(stderr, "invalid vertical alignment: %s\n", optarg);
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

  img_render(fb, fbi.width, fbi.height, fbi.stride, img->pixels, img->width, img->height, &cfg);
  jpeg_free(img);

  while (!g_quit)
    sleep(1);

  drm_mgr_release_fb(mgr);
  drm_mgr_free(mgr);
  return 0;
}
