#define _GNU_SOURCE
#include "immich_backend.h"

#include "libimmich-random/immich.h"

#include <json-c/json.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

struct pp_immich_backend {
  struct immich_client *client;
  struct immich_random_album_picture *picker;

  // The filter handed to the picker, kept only to answer "did this change?".
  // D-Bus thread only: the picker owns the copy the fetch thread reads.
  struct pp_album_filter_config filter;

  // Fetch thread only
  uint32_t album_refresh_s; // 0: never
  long last_refresh_s;      // Monotonic
  // The last rotation we reported, so a worker retrying every couple of
  // seconds doesn't repeat itself
  enum immich_album_selection last_selection;
  size_t last_total;
  size_t last_kept;
};

// Hands f to the picker. Returns 0, or -1 if the picker rejected it (a year
// out of range, or a reversed year range: it says which on stderr).
static int push_filter(struct pp_immich_backend *s,
                       const struct pp_album_filter_config *f) {
  const struct immich_album_filter af = {
      .name = f->name,
      .exclude = f->exclude,
      .from_year = f->from_year,
      .to_year = f->to_year,
  };
  return immich_random_album_picture_set_filter(s->picker, &af);
}

static long now_monotonic_s(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (long)ts.tv_sec;
}

struct pp_immich_backend *pp_immich_backend_init(const struct pp_config *cfg) {
  const struct pp_immich_config *icfg = &cfg->immich;
  struct pp_immich_backend *s = calloc(1, sizeof(*s));
  if (!s)
    return NULL;

  // Neither of these touches the network: the picker fetches the album list on
  // the first fetch_next. connect_timeout_s / request_timeout_s don't apply:
  // libimmich-random has no timeout settings (it gives up connecting after
  // 10s, and aborts transfers that stall for 30s).
  s->client = immich_client_new(icfg->server_url, icfg->api_key);
  if (!s->client) {
    pp_immich_backend_free(s);
    return NULL;
  }
  s->picker = immich_random_album_picture_new(
      s->client, icfg->max_pictures_per_album, icfg->percent_per_album);
  if (!s->picker) {
    pp_immich_backend_free(s);
    return NULL;
  }

  s->filter = icfg->album_filter;
  if (push_filter(s, &s->filter) < 0) {
    // pp_config_load already rejects the ranges the picker rejects, so this
    // means the two disagree. Start unfiltered rather than not at all.
    fprintf(stderr, "immich backend: the configured album filter was rejected, "
                    "starting with no filter\n");
    memset(&s->filter, 0, sizeof(s->filter));
    immich_random_album_picture_set_filter(s->picker, NULL);
  }

  s->album_refresh_s = icfg->album_refresh_s;
  s->last_refresh_s = now_monotonic_s();
  s->last_selection = IMMICH_ALBUMS_NONE_KNOWN;
  if (cfg->embed_qr)
    printf("immich backend: embed_qr isn't supported, ignoring it\n");
  return s;
}

void pp_immich_backend_free(struct pp_immich_backend *s) {
  if (!s)
    return;
  immich_random_album_picture_free(s->picker);
  immich_client_free(s->client);
  free(s);
}

int pp_immich_backend_set_target_size(struct pp_immich_backend *s, uint32_t w,
                                      uint32_t h) {
  // Immich can't render a picture at a given size, so we always serve its
  // preview (its resolution is set in Immich's image settings). Nothing
  // changes, so the cache keeps its photos.
  printf("immich backend: ignoring target size %ux%u, serving Immich's "
         "preview size\n",
         w, h);
  return 0;
}

int pp_immich_backend_set_embed_qr(struct pp_immich_backend *s, bool v) {
  if (v)
    printf("immich backend: embed_qr isn't supported, ignoring it\n");
  return 0;
}

static bool same_filter(const struct pp_album_filter_config *a,
                        const struct pp_album_filter_config *b) {
  return a->from_year == b->from_year && a->to_year == b->to_year &&
         strcmp(a->name, b->name) == 0 && strcmp(a->exclude, b->exclude) == 0;
}

int pp_immich_backend_set_album_filter(struct pp_immich_backend *s,
                                       const struct pp_album_filter_config *f) {
  if (same_filter(&s->filter, f))
    return 0;
  if (push_filter(s, f) < 0)
    return -1;

  s->filter = *f;
  if (f->name[0] || f->exclude[0] || f->from_year || f->to_year)
    printf("immich backend: album filter set to name=\"%s\" exclude=\"%s\" "
           "years=%u..%u\n",
           f->name, f->exclude, f->from_year, f->to_year);
  else
    printf("immich backend: album filter cleared, every album is back in the "
           "rotation\n");
  return 1;
}

const char *pp_immich_backend_unavailable_reason(struct pp_immich_backend *s) {
  // Lock-free, so this answers straight away even while the worker is blocked
  // on a download
  if (immich_random_album_picture_status(s->picker, NULL, NULL) ==
      IMMICH_ALBUMS_NONE_MATCH_FILTER)
    return "No album matches the album filter";
  return NULL;
}

// Reports how many albums survived the filter, but only when that changes: the
// cache worker retries every couple of seconds while nothing can be served,
// and "1236 albums, 9 kept" is only worth saying once per rotation.
static void log_selection(struct pp_immich_backend *s) {
  size_t total = 0;
  size_t kept = 0;
  enum immich_album_selection sel =
      immich_random_album_picture_status(s->picker, &total, &kept);
  if (sel == s->last_selection && total == s->last_total &&
      kept == s->last_kept)
    return;
  s->last_selection = sel;
  s->last_total = total;
  s->last_kept = kept;

  switch (sel) {
  case IMMICH_ALBUMS_OK:
    printf("immich backend: %zu albums, %zu in the rotation\n", total, kept);
    break;
  case IMMICH_ALBUMS_NONE_KNOWN:
    printf("immich backend: no album list yet\n");
    break;
  case IMMICH_ALBUMS_NONE_WITH_PICTURES:
    printf("immich backend: %zu albums, none with any pictures\n", total);
    break;
  case IMMICH_ALBUMS_NONE_MATCH_FILTER:
    // The filter itself was logged when it was set. Reading it here would
    // race the D-Bus thread setting the next one.
    printf("immich backend: %zu albums, none matches the album filter\n",
           total);
    break;
  }
}

// Asks the picker to re-fetch the album list every album_refresh_s. It does so
// when it moves on to its next album.
static void maybe_refresh_albums(struct pp_immich_backend *s) {
  if (s->album_refresh_s == 0)
    return;
  long now = now_monotonic_s();
  if (now - s->last_refresh_s < (long)s->album_refresh_s)
    return;
  immich_random_album_picture_refresh(s->picker);
  s->last_refresh_s = now;
}

// ambience only decodes JPEG, but Immich can be set to make WebP previews
static bool is_jpeg(int fd) {
  unsigned char magic[3];
  return pread(fd, magic, sizeof(magic), 0) == (ssize_t)sizeof(magic) &&
         magic[0] == 0xFF && magic[1] == 0xD8 && magic[2] == 0xFF;
}

// Immich dates are ISO 8601 ("2019-07-14T12:34:56.000Z"), while wwwslide sends
// "EXIF DateTimeOriginal" in EXIF's format ("2019:07:14 12:34:56"). Converts
// the former to the latter; anything unexpected is passed through as is.
static void iso_to_exif_datetime(const char *iso, char *out, size_t out_sz) {
  snprintf(out, out_sz, "%.19s", iso);
  if (strlen(out) != 19 || out[4] != '-' || out[7] != '-' || out[10] != 'T')
    return;
  out[4] = ':';
  out[7] = ':';
  out[10] = ' ';
}

// Adds key: val to obj, unless val is empty
static void add_str(struct json_object *obj, const char *key, const char *val) {
  if (val[0])
    json_object_object_add(obj, key, json_object_new_string(val));
}

// Fetches the picture's metadata as JSON: the wwwslide keys consumers read
// (see backend.h), plus some of Immich's own. There's no "albumpath": the
// library doesn't say which album a picture was picked from. Returns a
// malloc'd string, or NULL on failure.
static char *fetch_meta_json(struct pp_immich_backend *s, const char *id) {
  struct immich_picture_info info;
  if (immich_get_picture_metadata(s->client, id, &info) < 0)
    return NULL;

  struct json_object *root = json_object_new_object();
  if (!root) {
    immich_picture_info_free(&info);
    return NULL;
  }

  add_str(root, "local_path", info.original_path);
  add_str(root, "filename", info.file_name);
  char taken[20];
  iso_to_exif_datetime(info.taken, taken, sizeof(taken));
  add_str(root, "EXIF DateTimeOriginal", taken);
  if (info.city[0] || info.state[0] || info.country[0]) {
    struct json_object *geo = json_object_new_object();
    add_str(geo, "city", info.city);
    add_str(geo, "state", info.state);
    add_str(geo, "country", info.country);
    json_object_object_add(root, "reverse_geo", geo);
  }

  add_str(root, "immich_id", info.id);
  add_str(root, "description", info.description);
  if (info.has_location) {
    json_object_object_add(root, "latitude",
                           json_object_new_double(info.latitude));
    json_object_object_add(root, "longitude",
                           json_object_new_double(info.longitude));
  }
  if (info.people_count > 0) {
    struct json_object *people = json_object_new_array();
    for (size_t i = 0; i < info.people_count; i++)
      json_object_array_add(people, json_object_new_string(info.people[i]));
    json_object_object_add(root, "people", people);
  }
  immich_picture_info_free(&info);

  const char *str =
      json_object_to_json_string_ext(root, JSON_C_TO_STRING_PLAIN);
  char *meta = str ? strdup(str) : NULL;
  json_object_put(root);
  return meta;
}

int pp_immich_backend_fetch_next(struct pp_immich_backend *s, int *fd_out,
                                 char **meta_out) {
  maybe_refresh_albums(s);

  char id[IMMICH_ID_SIZE];
  int picked = immich_get_random_album_picture(s->picker, id);
  // After the pick, so it sees the rotation this call may have rebuilt
  log_selection(s);
  if (picked < 0)
    return -1;

  int fd = immich_fetch_picture_to_fd(s->client, id, IMMICH_PICTURE_PREVIEW);
  if (fd < 0)
    return -1;
  if (!is_jpeg(fd)) {
    fprintf(stderr,
            "immich backend: preview of %s isn't a JPEG; set Immich's "
            "preview format to JPEG\n",
            id);
    close(fd);
    return -1;
  }

  char *meta = fetch_meta_json(s, id);
  if (!meta) {
    fprintf(stderr, "meta fetch failed; serving image with empty metadata\n");
    meta = strdup("{}");
    if (!meta) {
      close(fd);
      return -1;
    }
  }

  *fd_out = fd;
  *meta_out = meta;
  return 0;
}
