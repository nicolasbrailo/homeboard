# photo-provider

Bridges a remote [wwwslide](https://github.com/nicolasbrailo/wwwslide) HTTP photo server with local D-Bus consumers. Registers as a client with the server, pre-fetches a small ring of JPEGs into `memfd`s, and hands them out one at a time over D-Bus (passing the fd directly). The ring also keeps a bounded history of already-delivered photos so consumers can step backwards. Target size and QR-embed flag can be changed at runtime over D-Bus; any change triggers a fresh server-side registration and flushes the cache.

```
photo-provider <config.json>
```

Runs until `SIGTERM` / `SIGINT`. Intended to run as a systemd service.

## D-Bus interface

Service `io.homeboard.PhotoProvider`, object `/io/homeboard/PhotoProvider`, interface `io.homeboard.PhotoProvider1`:

| Method | Signature | Effect |
|--------|-----------|--------|
| `GetPhoto` | `() → (h, s)` | Advances the cursor and returns `(fd, metadata_json)` for the next photo. Fd is a `memfd`; caller closes. Blocks up to 5s if prefetch is empty, then fails with `Unavailable`. |
| `GetPrevPhoto` | `() → (h, s)` | Retreats the cursor one step and returns the previous photo from the history window. Non-blocking; fails with `Unavailable` if no history is held. |
| `SetTargetSize` | `(uu) → ()` | Updates `(w, h)` and flushes the cache; the worker re-registers with the server before its next fetch. Returns immediately, never touches the network. Fails with `InvalidArgs` outside 128..3840. |
| `SetEmbedQr` | `(b) → ()` | Toggles QR embed; same re-register + flush behavior as `SetTargetSize`. |

Every method answers promptly, even while the server is unreachable: `GetPhoto` is the only one that waits, and its 5s bound is well under client call timeouts. Errors are named `io.homeboard.PhotoProvider.Error.*`.

## Config

JSON file; see `config.json` for an example.

| Key | Meaning |
|-----|---------|
| `server_url` | wwwslide base URL (no trailing slash) |
| `target_size_w` / `target_size_h` | Requested render size (128..3840) |
| `embed_qr` | Ask server to burn a QR code into the image |
| `cache_depth` | Worker keeps this many photos pre-fetched ahead of the cursor |
| `history_depth` | Photos behind the cursor retained for `GetPrevPhoto` (0 disables `prev`) |
| `dump_to_disk` / `dump_dir` | If true, also write each fetched image as `N.jpeg` into `dump_dir` |
| `connect_timeout_s` / `request_timeout_s` | libcurl timeouts for all HTTP calls |

## Install

```
sudo make install_sysroot_deps   # json-c, libsystemd, libcurl + openldap
make
make deploy
make deploy-config
make deploy-dbus-policy          # one-time; reloads dbus
```

## Source files

| File | Purpose |
|------|---------|
| `main.c` | Entry point, lifecycle wiring |
| `config.c` | json-c config loader |
| `www_session.c` | libcurl HTTP client; owns `client_id`, target size, embed_qr; handles (re-)registration |
| `cache.c` | Worker thread + cursor-indexed ring (history · current · prefetch); generation counter for invalidation |
| `dbus.c` | sd-bus vtable for the methods above |

---

## Notes for LLMs working in this directory

**Architecture: three layers, bottom-up ownership.**

- `www_session` is the lowest layer: pure HTTP, no threads of its own. Owns one `CURL *` handle, the `client_id`, and the config. **All network IO runs on the cache worker thread** (inside `pp_www_session_fetch_next`), so the handle and `client_id` are private to that thread and need no locking.
- `cache` sits above `www_session`. It owns the worker thread, the ring, a mutex+condvar, and a generation counter. It borrows the `www_session *` (does not own it).
- `dbus` sits above both. Method handlers call into `cache` (for `GetPhoto` / `GetPrevPhoto`, and to invalidate after a config change) or `www_session` (for setters). The dbus layer owns nothing but the sd-bus slot/connection. **Dispatch is single-threaded, so a handler must never block on the network**: one slow handler delays every caller, and a caller whose call times out leaves a late reply on its connection. This is why setters don't do HTTP and `GetPhoto`'s wait is bounded to 5s.

**Cache model: single ring + cursor.** The cache is a single `buf_cap = history_depth + 1 + cache_depth` ring indexed by monotonically-increasing logical positions (mod capacity). Three indices split it: `[head, cursor)` is history, `cursor` is the photo currently delivered (`-1` before the first delivery), `(cursor, tail)` is prefetched-but-not-yet-delivered. `GetPhoto` advances `cursor`; `GetPrevPhoto` retreats it. The worker fills from `tail`, and evicts `head` on overflow — but only when `head < cursor`, so it can never drop the photo the consumer is currently viewing. "prev then next" walks back over the same slot, so it returns the exact same photo. Going past the held history returns `Unavailable`.

**Registration is lazy, driven by a flag.** `needs_register` (atomic) starts `true` and is set again by setters that change a value and by `fetch_next` when the client has been idle past the server's eviction window. `fetch_next` does `atomic_exchange(needs_register, false)` and, if it was set, runs `/client_register` + config pushes before fetching. On failure it sets the flag again and returns -1, so the worker keeps retrying (every ~2s plus timeouts) until the server is reachable. Consequences: the service starts and answers D-Bus even if the server is down at boot, and a config change is never lost to a failed registration. The flag is cleared *before* `reregister` reads the config, so a setter racing with a registration sets it again and gets its own registration with its new values.

**Invalidation via generation counter.** When a setter reports a change (returns 1), `dbus.c` calls `pp_cache_invalidate`, which bumps a generation under the cache mutex and drains the ring. The setter stores the new value and sets `needs_register` *before* that, so a worker that observes the new generation also observes the flag. The worker captures `gen` before each fetch and, on push, discards the result if the current gen no longer matches — this is the only mechanism that protects against "worker fetched with the old config and finished after the change." A stale-client re-register does not invalidate: photos already held are still valid.

**Atomics for the mutable config.** `target_w`, `target_h`, `embed_qr` in `www_session` are `atomic_uint` / `atomic_bool`. Setters use `atomic_exchange` to detect no-op writes. No mutex needed because these are only *read* inside `reregister`, where the new register call defines ordering against the server; there is no invariant between `target_w` and `target_h` that an intermediate read could violate. **Do not** add a mutex around these.

**Timeouts.** Both timeouts live in config and are applied on every `curl_easy_setopt` call (we `curl_easy_reset` per request). The `GetPhoto` wait is on `CLOCK_MONOTONIC` (no RTC on a Pi, so the wall clock jumps when NTP syncs). Worst-case shutdown latency is one worker iteration — a re-register (3 requests) plus image + meta fetch, i.e. 5 × `request_timeout_s` — plus the 5s `GetPhoto` wait; current config gives ~55s. sd-bus dispatch is single-threaded so there's no in-flight method handler at teardown — see the shutdown-safety discussion in git history if touching `pp_dbus_free`.

**Meta-fetch fallback.** If `/get_current_img_meta` fails but the image fetch succeeded, we serve the image with `"{}"` metadata rather than discarding it. The image is the expensive/important part.

**Build and deploy are driven by the user.** Do not run `make`, `make deploy`, or `apt install`/`add_sysroot_pkg.sh` yourself — propose changes, let the user run them, and wait for output.
