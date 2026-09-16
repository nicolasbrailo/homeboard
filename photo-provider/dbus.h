#pragma once

#include "backend.h"
#include "cache.h"

int pp_dbus_init(struct pp_backend *backend, struct pp_cache *cache);
void pp_dbus_free(void);
int pp_dbus_run_once(int timeout_ms);
