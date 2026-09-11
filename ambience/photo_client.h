#pragma once

#include <stdbool.h>
#include <stdint.h>

// Client for io.homeboard.PhotoProvider. Owns a private sd-bus connection, which
// is not thread safe: every call on a PhotoClient must come from one thread.
//
// If a call fails for any reason other than an error reply from
// photo-provider, the connection is dropped and the next call reconnects.

struct img_render_cfg;
struct PhotoClient;
struct PhotoClient *photo_client_init();
void photo_client_free(struct PhotoClient *pc);

// Fetches a photo with `method` (GetPhoto or GetPrevPhoto). Returns 0 on
// success: *fd_out is an fd to read the JPEG from (caller closes) and
// *meta_out a malloc'd metadata string (caller frees; may be NULL). Returns -1
// on failure, leaving *fd_out and *meta_out untouched.
int photo_client_fetch_one(struct PhotoClient *pc, const char *method,
                           int *fd_out, char **meta_out,
                           const struct img_render_cfg *render_cfg);

// Pushes target size and embed_qr to photo-provider. Returns 0 on success.
int push_initial_config(struct PhotoClient *pc, uint32_t w, uint32_t h,
                        bool embed_qr);
