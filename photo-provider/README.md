# photo-provider

Bridges a remote photo source with local D-Bus consumers. The source (backend) is chosen in the config: a [wwwslide](https://github.com/nicolasbrailo/wwwslide) HTTP photo server, or an [Immich](https://immich.app) server (via the `libimmich-random` submodule: pictures come album by album, a random sample of a random album at a time). With wwwslide, it registers as a client with the server, pre-fetches a small ring of JPEGs into `memfd`s, and hands them out one at a time over D-Bus (passing the fd directly). The ring also keeps a bounded history of already-delivered photos so consumers can step backwards. Target size and QR-embed flag can be changed at runtime over D-Bus; any change triggers a fresh server-side registration and flushes the cache.

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
| `SetAlbumFilter` | `(ssuu) → ()` | `(name, exclude, from_year, to_year)`: restricts which albums pictures come from, flushes the cache and saves the filter back to the config file. Ignored by the wwwslide backend. Fails with `InvalidArgs` on a year outside 0..9999, `from_year > to_year`, or a string over 511 bytes. |

Every method answers promptly, even while the server is unreachable: `GetPhoto` is the only one that waits, and its 5s bound is well under client call timeouts. Errors are named `io.homeboard.PhotoProvider.Error.*`. `GetPhoto` answers `Error.Unavailable` in general, but `Error.NoAlbumMatchesFilter` when the album list is non-empty and the filter dropped every album — consumers show its message, so a mistyped pattern doesn't read as a blank screen.

## Config

JSON file; see `config.json` for an example.

| Key | Meaning |
|-----|---------|
| `backend` | `wwwslide` (default) or `immich`. Any other value stops the service |
| `server_url` | wwwslide base URL (no trailing slash) |
| `target_size_w` / `target_size_h` | Requested render size (128..3840) |
| `embed_qr` | Ask server to burn a QR code into the image |
| `cache_depth` | Worker keeps this many photos pre-fetched ahead of the cursor |
| `history_depth` | Photos behind the cursor retained for `GetPrevPhoto` (0 disables `prev`) |
| `dump_to_disk` / `dump_dir` | If true, also write each fetched image as `N.jpeg` into `dump_dir` |
| `connect_timeout_s` / `request_timeout_s` | libcurl timeouts for all HTTP calls |
| `immich.server_url` | Immich base URL. Required with `backend: immich` |
| `immich.api_key` | Immich API key, with `album.read`, `asset.read` and `asset.view` permissions. Required with `backend: immich` |
| `immich.max_pictures_per_album` | Show at most this many pictures from each album (0: no limit) |
| `immich.percent_per_album` | Show this percentage of each album's pictures (0 or 100: all). Applied before `max_pictures_per_album` |
| `immich.album_refresh_s` | Re-fetch the album list this often (0: never) |
| `immich.album_filter` | `{name, exclude, from_year, to_year}`: which albums pictures may come from. Empty strings and `0` years mean no constraint, which is the default. **Written back by `SetAlbumFilter`**, so a filter set over D-Bus or MQTT survives a restart — and `make deploy-config` overwrites it, like every other persisted setting |

The `immich` section is ignored with `backend: wwwslide`, and the wwwslide keys (`server_url`, `embed_qr`) with `backend: immich`.

### Album filter

Immich's `GET /albums` can only filter by exact name, so anything more expressive is decided here, on the list the server already returned — which costs nothing extra, since each entry already carries the album's name, asset count and the dates of its oldest and newest asset.

`name` and `exclude` are comma-separated glob patterns matched against the **whole** album name, case-insensitively, with `*` and `?` as the only metacharacters (so `Trip (2019)` is selected by writing exactly that); `exclude` wins on a conflict. The year range is an **overlap** test against the server's dates, so an album running 2010–2026 matches `from_year=2019, to_year=2021`; an album with no dates is dropped as soon as either bound is set, and one with no assets never appears. A reversed range is rejected rather than honoured, since the overlap test would turn it into "the albums straddling the boundary" instead of "nothing".

The filter is applied where albums enter the rotation, in `libimmich-random`, so an excluded album costs no request at all, and it is re-applied whenever the album list is refreshed. Setting it never triggers a fetch of its own. The service logs the filter when it changes and `"N albums, M in the rotation"` when the rotation is rebuilt. `libimmich-random`'s `make test` covers the selection rules.

With `backend: immich`, `SetTargetSize` and `SetEmbedQr` are accepted but ignored (Immich can't render to a size or embed a QR code), and `connect_timeout_s` / `request_timeout_s` don't apply (the library uses a 10s connect timeout and aborts transfers stalled for 30s). Photos are Immich's previews: set their resolution to suit the display, and their format to JPEG, in Immich's image settings; non-JPEG previews are skipped. The metadata has wwwslide's keys except `albumpath`, plus `immich_id`, `description`, `latitude` / `longitude` and `people`.

## Install

```
git submodule update --init      # libimmich-random
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
| `config.c` | json-c config loader, plus the write-back that persists `SetAlbumFilter` |
| `backend.c` | Picks the backend from the config and dispatches to it; documents the contract every backend follows |
| `wwwslide_backend.c` | wwwslide backend: libcurl HTTP client; owns `client_id`, target size, embed_qr; handles (re-)registration |
| `immich_backend.c` | Immich backend: picks pictures with `libimmich-random`, serves Immich's preview JPEG, converts Immich metadata to wwwslide's JSON keys |
| `libimmich-random/` | Git submodule: the Immich client library |
| `cache.c` | Worker thread + cursor-indexed ring (history · current · prefetch); generation counter for invalidation |
| `dbus.c` | sd-bus vtable for the methods above |

---

## Notes for LLMs working in this directory

**Architecture: three layers, bottom-up ownership.**

- `backend` is the lowest layer: a thin dispatcher (a `switch` on the configured type) to one backend, `wwwslide_backend` or `immich_backend`. Every backend has the same contract, documented in `backend.h`: `fetch_next` does all network IO and runs only on the cache worker thread; setters never block; metadata is wwwslide-format JSON.
- `wwwslide_backend` is pure HTTP, no threads of its own. Owns one `CURL *` handle, the `client_id`, and the config. **All network IO runs on the cache worker thread** (inside `pp_wwwslide_backend_fetch_next`), so the handle and `client_id` are private to that thread and need no locking.
- `cache` sits above `backend`. It owns the worker thread, the ring, a mutex+condvar, and a generation counter. It borrows the `pp_backend *` (does not own it).
- `dbus` sits above both. Method handlers call into `cache` (for `GetPhoto` / `GetPrevPhoto`, and to invalidate after a config change) or `backend` (for setters). The dbus layer owns nothing but the sd-bus slot/connection. **Dispatch is single-threaded, so a handler must never block on the network**: one slow handler delays every caller, and a caller whose call times out leaves a late reply on its connection. This is why setters don't do HTTP and `GetPhoto`'s wait is bounded to 5s.

**Cache model: single ring + cursor.** The cache is a single `buf_cap = history_depth + 1 + cache_depth` ring indexed by monotonically-increasing logical positions (mod capacity). Three indices split it: `[head, cursor)` is history, `cursor` is the photo currently delivered (`-1` before the first delivery), `(cursor, tail)` is prefetched-but-not-yet-delivered. `GetPhoto` advances `cursor`; `GetPrevPhoto` retreats it. The worker fills from `tail`, and evicts `head` on overflow — but only when `head < cursor`, so it can never drop the photo the consumer is currently viewing. "prev then next" walks back over the same slot, so it returns the exact same photo. Going past the held history returns `Unavailable`.

**Registration is lazy, driven by a flag.** `needs_register` (atomic) starts `true` and is set again by setters that change a value and by `fetch_next` when the client has been idle past the server's eviction window. `fetch_next` does `atomic_exchange(needs_register, false)` and, if it was set, runs `/client_register` + config pushes before fetching. On failure it sets the flag again and returns -1, so the worker keeps retrying (every ~2s plus timeouts) until the server is reachable. Consequences: the service starts and answers D-Bus even if the server is down at boot, and a config change is never lost to a failed registration. The flag is cleared *before* `reregister` reads the config, so a setter racing with a registration sets it again and gets its own registration with its new values.

**Invalidation via generation counter.** When a setter reports a change (returns 1), `dbus.c` calls `pp_cache_invalidate`, which bumps a generation under the cache mutex and drains the ring. The setter stores the new value and sets `needs_register` *before* that, so a worker that observes the new generation also observes the flag. The worker captures `gen` before each fetch and, on push, discards the result if the current gen no longer matches — this is the only mechanism that protects against "worker fetched with the old config and finished after the change." A stale-client re-register does not invalidate: photos already held are still valid.

**Atomics for the mutable config.** `target_w`, `target_h`, `embed_qr` in `wwwslide_backend` are `atomic_uint` / `atomic_bool`. Setters use `atomic_exchange` to detect no-op writes. No mutex needed because these are only *read* inside `reregister`, where the new register call defines ordering against the server; there is no invariant between `target_w` and `target_h` that an intermediate read could violate. **Do not** add a mutex around these.

**Timeouts.** Both timeouts live in config and are applied on every `curl_easy_setopt` call (we `curl_easy_reset` per request). The `GetPhoto` wait is on `CLOCK_MONOTONIC` (no RTC on a Pi, so the wall clock jumps when NTP syncs). Worst-case shutdown latency is one worker iteration — a re-register (3 requests) plus image + meta fetch, i.e. 5 × `request_timeout_s` — plus the 5s `GetPhoto` wait; current config gives ~55s. The Immich backend ignores these timeouts (see Config), so its worst case isn't bounded the same way: a transfer that keeps trickling data never times out. sd-bus dispatch is single-threaded so there's no in-flight method handler at teardown — see the shutdown-safety discussion in git history if touching `pp_dbus_free`.

**Meta-fetch fallback.** If `/get_current_img_meta` fails but the image fetch succeeded, we serve the image with `"{}"` metadata rather than discarding it. The image is the expensive/important part.

**Build and deploy are driven by the user.** Do not run `make`, `make deploy`, or `apt install`/`add_sysroot_pkg.sh` yourself — propose changes, let the user run them, and wait for output.
