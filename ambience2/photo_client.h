#pragma once

#include <stdint.h>
#include <stdbool.h>

struct PhotoClient;
struct PhotoClient* photo_client_init();
void photo_client_free(struct PhotoClient *pc);
int photo_client_fetch_one(struct PhotoClient *pc, const char *method, int *fd_out, char **meta_out);
int push_initial_config(struct PhotoClient *pc, uint32_t w, uint32_t h, bool embed_qr);
