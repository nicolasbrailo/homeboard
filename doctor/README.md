# homeboard-doctor

A host + network health monitor and self-healer for the homeboard device. It
exists because the device (a Pi Zero 2 W on a USB Ethernet adapter) periodically
loses connectivity when the `r8152` USB NIC's receive path wedges — carrier
stays up, but no packets come in and the box drops off the network until it's
power-cycled by hand. The doctor detects that condition, recovers it by bouncing
the USB device, tells you on-screen, and publishes health telemetry.

Unlike the other services this one is a **bash script**, and it runs as **root**
(it must write `/sys/bus/usb/.../{unbind,bind}` to bounce the NIC).

## What it does

Every `poll_interval_s` (default 30s):

1. **Probes network health**
   - default gateway reachable (`ping`)
   - photo-provider reachable (`server_url` from `homeboard-photo-provider.json`)
   - MQTT broker reachable (TCP connect to host/port from
     `homeboard-dbus-mqtt-bridge.json`)
2. **Samples host metrics** — uptime, CPU load, available memory, SoC
   temperature, `vcgencmd get_throttled` (under-voltage / throttling bits), and
   the `eth0` carrier / speed / rx-tx packet & error counters.
3. **Detects the RX-wedge** — gateway unreachable *while* carrier is up and
   `rx_packets` is frozen since the last sample. This is the exact signature of
   the failure this device suffers.
4. **Shows an on-screen error** via the ambience `Announce` D-Bus method
   (`io.homeboard.Ambience1.Announce (us)`) while unhealthy, and clears it on
   recovery.
5. **Recovers a stuck adapter** — after `gateway_fail_threshold` consecutive
   gateway failures it bounces the USB device (`unbind` → `bind`), rate-limited
   by `recover_cooldown_s`. Successful recoveries are counted and persisted.
6. **Publishes a snapshot** to `<topic_prefix>doctor` on MQTT every cycle.

A gateway-reachable-but-service-down condition (photo/MQTT) is reported on
screen and in telemetry, but does **not** trigger a USB bounce — that's a remote
problem, not a stuck NIC.

## Config

Deployed to `etc/homeboard-doctor.json`:

| Key | Meaning |
| --- | --- |
| `poll_interval_s` | Seconds between health cycles |
| `net_iface` | Interface to watch (default `eth0`) |
| `usb_device` | USB device id to bounce for recovery (default `1-1`) |
| `gateway_fail_threshold` | Consecutive gateway-ping failures before recovery |
| `recover_cooldown_s` | Minimum seconds between USB bounces |
| `announce_timeout_s` | Timeout requested on each `Announce`. A safety net only: if the doctor dies, a stale on-screen message self-clears after this long instead of sticking forever |
| `announce_refresh_s` | Re-arm the on-screen message this often while the error persists. Must be `< announce_timeout_s` (auto-clamped to half if not) so the message never actually expires on screen |
| `photo_provider_config` | Path to the photo-provider config (for `server_url`) |
| `mqtt_config` | Path to the mqtt-bridge config (broker + topic prefix) |
| `state_file` | Where the recovery counter is persisted across restarts |

## MQTT telemetry — `<topic_prefix>doctor`

```json
{
  "ts": "2026-07-23T09:00:00+01:00",
  "host": "BananaPizza",
  "state": "ok",            // ok | degraded | net_down
  "uptime_s": 138129,
  "temp_c": 47.2,
  "load1": 0.09,
  "mem_avail_kb": 275368,
  "throttled": "0x0",
  "iface": "eth0",
  "operstate": "up",
  "carrier": 1,
  "speed": 100,
  "rx_packets": 605250,
  "tx_packets": 334893,
  "rx_errors": 0,
  "tx_errors": 0,
  "gateway_ok": true,
  "photo_ok": true,
  "mqtt_ok": true,
  "rx_wedged": false,
  "gw_fails": 0,
  "net_recoveries": 3,
  "last_recovery_iso": "2026-07-23T02:56:26+01:00"
}
```

The snapshot is best-effort: during an outage the broker is unreachable, so
nothing is published until connectivity is back — at which point `net_recoveries`
reflects any bounces that happened while offline.

## Runtime deps

`jq`, `mosquitto-clients` (mosquitto_pub), `busctl`, `nmcli`/`curl`/`ping`,
`vcgencmd`. Install the two that aren't in the base image with `make install-deps`.

## Deploy

```sh
make -C doctor install-deps      # once: jq + mosquitto-clients on target
make -C doctor deploy-bin
make -C doctor deploy-config
make -C doctor install-systemd   # installs the root unit, enables + starts it
```

Or via the top-level targets (`make deploy-all`, `make install-systemd`), which
now include `doctor`.

## Notes / limitations

- **Runs as root** by design (USB rebind). It's a small bash script; read it
  before trusting it with that.
- The USB bounce briefly drops the network — expected. The doctor is a local
  service, so it survives its own recovery action; any SSH session over `eth0`
  will not.
- On-screen text uses the ambience `Announce` method (cairo/freetype text over a
  dimmed scrim). This is the HDMI/photo display path; the eink path is separate.
