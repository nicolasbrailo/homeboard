#pragma once

#include <stdbool.h>
#include <systemd/sd-bus.h>

// One-shot check: does anyone currently own the occupancy service name?
bool is_service_up(sd_bus *bus, const char *svc_name);

// Invoked when svc_name appears on (up=true) or disappears from (up=false) the
// bus.
typedef void (*service_updown_cb)(void *ud, bool up);

// Subscribe to NameOwnerChanged for svc_name. Returns a slot owned by the
// caller; release with sd_bus_slot_unref.
sd_bus_slot *on_service_updown(sd_bus *bus, const char *svc_name,
                               service_updown_cb cb, void *ud);
