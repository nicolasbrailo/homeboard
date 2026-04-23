# CLAUDE.md

C implementation of the HLK-LD2410S mmWave presence-sensor driver, wrapped as a
systemd-deployable daemon that publishes occupancy state on the system D-Bus.

See `README.md` for feature list, config format, and protocol reference — the
protocol details there (frame layouts, command words, gate model, calibration
semantics) are authoritative and not repeated here.

## Layer map

```
main.c            thin: json config, dbus init/emit, calibration schedule
ld2410s.{c,h}     combined sensor: owns UART + GPIO, poller thread,
                  state-change callback, is_room_empty(), set_param(),
                  start_calibration()
ld2410s_uart/
  ld2410s_uart.{c,h}   low-level UART driver (public API prefixed ld2410s_uart_*)
  session.{c,h}        command queue + config-mode wrap + send/wait/ack
  transport.{c,h}      framing, reader thread, frame-type dispatch
../lib/rpigpio/        GPIO read wrapper (shared library)
```

The combined `LD2410S` class is what `main.c` talks to. It runs its own poller
thread that merges GPIO polling with the latest UART-reported state (OR on
`occupied`) and fires the user-supplied callback **only on state change**. UART
reports arrive via `on_uart_report` from the transport's reader thread and land
in atomics; the poller reads them. There is no direct callback from UART to the
user — everything funnels through the poller so GPIO and UART changes are
reported uniformly.

## Threading

- Transport reader thread: reads serial, dispatches frames to session/report
  decoders. Updates atomics in the wrapper.
- Session: blocks the caller on command send, matched by response command word
  (`request + 0x0100`). ~6s timeout.
- Combined-class poller thread: 1Hz loop, reads GPIO + atomics, emits on change.
- Main thread: just runs the calibration scheduler and blocks on signal.

## D-Bus

- Static link of libsystemd is not supported on Debian; Makefile uses
  `-Wl,-Bstatic` for json-c and `-Wl,-Bdynamic` for libsystemd.
- Owning a name on the **system** bus requires a policy file. It lives at
  `io.homeboard.Occupancy.conf` in this repo and is installed with
  `make deploy-dbus-policy`. Without it, `sd_bus_request_name` returns `EACCES`.
- The policy's `<policy user="...">` must match the service's runtime user.

## Calibration policy

- 03:00 daily, plus `--calibrate` to force on next idle window.
- Gated by `ld2410s_is_room_empty()` — wraps `ld2410s_uart_get_vacant_reports_count() >= 30`.
- Threshold lives as a define in `ld2410s.c`.
- Calibration only runs when UART is enabled (GPIO-only mode cannot calibrate).

## Build gotchas

- The Makefile's `build/%.o` rule uses `$(notdir ...)`, so two source files
  with the same basename collide. That's why the UART driver is
  `ld2410s_uart/ld2410s_uart.c`, not `ld2410s_uart/ld2410s.c`.
- Cross-compile only: the binary targets `arm-linux-gnueabihf` (Pi Zero /
  ARMv6). `make install_sysroot_deps` pulls the needed `.deb`s into the
  rpiz-xcompile sysroot; URLs can drift — check the Raspbian pool listing if a
  fetch 404s.
- `json-c` is statically linked; `libsystemd` is linked dynamically (no static
  build ships on Debian/Raspbian).

## Related

- `debugScript/` — Python reference driver + interactive file-based command
  shell. Use it to experiment with the sensor before changing C code.
- `debugScript/CLAUDE.md` — more protocol depth from the Python side
  (config-mode wrapping, dispatch table, `rfind`-based frame detection).
