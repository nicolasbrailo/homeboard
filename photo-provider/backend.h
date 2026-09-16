#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "config.h"

// A source of photos, selected by the config's "backend" key. Dispatches to
// wwwslide_backend or immich_backend. Every backend follows the same
// contract:
//
// Threading: pp_backend_fetch_next does all network IO and must only be called
// from one thread (the cache worker). Setters may be called from any other
// thread: they never block, they only record the new value, which the next
// fetch applies.
//
// Metadata: fetch_next returns a JSON object in wwwslide's format, because
// consumers parse it: ambience shows "<reverse_geo.city>, <year>" on the eink,
// taking the year from the first 4 chars of "EXIF DateTimeOriginal", and logs
// "local_path" (or "albumpath" + "filename"). dbus-mqtt-bridge publishes the
// whole string to MQTT as is. Backends may add keys of their own.

struct pp_backend;

// Constructs the backend chosen by cfg->backend. No network is performed yet.
// Returns NULL on failure.
struct pp_backend *pp_backend_init(const struct pp_config *cfg);
void pp_backend_free(struct pp_backend *b);

// Setters. Return 1 if the value changed (the caller should drop photos
// fetched with the old config), 0 if unchanged or not supported by the
// backend, -1 if the value is invalid.
int pp_backend_set_target_size(struct pp_backend *b, uint32_t w, uint32_t h);
int pp_backend_set_embed_qr(struct pp_backend *b, bool v);

// Fetches the next photo and its metadata. On success, *fd_out is a memfd
// (caller closes) and *meta_out is a malloc'd JSON string (caller frees). On
// failure returns -1 with both outputs untouched and nothing to clean up.
int pp_backend_fetch_next(struct pp_backend *b, int *fd_out, char **meta_out);
