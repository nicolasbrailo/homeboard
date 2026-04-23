#define _GNU_SOURCE
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>

#include "dbus.h"
#include "drm.h"

static volatile sig_atomic_t g_quit;

static void sig_handler(int sig) {
  (void)sig;
  g_quit = 1;
}

int main(int argc, char *argv[]) {
  (void)argc;
  (void)argv;

  struct DRM_state *drm = drm_init();
  if (!drm)
    return 1;

  if (drm_create_framebuffer(drm) < 0) {
    drm_free(drm);
    return 1;
  }

  const bool display_on_at_startup = drm_is_display_on(drm);

  if (dbus_init(drm) < 0) {
    drm_free(drm);
    return 1;
  }

  signal(SIGTERM, sig_handler);
  signal(SIGINT, sig_handler);

  printf("display-mgr running, %zu output(s)\n", drm_get_num_outputs(drm));
  while (!g_quit) {
    if (dbus_run_once(1000) < 0)
      break;
  }

  if (display_on_at_startup)
    drm_display_on(drm);
  else
    drm_display_off(drm);

  dbus_free();
  drm_free(drm);
  return 0;
}
