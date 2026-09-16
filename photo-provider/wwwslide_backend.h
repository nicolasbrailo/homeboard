#pragma once

#include <stdbool.h>
#include <stdint.h>

// A session with the wwwslide HTTP server. Owns:
//   - a registered client_id (refreshed on every config change)
//   - the current target_size / embed_qr params
//   - one CURL handle
//
// All network IO happens in pp_wwwslide_backend_fetch_next, which must only be
// called from one thread (the cache worker). Setters may be called from any
// other thread: they never block, they only record the new value and flag a
// registration, which fetch_next performs before its next fetch. A fetch that
// was already in flight still uses the old config; the cache layer is
// responsible for discarding it (generation counter).

struct pp_wwwslide_backend;

// Constructs the session. No network is performed yet; the first fetch_next
// registers with the server.
struct pp_wwwslide_backend *pp_wwwslide_backend_init(
    const char *server_url, uint32_t target_w, uint32_t target_h, bool embed_qr,
    uint32_t connect_timeout_s, uint32_t request_timeout_s);

void pp_wwwslide_backend_free(struct pp_wwwslide_backend *s);

// Setters. Return 1 if the value changed (a re-register is scheduled; the
// caller should drop photos fetched with the old config), 0 if unchanged, -1
// if the value is invalid.
int pp_wwwslide_backend_set_target_size(struct pp_wwwslide_backend *s,
                                        uint32_t w, uint32_t h);
int pp_wwwslide_backend_set_embed_qr(struct pp_wwwslide_backend *s, bool v);

// (Re-)registers first if needed (config change, stale client, or a previous
// registration failed), then fetches the next image + its metadata as a pair
// (they use the same client_id, so they always correspond to the same
// server-side state). On success, *fd_out is a memfd (caller closes) and
// *meta_out is a malloc'd JSON string (caller frees). On failure returns -1
// with both outputs untouched and nothing to clean up.
int pp_wwwslide_backend_fetch_next(struct pp_wwwslide_backend *s, int *fd_out,
                                   char **meta_out);
