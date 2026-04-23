#pragma once

#include <stdbool.h>

#include "drm.h"

#define DBUS_SERVICE "io.homeboard.Display"
#define DBUS_PATH "/io/homeboard/Display"
#define DBUS_INTERFACE "io.homeboard.Display1"

int dbus_init(struct DRM_state *drm);
void dbus_free();

// Run one iteration of the bus event loop, waiting up to timeout_ms for activity.
// Returns < 0 on fatal error.
int dbus_run_once(int timeout_ms);
