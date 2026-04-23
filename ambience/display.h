#pragma once

#include <systemd/sd-bus.h>

struct Display;

typedef void (*display_state_cb)(void *);

// `bus` is borrowed; the caller owns it and runs the sd_bus event loop.
struct Display *display_init(sd_bus *bus, display_state_cb on_display_turned_on, display_state_cb on_display_turned_off,
                             void *ud);
void display_free(struct Display *s);

// Synthesize an occupancy report. The next real report clears the override
// automatically. Must be called on the sd_bus dispatch thread.
void display_force_on(struct Display *s);
void display_force_off(struct Display *s);
