#pragma once

#include "cache.h"
#include "www_session.h"

int pp_dbus_init(struct pp_www_session *ws, struct pp_cache *cache);
void pp_dbus_free(void);
int pp_dbus_run_once(int timeout_ms);
