# ambience

A C service that renders a slideshow of images onto a display using DRM/KMS on
a Raspberry Pi. It reacts to room occupancy (starting/stopping the slideshow
as people come and go), fetches photos from a sibling service, and exposes a
D-Bus interface for manual control.

## What it does

- Acquires a DRM framebuffer via `display-mgr` and draws JPEGs directly into it.
- Subscribes to occupancy signals from `occupancy-sensor`; when the room is
  occupied it calls `display-mgr` to turn the panel on and starts the
  slideshow; when vacant it does the inverse.
- Runs a background worker that pulls photos from `photo-provider` and renders
  them at the configured interval with optional rotation and QR-code overlay.
- Exposes `io.homeboard.Ambience1` for external triggers (buttons,
  automations): `Next()` to skip to the next picture, `Prev()` for previous, and
  `ForceSlideshowOn()` / `ForceSlideshowOff()` to override the occupancy
  gate until the next real occupancy report arrives.
- Transition time between pictures can be configured over dbus with `SetTransitionTimeSecs`
- Emits a `DisplayingPhoto(s)` signal on the same interface every time a new
  photo is rendered; the payload is the opaque metadata string supplied by
  `photo-provider`.
- Emits a `SlideshowActive(b)` signal when the screen transitions on/off
  (occupancy-driven or forced); `true` means the slideshow is now running and
  the display is on, `false` means it has stopped.

## Dependencies

Ambience is the presentation layer and relies on three other services:

| Service | Role |
|---|---|
| `display-mgr` | Owns the DRM device; ambience gets its framebuffer through it and calls `io.homeboard.Display1.On/Off` to control panel power. |
| `photo-provider` | Caches and serves JPEGs; ambience calls `io.homeboard.PhotoProvider1.GetPhoto` for each transition, plus `SetTargetSize` / `SetEmbedQr` at startup. |
| `occupancy-sensor` | Emits `io.homeboard.Occupancy1.StateChanged`; ambience uses it to decide when to run. |

All three must be reachable on the **system** bus.

## Features

- DRM/KMS rendering via the shared `drm_mgr` library
- JPEG decode and scale (bilinear) to the framebuffer, with 0/90/180/270°
  rotation
- Occupancy-gated lifecycle: slideshow only runs when someone is in the room
- Manual override: `ForceSlideshowOn` acts like a synthetic `occupied=true`
  (any real report then wins). `ForceSlideshowOff` latches the display off
  and ignores `occupied=true` reports until a real `occupied=false` releases
  the latch — needed because the occupancy sensor re-publishes on distance
  changes, not just state transitions.
- Worker can be interrupted mid-wait for immediate advance (`Next`)
- Single sd_bus event loop shared between all D-Bus consumers

## Build & deploy

Cross-compiled for ARMv6 using `rpiz-xcompile`:

```sh
make xcompile-start         # mount sysroot
make                        # build/ambience
make deploy                 # scp binary to DEPLOY_TGT_HOST
make deploy-config          # scp config.json
make deploy-dbus-policy     # install D-Bus policy (required, one-time)
make next                   # trigger Ambience.Next() over SSH (for testing)
```

## Config

`config.json`:

```json
{
  "transition_time_s": 30,
  "rotation": 0,
  "embed_qr": false
}
```

- `transition_time_s` — seconds between pictures. Clamped ≥1, warning outside
  `(3, 300]`.
- `rotation` — one of `0`, `90`, `180`, `270`. Axes swap for 90/270 when
  requesting target size from `photo-provider` so the server renders at the
  correct aspect ratio.
- `embed_qr` — if true, `photo-provider` overlays a QR code on each image.

## D-Bus interface

| | |
|---|---|
| Bus | system |
| Service | `io.homeboard.Ambience` |
| Object | `/io/homeboard/Ambience` |
| Interface | `io.homeboard.Ambience1` |

Methods (all take no arguments, return nothing):

| Method | Effect |
|---|---|
| `Next()` | Advance to the next picture immediately |
| `ForceSlideshowOn()` | Turn the display on and start the slideshow as if `occupied=true` had been reported. Cleared by the next real occupancy report. |
| `ForceSlideshowOff()` | Turn the display off and stop the slideshow. Latched: subsequent `occupied=true` reports are ignored until a real `occupied=false` report (or the occupancy service dropping out) releases it. After release, normal behavior resumes — the next `occupied=true` turns the display back on. `ForceSlideshowOn()` also releases the latch. |

Signals:

| Signal | Signature | When |
|---|---|---|
| `DisplayingPhoto` | `s` (metadata string) | Emitted from the slideshow worker after each successful render, carrying the metadata blob `photo-provider` returned alongside the image. |
| `SlideshowActive` | `b` | Emitted from the main dispatch thread on screen on/off transitions. `true` when the display is turned on and the slideshow starts; `false` when it is turned off and the slideshow stops. No signal is emitted on service shutdown. |

Subscriber caveat: `DisplayingPhoto` is emitted from the slideshow worker's
private bus connection, whose unique name does **not** own
`io.homeboard.Ambience` (the main dispatch bus does). dbus-daemon resolves a
well-known sender in a match rule to the current owner's unique name at
install time, so a match filtering on `sender=io.homeboard.Ambience` will
silently drop these signals. Subscribers must pass `NULL` (no sender filter)
to `sd_bus_match_signal` and identify the signal by path + interface + member.
`SlideshowActive` is emitted from the name-owning connection and is not
subject to this quirk, but subscribers are encouraged to use `NULL` sender
uniformly for both signals.

Shell invocation:

```sh
busctl --system call io.homeboard.Ambience /io/homeboard/Ambience io.homeboard.Ambience1 Next
busctl --system call io.homeboard.Ambience /io/homeboard/Ambience io.homeboard.Ambience1 ForceSlideshowOn
busctl --system call io.homeboard.Ambience /io/homeboard/Ambience io.homeboard.Ambience1 ForceSlideshowOff
busctl --system monitor io.homeboard.Ambience     # watch DisplayingPhoto live
```

The policy file `io.homeboard.Ambience.conf` grants `own` to the running user
and send/receive access to everyone else. Without it,
`sd_bus_request_name` returns `EACCES`.

## Layer map

```
main.c             thin: opens the shared sd_bus, loads config, wires modules,
                   runs the dispatch loop
config.{c,h}       json-c config parsing
dbus.{c,h}         borrows the shared sd_bus; hosts io.homeboard.Ambience1
                   (Next, ForceSlideshowOn, ForceSlideshowOff)
display.{c,h}      borrows the shared sd_bus; subscribes to
                   Occupancy1.StateChanged, calls Display1.On/Off, invokes
                   caller callbacks on toggle. Exposes display_force_on /
                   display_force_off for manual overrides that synthesise an
                   occupancy report.
slideshow.{c,h}    borrows the shared sd_bus for service up/down monitoring
                   and startup config. Worker thread: fetches from
                   PhotoProvider, decodes, renders. Holds a dedicated
                   sd_bus connection for GetPhoto so the synchronous call
                   never touches the dispatch bus from another thread.
../lib/drm_mgr/              framebuffer acquisition via display-mgr
../lib/jpeg_render/          libjpeg-based decode + scaler
```

### Threading

- Main thread runs a single `sd_bus_process` / `sd_bus_wait` loop via
  `ambience_dbus_run_once`. `dbus.c` (server vtable), `display.c` (Occupancy
  match + Display1.On/Off calls), and `slideshow.c` (PhotoProvider
  name-owner match + initial `SetTargetSize`/`SetEmbedQr`) all share that
  bus. `Next`, `ForceSlideshowOn`, `ForceSlideshowOff`, and the occupancy
  signal handler all dispatch on this thread.
- Slideshow worker thread: fetches one photo (`GetPhoto`), decodes, renders,
  then waits on `wake_sem` up to `transition_time_s`. `slideshow_stop()` and
  `slideshow_next()` both post the sem; `stop_requested` (plain bool,
  synchronised by the sem) disambiguates. The worker uses a dedicated
  `sd_bus` connection (`worker_bus`) so its synchronous `GetPhoto` calls
  never touch the dispatch bus from another thread — `sd_bus` is not
  thread-safe.

### Startup ordering

`main.c` opens the shared `sd_bus` up front and passes it as a borrowed
pointer to `slideshow_init`, `display_init`, and `ambience_dbus_init`. The
three modules can be created in any order; they keep only a borrowed
reference. Teardown frees each module (unrefs its slots) before `main.c`
calls `sd_bus_flush_close_unref` on the shared bus.

## Build gotchas

- `json-c` is statically linked; `libsystemd` and `libjpeg` are dynamic.
- The Makefile's `build/%.o` rule uses `$(notdir ...)`, so source files with
  the same basename across directories would collide.
- Cross-compile only: targets `arm-linux-gnueabihf`.
- If display-mgr goes away, ambience will break (can't render but also won't crash)
