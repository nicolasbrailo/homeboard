#define _GNU_SOURCE
#include <curl/curl.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>

#include "backend.h"
#include "cache.h"
#include "config.h"
#include "dbus.h"

static volatile sig_atomic_t g_quit;

static void sig_handler(int sig) {
  (void)sig;
  g_quit = 1;
}

int main(int argc, char *argv[]) {
  // stdout is a pipe under systemd; without this, printf logs only reach the
  // journal in 4KB chunks.
  setvbuf(stdout, NULL, _IOLBF, 0);

  if (argc < 2) {
    fprintf(stderr, "Usage: %s <config.json>\n", argv[0]);
    return 1;
  }

  struct pp_config cfg;
  if (pp_config_load(argv[1], &cfg) < 0)
    return 1;

  curl_global_init(CURL_GLOBAL_DEFAULT);

  struct pp_backend *backend = pp_backend_init(&cfg);
  if (!backend) {
    fprintf(stderr, "pp_backend_init failed\n");
    curl_global_cleanup();
    return 1;
  }

  struct pp_cache_params params = {
      .backend = backend,
      .cache_depth = cfg.cache_depth,
      .history_depth = cfg.history_depth,
      .dump_to_disk = cfg.dump_to_disk,
      .dump_dir = cfg.dump_dir,
  };
  // The cache worker contacts the server on its first fetch, and keeps
  // retrying if the server is unreachable; until then GetPhoto answers
  // Unavailable.
  struct pp_cache *cache = pp_cache_init(&params);
  if (!cache) {
    fprintf(stderr, "pp_cache_init failed\n");
    pp_backend_free(backend);
    curl_global_cleanup();
    return 1;
  }

  if (pp_dbus_init(backend, cache) < 0) {
    pp_cache_free(cache);
    pp_backend_free(backend);
    curl_global_cleanup();
    return 1;
  }

  signal(SIGTERM, sig_handler);
  signal(SIGINT, sig_handler);

  printf("photo-provider running\n");
  while (!g_quit) {
    if (pp_dbus_run_once(1000) < 0)
      break;
  }

  pp_dbus_free();
  pp_cache_free(cache);
  pp_backend_free(backend);
  curl_global_cleanup();
  return 0;
}
