#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "www_session.h"

struct pp_cache;

struct pp_cache_params {
  struct pp_www_session *ws; // borrowed, not owned
  uint32_t cache_depth;
  uint32_t history_depth; // 0 disables the "prev" history
  bool dump_to_disk;
  const char *dump_dir;
};

// Starts a worker thread that refills the ring from ws. Main is expected
// to wire pp_cache_invalidate as ws's invalidate callback so the ring is
// flushed when a config change rotates the client_id server-side.
struct pp_cache *pp_cache_init(const struct pp_cache_params *p);
void pp_cache_free(struct pp_cache *c);

// Signature matches pp_ws_invalidate_fn; pass as ws's on_invalidate.
void pp_cache_invalidate(void *cache);

// Advances the cursor to the next photo and returns a copy. On success,
// *fd_out is an owned memfd (caller closes) and *meta_out is a malloc'd
// JSON string (caller frees). Blocks up to timeout_ms. Returns 0 on
// success, -1 on timeout/error.
int pp_cache_pop(struct pp_cache *c, int *fd_out, char **meta_out, int timeout_ms);

// Retreats the cursor to the previous photo (still within the history
// window) and returns a copy. Non-blocking: returns -1 immediately if the
// cursor is already at the oldest held entry. A subsequent pp_cache_pop
// walks forward over the same entry, so "prev then next" is symmetric.
// Ownership of fd/meta matches pp_cache_pop.
int pp_cache_pop_prev(struct pp_cache *c, int *fd_out, char **meta_out);
