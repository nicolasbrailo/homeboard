#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <systemd/sd-bus.h>

typedef void (*ambience_next_cb)(void *);
typedef void (*ambience_prev_cb)(void *);
typedef void (*ambience_force_cb)(void *);
typedef bool (*ambience_set_transition_time_cb)(void *, uint32_t);
typedef int (*ambience_announce_requested_cb)(void*, uint32_t, const char*);

struct AmbienceDbus;

// Hosts io.homeboard.Ambience on the given bus (borrowed; owned by the caller
// and must outlive the AmbienceDbus). All callbacks are invoked from the bus
// dispatch loop and share the same `ud`.
struct AmbienceDbus *ambience_dbus_init(sd_bus *bus, ambience_next_cb on_next, ambience_prev_cb on_prev,
                                        ambience_force_cb on_force_on, ambience_force_cb on_force_off,
                                        ambience_set_transition_time_cb on_set_transition_time,
                                        ambience_announce_requested_cb on_announce_requested_cb,
                                        void *ud);
void ambience_dbus_free(struct AmbienceDbus *d);

// Broadcast a DisplayingPhoto(s) signal on io.homeboard.Ambience1. Safe to
// call from any thread as long as `bus` is used only by the caller's thread.
// Pass the slideshow worker's private bus when emitting from the worker.
//
// Subscriber note: this signal is emitted from the worker's private
// connection, whose unique name is NOT the owner of io.homeboard.Ambience
// (that well-known name is held by the main dispatch bus). dbus-daemon
// resolves a well-known sender in a match rule to the current owner's unique
// name at install time, so a match that filters on sender=io.homeboard.Ambience
// will silently drop these signals. Subscribers must pass NULL as the sender
// to sd_bus_match_signal (or equivalent) and rely on path+interface+member
// to identify the signal.
int ambience_dbus_emit_displaying_photo(sd_bus *bus, const char *meta);

// Broadcast a SlideshowActive(b) signal on io.homeboard.Ambience1. Emitted
// on the main dispatch bus (the connection that owns io.homeboard.Ambience),
// so sender-based match filters work here — but subscribers that already
// use NULL sender for DisplayingPhoto can keep doing so uniformly.
int ambience_dbus_emit_slideshow_active(sd_bus *bus, bool active);

// Process pending messages and block until new activity (or a signal).
// Returns negative on error, 0 otherwise.
int ambience_dbus_run_once(struct AmbienceDbus *d);
