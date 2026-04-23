# occupancy-sensor-ld2410s

A C service for the HLK-LD2410S mmWave presence sensor. Runs on a Raspberry Pi,
talks to the sensor over UART and/or a GPIO pin, and broadcasts occupancy state
on the system D-Bus so other services can react to transitions.

This is a C port of the Python debug driver in `debugScript/` — the Python
script stays as an interactive debugger; this service is the production daemon.

## Features

- Reads occupancy + distance over UART (115200 baud binary protocol)
- Optionally reads a simple occupancy boolean from a GPIO pin, OR-combined with
  the UART signal so either interface can trigger "occupied"
- Configurable startup grace period (first event held for N seconds, then
  forced) and per-direction hysteresis on occupancy transitions; distance
  updates emit only during a committed/stable occupancy period
- Broadcasts `io.homeboard.Occupancy1.StateChanged(b occupied, u distance)` on
  the system D-Bus
- Auto-calibration at 03:00 each day (only fires once the room has been vacant
  long enough to be safe), or `--calibrate` to force on next idle period
- Applies per-gate thresholds and common parameters from a JSON config file

## Build & deploy

Cross-compiled for ARMv6 (Pi Zero) using `rpiz-xcompile`:

```sh
make xcompile-start         # mount sysroot
make install_sysroot_deps   # first time: fetch json-c, libsystemd, libcap .debs
make                        # build/occupancy-sensor
make deploy                 # scp binary to DEPLOY_TGT_HOST
make deploy-config          # scp config.json
make deploy-dbus-policy     # install D-Bus policy (required, one-time)
make dbus-listen            # tail StateChanged signals over SSH
```

## D-Bus interface

| | |
|---|---|
| Bus | system |
| Service | `io.homeboard.Occupancy` |
| Object | `/io/homeboard/Occupancy` |
| Interface | `io.homeboard.Occupancy1` |
| Signal | `StateChanged(b occupied, u distance)` |

Subscribe from any language with a D-Bus binding, or from the shell:

```sh
dbus-monitor --system "type='signal',interface='io.homeboard.Occupancy1'"
```

The D-Bus policy file (`io.homeboard.Occupancy.conf`) grants `own`
to the running user and broadcast/receive access to everyone else.

## Sensor protocol

Binary frame protocol at 115200 baud. Three frame types:

- **Command frames** — config/control exchanges:
  ```
  FD FC FB FA  [len_lo len_hi]  [cmd_lo cmd_hi]  [payload]  04 03 02 01
  ```
  Responses use `cmd + 0x0100` as the command word. e.g. `0x00FF` (enable config)
  → `0x01FF` (ack).
- **Calibration report frames** — progress pings during auto-calibration:
  ```
  F4 F3 F2 F1  [len_lo len_hi]  [payload]  F8 F7 F6 F5
  ```
- **Simple report frames** — continuous occupancy/distance, 5 bytes:
  ```
  6E  [state]  [dist_lo dist_hi]  62
  ```
  `state` 0/1 = vacant, 2/3 = occupied. `distance` is in cm.

### Example: enable config mode

Request:
```
FD FC FB FA  04 00  FF 00 01 00  04 03 02 01
             \----/ \--/  \--/
             len=4  cmd  protocol version
```

Response (`cmd + 0x0100 = 0x01FF`):
```
FD FC FB FA  08 00  FF 01 00 00 01 00 40 00  04 03 02 01
                    \--/ \-------/ \---/ \-/
                    cmd  status=OK  proto buf_size
```

### Example: occupancy report

```
6E 03 7C 00 62
   \/ \---/
   st  dist=124cm (occupied)
```

### Gates

The sensor splits its detection range into 16 zones (gates 0–15), ~75cm each,
up to ~12m. Every gate has independent thresholds:

- Gates 0–7 — "threshold" params: trigger + holding, commands `0x0072` / `0x0073`
- Gates 8–15 — "SNR" params: trigger + hold SNR, commands `0x0074` / `0x0075`

`farthest_gate` / `nearest_gate` in the common params clip the active window.

### Key command words

| Command | Word | Purpose |
|---|---|---|
| Enable config | `0x00FF` | must wrap parameter writes |
| End config | `0x00FE` | exit config mode |
| Read firmware | `0x0000` | firmware version |
| Read serial | `0x0011` | ASCII serial number |
| Write serial | `0x0010` | set serial (8 bytes ASCII) |
| Calibrate | `0x0009` | auto-threshold calibration |
| Read/write common | `0x0071` / `0x0070` | farthest_gate, unmanned_delay, etc. |
| Read/write threshold | `0x0073` / `0x0072` | per-gate trigger/holding (0–7) |
| Read/write SNR | `0x0075` / `0x0074` | per-gate SNR (8–15) |

### Calibration

Parameters:

- **trigger** — factor to enter "occupied". Higher = less sensitive.
- **retention** — factor to stay "occupied". Lower = stickier.
- **duration** — scan length in seconds.

The trigger/retention split provides hysteresis and kills flicker at range edges.
This service runs calibration at 03:00 daily — only after the UART has
reported ≥30 consecutive vacant samples, so it won't bake a human presence into
the new thresholds.

## References

- Manufacturer: https://www.hlktech.net/index.php?id=1176
- Protocol spec: https://drive.google.com/file/d/1LFyf6w9nOxW7b5z0rg5I3mPkk2KjQviE/view
- Sensor manual: https://drive.google.com/file/d/1RYpSp6NaCerTm2P-qLDfyDEjUKo1RAYG/view
- Python reference driver: `./debugScript/`
