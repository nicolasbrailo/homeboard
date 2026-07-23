# presence-service

Filters a noisy raw occupancy sensor (PIR or mmWave) into a debounced room
presence signal on the system D-Bus. It consumes 1Hz
`io.homeboard.Occupancy1.Report` signals from
[occupancy-sensor-ld2410s](../occupancy-sensor-ld2410s/README.md), applies
hysteresis + a sensor-stabilization delay to kill spurious wakeups, and emits a
`PresenceChanged` signal only on genuine transitions. It also drives the display
on/off in lockstep with presence.

## Filtering

Raw reports are noisy, so a transition is only committed after enough
consecutive reports agree:

- `hysteresis_occupied` — consecutive `occupied` reports required before
  announcing presence=true.
- `hysteresis_vacant` — consecutive `vacant` reports required before
  announcing presence=false.
- `sensor_stabilization_delay_secs` — reports are observed but not announced for
  this many seconds each time the sensor service (re)appears on the bus, hiding
  flicker during the sensor's own boot.

## Silent (nighttime) period

During a configurable "silent" window — intended for nighttime — stricter
hysteresis is applied so a stray reading doesn't wake the room. Outside the
window, the normal `hysteresis_*` values apply.

The typical intent is to make it *hard to turn on* but *easy to turn off* at
night: raise the occupied threshold well above the daytime value (e.g. 30, so
brief noise won't trigger a wakeup) while lowering the vacant threshold (e.g. 5,
so the room goes dark quickly once you leave).

Config keys:

| Key | Meaning |
|---|---|
| `silent_start` | Window start, local time `"HH:MM"` (24h) |
| `silent_end` | Window end, local time `"HH:MM"` (24h), exclusive |
| `silent_hysteresis_occupied` | Occupied threshold used inside the window |
| `silent_hysteresis_vacant` | Vacant threshold used inside the window |

Notes:

- The window is `[silent_start, silent_end)` and **wraps midnight** when start >
  end, so `"23:00"`–`"07:00"` covers the night as expected.
- Times are **local wall-clock** (via `localtime`), so the window follows the
  host's timezone and DST. On a UTC-only host, offset the values yourself.
- The window is checked **per report**, so crossing a boundary mid-transition
  simply switches which threshold applies from that report on. A pending run of
  occupied reports that started before the window won't commit until it reaches
  the (stricter) silent threshold.
- Omit `silent_start`/`silent_end` (or leave either malformed) to disable the
  silent period entirely; behaviour then falls back to normal hysteresis at all
  hours.

### Example

```json
{
  "hysteresis_occupied": 5,
  "hysteresis_vacant": 9,

  "silent_start": "23:00",
  "silent_end": "07:00",
  "silent_hysteresis_occupied": 30,
  "silent_hysteresis_vacant": 5
}
```

Between 23:00 and 07:00 local time, it takes 30 consecutive occupied reports to
turn presence on and 5 to turn it off; the rest of the day, the normal 5 / 9
thresholds apply.
