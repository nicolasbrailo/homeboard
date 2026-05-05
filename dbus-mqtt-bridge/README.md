# remote-control-svc

A bridge between the homeboard's D-Bus services and an MQTT broker. Exposes selected D-Bus methods as MQTT commands and republishes D-Bus signals as MQTT state updates, so an external client (phone, web page, home automation) can drive the device without talking to D-Bus directly.

## Why MQTT (and not a web server on the device)

The RPi Zero is constrained and the security goal is to keep the attack surface on the device as small as possible. This service:

- Opens **only outbound** TCP connections (to the broker). There is no listening socket on the device, so a LAN attacker cannot reach this process directly — they have to go through the broker.
- Keeps the wire format strict (JSON objects, length-capped, no HTTP). json-c is already linked in for config parsing, so the only added cost is the per-command parse.
- Runs fine without TLS on a trusted LAN. The "no listening port on the device" property holds regardless.

If the broker or a web UI in front of it is compromised, the blast radius on the device is limited to whatever D-Bus methods this bridge explicitly forwards.

## What it does

- Connects to an MQTT broker as a client, with Last-Will-and-Testament set to `<prefix>state/bridge` = `{"state":"offline", machine_id, hostname, ip, host_model, ...}` (retained). The LWT payload is pre-formatted from host info collected at startup, so an ungraceful disconnect leaves a useful last-known record on the broker rather than an anonymous "offline" marker.
- Subscribes to `<prefix>cmd/#`. Each known topic suffix maps to one D-Bus method call. Payloads are JSON objects (see table below).
- Listens for `io.homeboard.Occupancy1.Report` on the system bus and republishes it as retained JSON on `<prefix>state/occupancy`.
- Listens for `io.homeboard.Ambience1.DisplayingPhoto` and republishes the photo-provider metadata JSON verbatim on `<prefix>state/displayed_photo` (passthrough, not re-wrapped).
- Listens for `io.homeboard.Ambience1.SlideshowActive` and republishes `{"active":true}` / `{"active":false}` retained on `<prefix>state/slideshow_active`.
- Auto-reconnects to both the broker and (implicitly) D-Bus on failure.
- Publishes the online variant of the same payload retained to `<prefix>state/bridge` on successful connect, and the offline variant on graceful shutdown — so the topic always carries a single record per device, with `state` toggling between `online` and `offline`. IP and other host info are captured at startup and do not refresh; a process restart is required to pick up changes.

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
| `main.c` | Entry point, event loop, command dispatch table, JSON payload parsers (json-c) |
| `config.c/h` | JSON config loader (json-c), defaults, `topic_prefix` validation |
| `mqtt.c/h` | libmosquitto wrapper: connect, LWT, subscribe/publish, non-blocking loop primitives |
| `dbus_client.c/h` | sd-bus client: method-call helpers for Ambience/Presence/PhotoProvider, signal subscribers for Occupancy and Ambience |
| `config.json` | Broker host/port, credentials, keepalive, topic prefix |

## MQTT interface

Default topic prefix `homeboard/` (configurable). Every topic below is relative to that prefix.

### Commands (bridge subscribes)

All command payloads are **JSON objects**. Field names and types must match exactly; missing or wrong-type fields cause the command to be logged and dropped. Void commands ignore their payload entirely (any value, including empty, is fine).

| Topic | D-Bus target | Payload | Notes |
|-------|--------------|---------|-------|
| `cmd/ambience/next` | `Ambience.Next` | ignored | advance slideshow |
| `cmd/ambience/prev` | `Ambience.Prev` | ignored | previous picture |
| `cmd/presence/force_on` | `Presence.ForceOn` | ignored | force presence=true |
| `cmd/presence/force_off` | `Presence.ForceOff` | ignored | latch presence=false until next genuine vacancy |
| `cmd/ambience/set_transition_time_secs` | `Ambience.SetTransitionTimeSecs` (`u`) | `{"secs": <uint>}` | e.g. `{"secs":30}` |
| `cmd/ambience/announce` | `Ambience.Announce` (`us`) | `{"timeout": <uint>, "msg": <string>}` | timeout `0` means no auto-clear |
| `cmd/ambience/set_svg_overlay` | `Ambience.SetSvgOverlay` (`us`) | `{"timeout": <uint>, "svg": <string>}` | timeout `0` means no auto-clear. SVG string capped at 128 KB; total payload capped at 192 KB. nanosvg is the parser — no `<text>`, no filters, limited gradients. Empty `svg` clears the overlay |
| `cmd/ambience/set_render_config` | `Ambience.SetRenderConfig` (`usss`) | `{"rotation": <uint>, "interp": <string>, "h_align": <string>, "v_align": <string>}` | rotation `0`/`90`/`180`/`270`; interp `nearest`/`bilinear`; aligns as on the D-Bus method. Validation happens service-side |
| `cmd/photo_provider/set_embed_qr` | `PhotoProvider.SetEmbedQr` (`b`) | `{"on": <bool>}` | |
| `cmd/photo_provider/set_target_size` | `PhotoProvider.SetTargetSize` (`uu`) | `{"w": <uint>, "h": <uint>}` | each in 1..10000 |

Unknown topics and malformed payloads are logged and dropped. The whole MQTT payload is hard-capped at 192 KB before JSON parsing.

### State (bridge publishes)

All state payloads are JSON. `state/displayed_photo` is a passthrough of `photo-provider`'s metadata blob — itself JSON, but its schema is owned by `photo-provider`, not the bridge.

| Topic | Payload | Retained |
|-------|---------|----------|
| `state/bridge` | `{"state":"online"\|"offline","machine_id":...,"hostname":...,"ip":...,"host_model":...,"started_at":...,"started_at_iso":...,"rotation":<uint>,"interp":...,"h_align":...,"v_align":...}` | yes (LWT pre-formatted with the offline variant; broker publishes it on ungraceful disconnect, retaining a last-known record for stale-device cleanup). The `rotation`/`interp`/`h_align`/`v_align` fields mirror the current Ambience render config; they default at startup and can be updated live via `rc_mqtt_claim_set_render_cfg`. The LWT carries the startup defaults — a crash after a render_cfg change will deliver stale render fields in the offline payload |
| `state/occupancy` | `{"occupied":<bool>,"distance_cm":<uint>,"ts":<unix_seconds>}` | yes — late-joining clients get current state |
| `state/displayed_photo` | photo-provider's metadata JSON, passed through verbatim | yes — late-joining clients see the currently-displayed photo |
| `state/slideshow_active` | `{"active":<bool>}` | yes — reflects whether the ambience screen is currently on and the slideshow is running |

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
