#define _GNU_SOURCE
#include <curl/curl.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>

#include "cache.h"
#include "config.h"
#include "dbus.h"
#include "www_session.h"

static volatile sig_atomic_t g_quit;

static void sig_handler(int sig) {
  (void)sig;
  g_quit = 1;
}

int main(int argc, char *argv[]) {
  if (argc < 2) {
    fprintf(stderr, "Usage: %s <config.json>\n", argv[0]);
    return 1;
  }

  struct pp_config cfg;
  if (pp_config_load(argv[1], &cfg) < 0)
    return 1;

  curl_global_init(CURL_GLOBAL_DEFAULT);

  struct pp_www_session *ws = pp_www_session_init(cfg.server_url, cfg.target_w, cfg.target_h, cfg.embed_qr,
                                                  cfg.connect_timeout_s, cfg.request_timeout_s);
  if (!ws) {
    fprintf(stderr, "pp_www_session_init failed\n");
    curl_global_cleanup();
    return 1;
  }

  struct pp_cache_params params = {
      .ws = ws,
      .cache_depth = cfg.cache_depth,
      .history_depth = cfg.history_depth,
      .dump_to_disk = cfg.dump_to_disk,
      .dump_dir = cfg.dump_dir,
  };
  struct pp_cache *cache = pp_cache_init(&params);
  if (!cache) {
    fprintf(stderr, "pp_cache_init failed\n");
    pp_www_session_free(ws);
    curl_global_cleanup();
    return 1;
  }

  if (pp_www_session_start(ws, pp_cache_invalidate, cache) < 0) {
    fprintf(stderr, "pp_www_session_start failed\n");
    pp_cache_free(cache);
    pp_www_session_free(ws);
    curl_global_cleanup();
    return 1;
  }

  if (pp_dbus_init(ws, cache) < 0) {
    pp_cache_free(cache);
    pp_www_session_free(ws);
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
  pp_www_session_free(ws);
  curl_global_cleanup();
  return 0;
}
