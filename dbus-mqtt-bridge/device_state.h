#pragma once

// Collects the device state the bridge sees, and publishes it as one retained
// JSON record on <prefix>state whenever it changes. The record has the same
// shape as the Android sister device's (AstroDock), so one UI renders both;
// fields homeboard doesn't know are null.

#include <stdbool.h>
#include <stdint.h>

struct rc_mqtt;
struct rc_state;

struct rc_state *rc_state_new(struct rc_mqtt *mqtt);
void rc_state_free(struct rc_state *s);

void rc_state_set_occupancy(struct rc_state *s, bool occupied,
                            uint32_t distance_cm);
void rc_state_set_slideshow_active(struct rc_state *s, bool active);
void rc_state_set_album_filter(struct rc_state *s, const char *name,
                               const char *exclude, uint32_t from_year,
                               uint32_t to_year);

// Call from the main loop; re-reads wifi every few seconds
void rc_state_poll_wifi(struct rc_state *s);
