#pragma once

#include "backend.h"
#include "cache.h"

// cfg and cfg_path let the setters persist what they change; both must
// outlive the service. cfg is the live copy: pp_dbus_init writes back to it.
int pp_dbus_init(struct pp_backend *backend, struct pp_cache *cache,
                 struct pp_config *cfg, const char *cfg_path);
void pp_dbus_free(void);
int pp_dbus_run_once(int timeout_ms);
