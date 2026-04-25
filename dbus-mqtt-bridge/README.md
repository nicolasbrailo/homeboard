# remote-control-svc

A bridge between the homeboard's D-Bus services and an MQTT broker. Exposes selected D-Bus methods as MQTT commands and republishes D-Bus signals as MQTT state updates, so an external client (phone, web page, home automation) can drive the device without talking to D-Bus directly.

## Why MQTT (and not a web server on the device)

The RPi Zero is constrained and the security goal is to keep the attack surface on the device as small as possible. This service:

- Opens **only outbound** TCP connections (to the broker). There is no listening socket on the device, so a LAN attacker cannot reach this process directly — they have to go through the broker.
- Keeps the message parser minimal (short, strict payloads; length-capped; no HTTP).
- Runs fine without TLS on a trusted LAN. The "no listening port on the device" property holds regardless.

If the broker or a web UI in front of it is compromised, the blast radius on the device is limited to whatever D-Bus methods this bridge explicitly forwards.

## What it does

- Connects to an MQTT broker as a client, with Last-Will-and-Testament set to `<prefix>state/bridge` = `"offline"` (retained).
- Subscribes to `<prefix>cmd/#`. Each known topic suffix maps to one D-Bus method call.
- Listens for `io.homeboard.Occupancy1.Report` on the system bus and republishes it as retained JSON on `<prefix>state/occupancy` (every second; retained).
- Listens for `io.homeboard.Ambience1.DisplayingPhoto` and republishes the raw metadata string as a retained message on `<prefix>state/displayed_photo`.
- Listens for `io.homeboard.Ambience1.SlideshowActive` and republishes `"true"` / `"false"` as a retained message on `<prefix>state/slideshow_active`.
- Auto-reconnects to both the broker and (implicitly) D-Bus on failure.
- Publishes `"online"` retained to `<prefix>state/bridge` on successful connect and `"offline"` on graceful shutdown.

This service is a **client** on every bus it touches: it doesn't own a D-Bus name, and it doesn't accept incoming TCP.

## Architecture

Single-threaded `poll()` event loop multiplexing two file descriptors:

- `sd_bus_get_fd()` — the system bus
- `mosquitto_socket()` — the MQTT TCP connection (may be `-1` while disconnected; omitted from the poll set in that case)

`poll` times out every 1 s so `mosquitto_loop_misc()` runs regularly for keepalive and reconnect. On wake, we drain `sd_bus_process` and call the appropriate `mosquitto_loop_read/write` depending on which fd signalled. All callbacks (MQTT message, D-Bus signal) fire from the main thread.

No worker threads. No `mosquitto_loop_start`. We own the loop explicitly because we already need to service D-Bus.

### Source files

| File | Purpose |
|------|---------|
| `main.c` | Entry point, event loop, command dispatch table, payload parsers |
| `config.c/h` | JSON config loader (json-c), defaults, `topic_prefix` validation |
| `mqtt.c/h` | libmosquitto wrapper: connect, LWT, subscribe/publish, non-blocking loop primitives |
| `dbus_client.c/h` | sd-bus client: method-call helpers for Ambience/Presence/PhotoProvider, signal subscribers for Occupancy and Ambience |
| `config.json` | Broker host/port, credentials, keepalive, topic prefix |

## MQTT interface

Default topic prefix `homeboard/` (configurable). Every topic below is relative to that prefix.

### Commands (bridge subscribes)

| Topic | D-Bus target | Payload | Notes |
|-------|--------------|---------|-------|
| `cmd/ambience/next` | `Ambience.Next` | ignored | advance slideshow |
| `cmd/ambience/prev` | `Ambience.Prev` | ignored | previous picture |
| `cmd/presence/force_on` | `Presence.ForceOn` | ignored | force presence=true |
| `cmd/presence/force_off` | `Presence.ForceOff` | ignored | latch presence=false until next genuine vacancy |
| `cmd/ambience/set_transition_time_secs` | `Ambience.SetTransitionTimeSecs` (`u`) | decimal string, e.g. `"30"` | |
| `cmd/photo_provider/set_embed_qr` | `PhotoProvider.SetEmbedQr` (`b`) | `"0"`/`"1"` or `"true"`/`"false"` (case-insensitive) | |
| `cmd/photo_provider/set_target_size` | `PhotoProvider.SetTargetSize` (`uu`) | `"<W>x<H>"`, e.g. `"1024x768"` | |

Unknown topics and malformed payloads are logged and dropped. Payloads have hard length caps in the parser.

### State (bridge publishes)

| Topic | Payload | Retained |
|-------|---------|----------|
| `state/bridge` | `"online"` / `"offline"` | yes (LWT sets `offline` on ungraceful disconnect) |
| `state/occupancy` | JSON `{"occupied":bool,"distance_cm":uint,"ts":unix_seconds}` | yes — late-joining clients get current state |
| `state/displayed_photo` | raw metadata string from `photo-provider` (opaque, not parsed) | yes — late-joining clients see the currently-displayed photo |
| `state/slideshow_active` | `"true"` or `"false"` | yes — reflects whether the ambience screen is currently on and the slideshow is running |

All publishes are QoS 0.

## D-Bus

No name is owned on the bus, so no `.conf` policy file is needed for this service. Each of the target services' existing `<policy context="default">` already permits `send_destination` and `receive_sender` for unprivileged callers, which is all the bridge needs.

The bridge talks to four services:

- `io.homeboard.Ambience` @ `/io/homeboard/Ambience` (interface `io.homeboard.Ambience1`) — method calls, plus signal subscriptions (`DisplayingPhoto s`, `SlideshowActive b`). Both subscriptions use `NULL` sender: `DisplayingPhoto` requires it (worker-thread bus doesn't own the well-known name — see the Ambience README), and `SlideshowActive` uses it for uniformity so the rule shape is robust if the emit side ever moves.
- `io.homeboard.Presence` @ `/io/homeboard/Presence` (interface `io.homeboard.Presence1`) — method calls (`ForceOn`, `ForceOff`)
- `io.homeboard.PhotoProvider` @ `/io/homeboard/PhotoProvider` (interface `io.homeboard.PhotoProvider1`) — method calls only
- `io.homeboard.Occupancy` @ `/io/homeboard/Occupancy` (interface `io.homeboard.Occupancy1`) — signal subscription (`Report bu`)

If a target service is down when a command arrives, `sd_bus_call_method` fails and the bridge logs the error; nothing crashes.

## Config

`config.json`:

```json
{
  "mqtt_host": "broker.lan",
  "mqtt_port": 1883,
  "mqtt_client_id": "homeboard-bridge",
  "mqtt_user": "",
  "mqtt_pass": "",
  "mqtt_keepalive_s": 30,
  "topic_prefix": "homeboard/"
}
```

`topic_prefix` must end with `/`. Empty `mqtt_user` disables authentication.

## Build

Requires in the cross-compile sysroot: `libmosquitto-dev`, `libssl1.1` (libmosquitto links against `libssl/libcrypto.so.1.1` even when TLS is unused), and `libjson-c-dev` (already present from other services).

```
make install_sysroot_deps   # fetches libmosquitto1, libmosquitto-dev, libssl1.1
make
make deploy
```

Run on target:

```
./remote-control-svc config.json
```

## Operational notes

- **No TLS, no on-device auth.** Trust boundary is the LAN. Broker-side ACLs (if any) are the only access control.
- **Reconnect behaviour.** libmosquitto retries with backoff 2s→30s. On a dead broker, the poll loop still services D-Bus — occupancy signals are still received, just not forwarded until MQTT is back.
- **Retained occupancy.** After a bridge restart, the last-known occupancy state stays available to new subscribers via the retained message, but the `ts` field will be stale until the next sensor event. Consumers should treat `ts` as authoritative for freshness.
- **Ordering.** QoS 0 means fire-and-forget — the bridge does not guarantee delivery of either commands or state. This is fine for a remote control; do not build safety-critical workflows on top of it.
