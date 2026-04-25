# ambience

A C service that renders a slideshow of images onto a display using DRM/KMS
on a Raspberry Pi. The slideshow's lifecycle is driven by `presence-service`
(start when present, stop when not); ambience itself is the rendering layer
and exposes a D-Bus interface for manual control of the slideshow.

## What it does

- Acquires a DRM framebuffer via `display-mgr` and draws JPEGs directly into it.
- Subscribes to `io.homeboard.Presence1.PresenceChanged`. On `true` it starts
  the render loop and emits `SlideshowActive(true)`; on `false` it stops the
  loop and emits `SlideshowActive(false)`. Display power is not ambience's
  concern — `presence-service` owns `Display.On/Off`.
- Drives a slideshow off the dispatch event loop: an `sd_event` timer fires
  the next transition, an async `GetPhoto` returns via callback, the photo
  is decoded, scaled/rotated/aligned into the framebuffer, and the timer is
  re-armed. Single thread, no synchronization.
- Exposes `io.homeboard.Ambience1` for external triggers: `Next` / `Prev` to
  advance, `SetTransitionTimeSecs(u)` to retune the wait, `SetRenderConfig`
  to reconfigure rotation/interpolation/alignment at runtime, and `Announce`
  to overlay a transient message on the current photo.
- Optional e-ink metadata strip: when `use_eink_for_metadata` is true, photo
  metadata is rendered to a secondary e-ink panel via the `eink` lib.
- Emits `DisplayingPhoto(s)` after each successful render (carrying the
  opaque metadata blob from `photo-provider`) and `SlideshowActive(b)` on
  start/stop transitions.

## Dependencies

| Service | Role |
|---|---|
| `display-mgr` | Owns the DRM device. Ambience acquires its framebuffer through `drm_mgr`. Ambience does not call `Display.On/Off` — `presence-service` does. |
| `photo-provider` | Caches and serves JPEGs. Ambience calls `GetPhoto`/`GetPrevPhoto` for each transition, plus `SetTargetSize` / `SetEmbedQr` at startup. |
| `presence-service` | Emits `io.homeboard.Presence1.PresenceChanged(b)`. Ambience starts/stops the slideshow on this signal. |

All three must be reachable on the **system** bus.

## Build & deploy

Cross-compiled for ARMv6 using `rpiz-xcompile`:

```sh
make xcompile-start         # mount sysroot
make                        # build/ambience
make deploy-bin             # scp binary to DEPLOY_TGT_HOST
make deploy-config          # scp config.json
make deploy-dbus-policy     # install D-Bus policy (required, one-time)
make tgt-next               # trigger Ambience.Next() over SSH (for testing)
```

## Config

`config.json`:

```json
{
  "transition_time_s": 30,
  "rotation": 0,
  "interpolation": "bilinear",
  "horizontal_align": "center",
  "vertical_align": "center",
  "embed_qr": false,
  "use_eink_for_metadata": true,
  "fallback_image": "/home/batman/homeboard/stockimgs/starry-night.jpg"
}
```

- `transition_time_s` — seconds between pictures. Clamped ≥1, warning outside
  `(3, 300]`.
- `rotation` — one of `0`, `90`, `180`, `270`. Axes swap for 90/270 when
  requesting target size from `photo-provider` so the server renders at the
  correct aspect ratio.
- `interpolation` — `nearest` or `bilinear`. Default `bilinear`.
- `horizontal_align` — `left`, `center`, or `right`. How the scaled image is
  placed inside the framebuffer when it doesn't fill the width. Default
  `center`.
- `vertical_align` — `top`, `center`, or `bottom`. Same idea for the vertical
  axis. Default `center`.
- `embed_qr` — if true, `photo-provider` overlays a QR code on each image.
- `use_eink_for_metadata` — if true, photo metadata is rendered on the e-ink
  panel (via the shared `eink` lib).
- `fallback_image` — path to a JPEG drawn whenever fetch/decode fails so the
  screen never goes blank. Optional; ignored if not readable.

## D-Bus interface

| | |
|---|---|
| Bus | system |
| Service | `io.homeboard.Ambience` |
| Object | `/io/homeboard/Ambience` |
| Interface | `io.homeboard.Ambience1` |

Methods:

| Method | Signature | Effect |
|---|---|---|
| `Next` | `()` → `()` | Advance to the next picture immediately. |
| `Prev` | `()` → `()` | Step backward to the previous picture. |
| `SetTransitionTimeSecs` | `(u)` → `()` | Update the wait between pictures. Returns `InvalidArgs` if outside the supported range. |
| `SetRenderConfig` | `(usss)` → `()` | Update rotation / interpolation / horizontal-align / vertical-align at runtime. Args: `u` rotation (`0`/`90`/`180`/`270`), `s` interpolation (`nearest`/`bilinear`), `s` horizontal_align (`left`/`center`/`right`), `s` vertical_align (`top`/`center`/`bottom`). Re-renders the current picture in place. When rotation flips between portrait/landscape, the new target size is pushed to `photo-provider` so future photos arrive with the correct aspect ratio. Returns `InvalidArgs` if any field is unrecognized. |
| `Announce` | `(us)` → `()` | Overlay a string on top of the current picture for `u` seconds. Returns `InvalidArgs` if the timeout is invalid or an announcement is already active. |

Slideshow on/off control lives on `io.homeboard.Presence1.ForceOn` /
`ForceOff` — there are no equivalent methods on Ambience.

Signals:

| Signal | Signature | When |
|---|---|---|
| `DisplayingPhoto` | `s` (metadata string) | Emitted after each successful render, carrying the metadata blob `photo-provider` returned alongside the image. |
| `SlideshowActive` | `b` | Emitted on slideshow start/stop. `true` when a `PresenceChanged(true)` starts the loop; `false` when `PresenceChanged(false)` stops it (or the presence service drops off the bus). No signal is emitted on service shutdown. |

Both signals are emitted on the same bus connection (the one owning
`io.homeboard.Ambience`), so a match filter on
`sender=io.homeboard.Ambience` works for both — no NULL-sender quirk.

Shell invocation:

```sh
busctl --system call io.homeboard.Ambience /io/homeboard/Ambience io.homeboard.Ambience1 Next
busctl --system call io.homeboard.Ambience /io/homeboard/Ambience io.homeboard.Ambience1 \
    SetRenderConfig usss 180 bilinear center center
busctl --system call io.homeboard.Ambience /io/homeboard/Ambience io.homeboard.Ambience1 \
    Announce us 10 "HOLA MUNDO"
busctl --system monitor io.homeboard.Ambience     # watch DisplayingPhoto live
```

The policy file `io.homeboard.Ambience.conf` grants `own` to the running
user and send/receive access to everyone else. Without it,
`sd_bus_request_name` returns `EACCES`.

## Layer map

```
main.c                thin: creates sd_event, wires modules, runs
                      sd_event_loop. Owns no D-Bus connection itself.
config.{c,h}          json-c config parsing
dbus_helpers.{c,h}    is_service_up + NameOwnerChanged subscription helper
dbus_listeners.{c,h}  owns its own sd_bus (attached to sd_event) and the
                      well-known name io.homeboard.Ambience. Hosts the
                      Ambience1 vtable (Next / Prev / SetTransitionTimeSecs
                      / SetRenderConfig / Announce), subscribes to
                      Presence.PresenceChanged and presence/photo service
                      up/down, and emits SlideshowActive + DisplayingPhoto.
                      Single dispatch surface for the Ambience interface.
render_loop.{c,h}     framebuffer side of the slideshow. Drives an
                      sd_event one-shot timer for transitions; on fire
                      issues photo_client_fetch_async and renders in the
                      reply callback. JPEG decode + scale/rotate/align,
                      blit, fallback image, cached last-decoded image for
                      in-place re-render on render-config change. Touches
                      no D-Bus directly.
photo_client.{c,h}    outbound PhotoProvider client. Owns its own sd_bus
                      (attached to sd_event). Async GetPhoto/GetPrevPhoto
                      via sd_bus_call_method_async (single in-flight slot,
                      cancellable); synchronous SetTargetSize/SetEmbedQr.
overlay.{c,h}         transient on-screen announcements; supplies a draw
                      callback the render loop calls after img_render but
                      before the blit.
eink_meta.{c,h}       optional e-ink metadata strip; rendered after each
                      photo from the dispatch thread.
../lib/drm_mgr/                framebuffer acquisition via display-mgr
../lib/jpeg_render/            libjpeg-based decode + scaler
../lib/eink/                   cairo-based e-ink renderer
../lib/rpigpio/                GPIO wrapper used by eink lib
```

### Threading

Single-threaded by design. Everything runs on the `sd_event` dispatch loop:
D-Bus method handlers, signal listeners, the transition timer, the
`GetPhoto` reply callback, JPEG decode, the blit, and the eink_meta render.
No worker threads, no mutexes, no atomics (apart from a vestigial flag in
`overlay`).

The one CPU-bound step is JPEG decode + scale; on a Pi Zero a high-res
image can stall the loop for ~100ms. If that becomes user-visible, push
*just decode* off-thread (with a result queue back to dispatch) — but
that's a localized fix in `render_loop`, not a structural change.

### Startup ordering

`main.c` creates `sd_event`, registers `SIGINT`/`SIGTERM` via
`sd_event_add_signal`, loads config, then wires
`overlay → eink_meta → photo_client → render_loop → dbus_listeners` and
calls `sd_event_loop`. Each D-Bus-aware module opens and attaches its own
bus during init; main never holds an `sd_bus *`. Teardown frees the
modules in reverse before unref'ing the event loop.

## Build gotchas

- `json-c` is statically linked; `libsystemd`, `libjpeg`, `libcairo`,
  `libpixman`, `libpng`, etc. are linked dynamically (no static builds on
  Debian/Raspbian).
- The Makefile's `build/%.o` rule uses `$(notdir ...)`, so source files with
  the same basename across directories would collide.
- Cross-compile only: targets `arm-linux-gnueabihf`. `make
  install_sysroot_deps` pulls the needed `.deb`s into the rpiz-xcompile
  sysroot.
- If `display-mgr` goes away ambience can't acquire a framebuffer at startup
  and exits; once running, render keeps drawing into the existing
  framebuffer regardless.
