#include "config.h"

#include <json-c/json.h>
#include <stdio.h>
#include <string.h>

static int parse_backend(const char *name, enum pp_backend_type *out) {
  if (!name)
    return -1;
  if (strcmp(name, "wwwslide") == 0) {
    *out = PP_BACKEND_WWWSLIDE;
    return 0;
  }
  if (strcmp(name, "immich") == 0) {
    *out = PP_BACKEND_IMMICH;
    return 0;
  }
  return -1;
}

// A year outside 0..9999 can't have come from a filter this service wrote, so
// it's a hand-edited file: clamp to "no bound" and say so, rather than
// refusing to start over a setting the user can't see.
static uint32_t load_year(struct json_object *val, const char *key) {
  int n = json_object_get_int(val);
  if (n < 0 || n > 9999) {
    fprintf(stderr,
            "Config: immich.album_filter.%s=%d is out of 0..9999, "
            "ignoring it\n",
            key, n);
    return 0;
  }
  return (uint32_t)n;
}

static void load_album_filter(struct json_object *filter,
                              struct pp_album_filter_config *f) {
  struct json_object *val;
  if (json_object_object_get_ex(filter, "name", &val))
    strncpy(f->name, json_object_get_string(val), sizeof(f->name) - 1);
  if (json_object_object_get_ex(filter, "exclude", &val))
    strncpy(f->exclude, json_object_get_string(val), sizeof(f->exclude) - 1);
  if (json_object_object_get_ex(filter, "from_year", &val))
    f->from_year = load_year(val, "from_year");
  if (json_object_object_get_ex(filter, "to_year", &val))
    f->to_year = load_year(val, "to_year");

  // The year test is an overlap test, so a reversed range selects the albums
  // straddling the boundary instead of nothing, which reads as the filter
  // being ignored. Drop the range rather than apply that.
  if (f->from_year != 0 && f->to_year != 0 && f->from_year > f->to_year) {
    fprintf(stderr,
            "Config: immich.album_filter from_year %u is after to_year "
            "%u, ignoring both\n",
            f->from_year, f->to_year);
    f->from_year = 0;
    f->to_year = 0;
  }
}

static void load_immich_section(struct json_object *immich,
                                struct pp_immich_config *icfg) {
  struct json_object *val;
  if (json_object_object_get_ex(immich, "server_url", &val))
    strncpy(icfg->server_url, json_object_get_string(val),
            sizeof(icfg->server_url) - 1);
  if (json_object_object_get_ex(immich, "api_key", &val))
    strncpy(icfg->api_key, json_object_get_string(val),
            sizeof(icfg->api_key) - 1);
  if (json_object_object_get_ex(immich, "max_pictures_per_album", &val)) {
    int n = json_object_get_int(val);
    icfg->max_pictures_per_album = n < 0 ? 0 : (uint32_t)n;
  }
  if (json_object_object_get_ex(immich, "percent_per_album", &val)) {
    int n = json_object_get_int(val);
    icfg->percent_per_album = n < 0 ? 0 : n > 100 ? 100 : (uint32_t)n;
  }
  if (json_object_object_get_ex(immich, "album_refresh_s", &val)) {
    int n = json_object_get_int(val);
    icfg->album_refresh_s = n < 0 ? 0 : (uint32_t)n;
  }
  if (json_object_object_get_ex(immich, "album_filter", &val))
    load_album_filter(val, &icfg->album_filter);
}

static int check_immich(const struct pp_immich_config *icfg) {
  if (icfg->server_url[0] == '\0') {
    fprintf(stderr, "Config: immich.server_url is required\n");
    return -1;
  }
  if (icfg->api_key[0] == '\0') {
    fprintf(stderr, "Config: immich.api_key is required\n");
    return -1;
  }
  return 0;
}

int pp_config_load(const char *path, struct pp_config *cfg) {
  struct json_object *root = json_object_from_file(path);
  if (!root) {
    fprintf(stderr, "Failed to parse config: %s\n", path);
    return -1;
  }

  memset(cfg, 0, sizeof(*cfg));
  cfg->backend = PP_BACKEND_WWWSLIDE;
  strncpy(cfg->server_url, "http://homeboard.local:8080",
          sizeof(cfg->server_url));
  cfg->target_w = 1920;
  cfg->target_h = 1080;
  cfg->embed_qr = true;
  cfg->cache_depth = 2;
  cfg->history_depth = 8;
  cfg->dump_to_disk = false;
  strncpy(cfg->dump_dir, "/tmp/photo-provider", sizeof(cfg->dump_dir));
  cfg->connect_timeout_s = 5;
  cfg->request_timeout_s = 60;

  struct json_object *val;
  // Unlike other keys, a bad backend is fatal: silently falling back to
  // wwwslide would serve photos from a server the user didn't ask for.
  if (json_object_object_get_ex(root, "backend", &val) &&
      parse_backend(json_object_get_string(val), &cfg->backend) < 0) {
    fprintf(stderr,
            "Config %s: unknown backend %s (expected \"wwwslide\" or "
            "\"immich\")\n",
            path, json_object_to_json_string(val));
    json_object_put(root);
    return -1;
  }
  if (json_object_object_get_ex(root, "server_url", &val))
    strncpy(cfg->server_url, json_object_get_string(val),
            sizeof(cfg->server_url) - 1);
  if (json_object_object_get_ex(root, "target_size_w", &val))
    cfg->target_w = (uint32_t)json_object_get_int(val);
  if (json_object_object_get_ex(root, "target_size_h", &val))
    cfg->target_h = (uint32_t)json_object_get_int(val);
  if (json_object_object_get_ex(root, "embed_qr", &val))
    cfg->embed_qr = json_object_get_boolean(val);
  if (json_object_object_get_ex(root, "cache_depth", &val)) {
    int n = json_object_get_int(val);
    cfg->cache_depth = n < 1 ? 1 : (uint32_t)n;
  }
  if (json_object_object_get_ex(root, "history_depth", &val)) {
    int n = json_object_get_int(val);
    cfg->history_depth = n < 0 ? 0 : (uint32_t)n;
  }
  if (json_object_object_get_ex(root, "dump_to_disk", &val))
    cfg->dump_to_disk = json_object_get_boolean(val);
  if (json_object_object_get_ex(root, "dump_dir", &val))
    strncpy(cfg->dump_dir, json_object_get_string(val),
            sizeof(cfg->dump_dir) - 1);
  if (json_object_object_get_ex(root, "connect_timeout_s", &val))
    cfg->connect_timeout_s = (uint32_t)json_object_get_int(val);
  if (json_object_object_get_ex(root, "request_timeout_s", &val))
    cfg->request_timeout_s = (uint32_t)json_object_get_int(val);
  if (json_object_object_get_ex(root, "immich", &val))
    load_immich_section(val, &cfg->immich);

  json_object_put(root);

  if (cfg->backend == PP_BACKEND_IMMICH && check_immich(&cfg->immich) < 0)
    return -1;

  printf("Config loaded: backend=%s %ux%u qr=%d cache=%u history=%u dump=%d "
         "dir=%s\n",
         cfg->backend == PP_BACKEND_IMMICH ? "immich" : "wwwslide",
         cfg->target_w, cfg->target_h, cfg->embed_qr, cfg->cache_depth,
         cfg->history_depth, cfg->dump_to_disk, cfg->dump_dir);
  if (cfg->backend == PP_BACKEND_IMMICH) {
    printf("Immich: server=%s max_per_album=%u percent_per_album=%u "
           "album_refresh_s=%u\n",
           cfg->immich.server_url, cfg->immich.max_pictures_per_album,
           cfg->immich.percent_per_album, cfg->immich.album_refresh_s);
    const struct pp_album_filter_config *f = &cfg->immich.album_filter;
    if (f->name[0] || f->exclude[0] || f->from_year || f->to_year)
      printf("Immich album filter: name=\"%s\" exclude=\"%s\" years=%u..%u\n",
             f->name, f->exclude, f->from_year, f->to_year);
    else
      printf("Immich album filter: none, every album is in the rotation\n");
  } else
    printf("wwwslide: server=%s\n", cfg->server_url);
  return 0;
}

int pp_config_save(const char *path, const struct pp_config *cfg) {
  struct json_object *root = json_object_new_object();
  struct json_object *immich = json_object_new_object();
  struct json_object *filter = json_object_new_object();
  if (!root || !immich || !filter) {
    fprintf(stderr, "Failed to allocate json object for config\n");
    json_object_put(root);
    json_object_put(immich);
    json_object_put(filter);
    return -1;
  }

  json_object_object_add(
      root, "backend",
      json_object_new_string(cfg->backend == PP_BACKEND_IMMICH ? "immich"
                                                               : "wwwslide"));
  json_object_object_add(root, "server_url",
                         json_object_new_string(cfg->server_url));
  json_object_object_add(root, "target_size_w",
                         json_object_new_int((int)cfg->target_w));
  json_object_object_add(root, "target_size_h",
                         json_object_new_int((int)cfg->target_h));
  json_object_object_add(root, "embed_qr",
                         json_object_new_boolean(cfg->embed_qr));
  json_object_object_add(root, "cache_depth",
                         json_object_new_int((int)cfg->cache_depth));
  json_object_object_add(root, "history_depth",
                         json_object_new_int((int)cfg->history_depth));
  json_object_object_add(root, "dump_to_disk",
                         json_object_new_boolean(cfg->dump_to_disk));
  json_object_object_add(root, "dump_dir",
                         json_object_new_string(cfg->dump_dir));
  json_object_object_add(root, "connect_timeout_s",
                         json_object_new_int((int)cfg->connect_timeout_s));
  json_object_object_add(root, "request_timeout_s",
                         json_object_new_int((int)cfg->request_timeout_s));

  const struct pp_immich_config *icfg = &cfg->immich;
  json_object_object_add(immich, "server_url",
                         json_object_new_string(icfg->server_url));
  json_object_object_add(immich, "api_key",
                         json_object_new_string(icfg->api_key));
  json_object_object_add(
      immich, "max_pictures_per_album",
      json_object_new_int((int)icfg->max_pictures_per_album));
  json_object_object_add(immich, "percent_per_album",
                         json_object_new_int((int)icfg->percent_per_album));
  json_object_object_add(immich, "album_refresh_s",
                         json_object_new_int((int)icfg->album_refresh_s));

  json_object_object_add(filter, "name",
                         json_object_new_string(icfg->album_filter.name));
  json_object_object_add(filter, "exclude",
                         json_object_new_string(icfg->album_filter.exclude));
  json_object_object_add(
      filter, "from_year",
      json_object_new_int((int)icfg->album_filter.from_year));
  json_object_object_add(filter, "to_year",
                         json_object_new_int((int)icfg->album_filter.to_year));
  json_object_object_add(immich, "album_filter", filter);
  json_object_object_add(root, "immich", immich);

  int rc = json_object_to_file_ext(
      path, root, JSON_C_TO_STRING_PRETTY | JSON_C_TO_STRING_SPACED);
  if (rc != 0)
    fprintf(stderr, "Failed to write config to %s\n", path);
  json_object_put(root);
  return rc;
}
