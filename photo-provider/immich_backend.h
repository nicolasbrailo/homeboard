#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "config.h"

// Backend for an Immich server, built on libimmich-random (a submodule). Picks
// pictures album by album, sampling each album as set in the config's
// "immich" section, and serves Immich's preview-size image. Same contract as
// every backend (see backend.h): fetch_next only runs on the cache worker
// thread, setters never block.
//
// Unlike wwwslide, Immich can't render a photo at the target size or embed a
// QR code in it, so those settings are ignored: set Immich's preview
// resolution (and JPEG format) in its image settings instead.

struct pp_immich_backend;

// Constructs the backend from cfg->immich and the common settings. No network
// is performed yet; the first fetch_next talks to the server.
struct pp_immich_backend *pp_immich_backend_init(const struct pp_config *cfg);

void pp_immich_backend_free(struct pp_immich_backend *s);

// Setters, with the same return values as pp_backend_set_*
int pp_immich_backend_set_target_size(struct pp_immich_backend *s, uint32_t w,
                                      uint32_t h);
int pp_immich_backend_set_embed_qr(struct pp_immich_backend *s, bool v);

// Fetches the next picture and its metadata, as pp_backend_fetch_next
int pp_immich_backend_fetch_next(struct pp_immich_backend *s, int *fd_out,
                                 char **meta_out);
