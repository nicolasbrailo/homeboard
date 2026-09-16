#include "backend.h"

#include "immich_backend.h"
#include "wwwslide_backend.h"

#include <stdlib.h>

// Exactly one of the backend pointers is set, the one matching type
struct pp_backend {
  enum pp_backend_type type;
  struct pp_wwwslide_backend *wwwslide;
  struct pp_immich_backend *immich;
};

struct pp_backend *pp_backend_init(const struct pp_config *cfg) {
  struct pp_backend *b = calloc(1, sizeof(*b));
  if (!b)
    return NULL;

  b->type = cfg->backend;
  switch (cfg->backend) {
  case PP_BACKEND_WWWSLIDE:
    b->wwwslide = pp_wwwslide_backend_init(
        cfg->server_url, cfg->target_w, cfg->target_h, cfg->embed_qr,
        cfg->connect_timeout_s, cfg->request_timeout_s);
    break;
  case PP_BACKEND_IMMICH:
    b->immich = pp_immich_backend_init(cfg);
    break;
  }

  if (!b->wwwslide && !b->immich) {
    free(b);
    return NULL;
  }
  return b;
}

void pp_backend_free(struct pp_backend *b) {
  if (!b)
    return;
  switch (b->type) {
  case PP_BACKEND_WWWSLIDE:
    pp_wwwslide_backend_free(b->wwwslide);
    break;
  case PP_BACKEND_IMMICH:
    pp_immich_backend_free(b->immich);
    break;
  }
  free(b);
}

int pp_backend_set_target_size(struct pp_backend *b, uint32_t w, uint32_t h) {
  switch (b->type) {
  case PP_BACKEND_WWWSLIDE:
    return pp_wwwslide_backend_set_target_size(b->wwwslide, w, h);
  case PP_BACKEND_IMMICH:
    return pp_immich_backend_set_target_size(b->immich, w, h);
  }
  return -1;
}

int pp_backend_set_embed_qr(struct pp_backend *b, bool v) {
  switch (b->type) {
  case PP_BACKEND_WWWSLIDE:
    return pp_wwwslide_backend_set_embed_qr(b->wwwslide, v);
  case PP_BACKEND_IMMICH:
    return pp_immich_backend_set_embed_qr(b->immich, v);
  }
  return -1;
}

int pp_backend_fetch_next(struct pp_backend *b, int *fd_out, char **meta_out) {
  switch (b->type) {
  case PP_BACKEND_WWWSLIDE:
    return pp_wwwslide_backend_fetch_next(b->wwwslide, fd_out, meta_out);
  case PP_BACKEND_IMMICH:
    return pp_immich_backend_fetch_next(b->immich, fd_out, meta_out);
  }
  return -1;
}
