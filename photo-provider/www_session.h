#pragma once

#include <stdbool.h>
#include <stdint.h>

// A session with the wwwslide HTTP server. Owns:
//   - a registered client_id (refreshed on every config change)
//   - the current target_size / embed_qr params
//   - two CURL handles (one for the fetch path used by the cache worker
//     thread; one for the control path used by the dbus thread)
//
// Fetch methods are safe to call concurrently with setters: the setter may
// change the client_id while a fetch is in flight; the cache layer is
// responsible for discarding any stale fetch result (generation counter).
//
// When a setter triggers a re-register, the on_invalidate callback is
// invoked after the new client_id is installed so the cache can flush its
// ring eagerly.

typedef void (*pp_ws_invalidate_fn)(void *ud);

struct pp_www_session;

// Constructs the session. No network is performed yet; fetch methods will
// fail with -1 until start() succeeds.
struct pp_www_session *pp_www_session_init(const char *server_url, uint32_t target_w, uint32_t target_h, bool embed_qr,
                                           uint32_t connect_timeout_s, uint32_t request_timeout_s);

// Installs the invalidate callback (once, immutable after) and performs the
// initial registration + config push. Callers should wire any dependents
// (eg. cache) before calling this so the callback is valid if it fires.
// Returns 0 on success.
int pp_www_session_start(struct pp_www_session *s, pp_ws_invalidate_fn on_invalidate, void *ud);

void pp_www_session_free(struct pp_www_session *s);

// Setters. Trigger a re-register + invalidate callback if the value actually
// changed. Return 0 on success (including "no change"), -1 on failure.
int pp_www_session_set_target_size(struct pp_www_session *s, uint32_t w, uint32_t h);
int pp_www_session_set_embed_qr(struct pp_www_session *s, bool v);

// Fetch the next image + its metadata as a pair (they share a client_id
// snapshot, so they always correspond to the same server-side state). On
// success, *fd_out is a memfd (caller closes) and *meta_out is a malloc'd
// JSON string (caller frees). On failure returns -1 with both outputs
// untouched and nothing to clean up.
int pp_www_session_fetch_next(struct pp_www_session *s, int *fd_out, char **meta_out);
