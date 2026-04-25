#pragma once

#include <stdbool.h>

// Presence policy. Pure logic, no D-Bus, no display calls.
//
// Inputs:
//   - 1Hz raw occupancy reports from the sensor (presence_on_report)
//   - manual ForceOn / ForceOff overrides
//   - sensor service appearing on / dropping off the bus
//
// Output: a debounced presence boolean delivered via the on_change callback,
// fired only on transitions. Hysteresis (hyst_occupied / hyst_vacant) and a
// sensor-stabilization delay (applied each time the sensor service comes
// up) suppress flicker; a force-off latch survives repeated occupied=true
// reports until the room genuinely empties (i.e. hysteresis commits to
// false).
//
// All functions must be called from a single thread.

struct Presence;

typedef void (*presence_change_cb)(void *ud, bool present);

struct Presence *presence_init(unsigned sensor_stabilization_delay_secs,
                               unsigned hyst_occupied, unsigned hyst_vacant,
                               presence_change_cb cb, void *ud);
void presence_free(struct Presence *p);

// 1Hz raw report from the sensor.
void presence_on_report(struct Presence *p, bool occupied);

// Manual overrides:
//   force_on:  clear latch, behave as occupied until the next genuine vacancy.
//   force_off: set latch, behave as vacant until hysteresis commits to false
//              (clearing the latch).
void presence_force_on(struct Presence *p);
void presence_force_off(struct Presence *p);

// Sensor service appeared on the bus. Re-arms the sensor-stabilization
// delay: the next `sensor_stabilization_delay_secs` reports are observed
// but not announced, so flicker during the sensor's own boot is hidden.
void presence_on_sensor_up(struct Presence *p);

// Sensor service disappeared from the bus: assume vacant, clear latch.
void presence_on_sensor_down(struct Presence *p);
