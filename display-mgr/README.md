# display-mgr

A simple DRM display management daemon for the homeboard. It owns the DRM master, manages display power state, and grants exclusive framebuffer access to a single client over the system D-Bus.

## What it does

- Opens `/dev/dri/card0` (or `card1`), takes DRM master, and creates a dumb framebuffer
- Allocates a DMA-BUF for the framebuffer that can be shared with clients
- Owns `io.homeboard.Display` on the system bus and exposes the `io.homeboard.Display1` interface at `/io/homeboard/Display`
- Manages display on/off state
- Grants exclusive framebuffer leases to one client at a time, tracked by D-Bus unique name
- Releases the lease automatically when the holder disconnects from the bus (via `sd_bus_track`)
- On shutdown, restores the display to whatever power state it was in at startup

## Architecture

Single-threaded sd-bus event loop: `sd_bus_process` + `sd_bus_wait` with a 1 s idle timeout. All method handlers and the lease-tracking callback run on the main thread; there is no explicit liveness polling — the bus daemon notifies us when a peer disappears.

### Source files

| File | Purpose |
|------|---------|
| `main.c` | Entry point, sd-bus event loop, shutdown sequencing |
| `drm.c` | DRM device init, framebuffer creation, display on/off via `drmModeSetCrtc` |
| `dbus.c` | D-Bus object vtable, lease tracking via `sd_bus_track`, `StateChanged` signal |

## D-Bus interface

Service: `io.homeboard.Display`
Path: `/io/homeboard/Display`
Interface: `io.homeboard.Display1`

### Methods

| Method | Signature | Description |
|--------|-----------|-------------|
| `On` | `()` → `()` | Turn the display on |
| `Off` | `()` → `()` | Turn the display off |
| `Status` | `()` → `s` | Returns `"on"` or `"off"` |
| `AcquireLease` | `()` → `huuuuu` | Returns `(fd, width, height, stride, format, size)`. `format` is `DRM_FORMAT_XRGB8888`. Fails with `io.homeboard.Display.Error.LeaseHeld` if another client holds the lease. |
| `ReleaseLease` | `()` → `()` | Release the lease. Fails with `io.homeboard.Display.Error.NotLeaseHolder` if the caller does not hold it. |

### Signals

| Signal | Signature | Description |
|--------|-----------|-------------|
| `StateChanged` | `s` | Fired when the display power state changes. Payload is `"on"` or `"off"`. |

### Lease semantics

Only one client may hold the lease at a time. The daemon tracks the holder by the caller's unique bus name (e.g. `:1.42`) using `sd_bus_track_add_sender`. When that name drops off the bus — normal disconnect, process exit, or crash — the lease clears automatically on the next bus event. PID is still queried via `sd_bus_creds_get_pid()` for logging.

The returned file descriptor is a DMA-BUF; the client `mmap()`s it to render directly into the framebuffer.

## D-Bus policy

Owning a name on the **system** bus requires a policy file. `io.homeboard.Display.conf` lives in this repo and is installed with:

```
make deploy-dbus-policy
```

Without it, `sd_bus_request_name` returns `EACCES`. The policy's `<policy user="...">` must match the service's runtime user.

## Build

Requires `libdrm-dev` and `libsystemd-dev` in the cross-compile sysroot:

```
make install_sysroot_deps
make
```

`libdrm` is statically linked; `libsystemd` is dynamic (no static build ships on Debian/Raspbian).

## Permissions

The daemon needs DRM master access:

```
make set_perms   # adds user to video group, sets cap_sys_admin
```
