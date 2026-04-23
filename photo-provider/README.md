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
| `GetPhoto` | `() → (h, s)` | Advances the cursor and returns `(fd, metadata_json)` for the next photo. Fd is a `memfd`; caller closes. Blocks up to 30s if prefetch is empty. |
| `GetPrevPhoto` | `() → (h, s)` | Retreats the cursor one step and returns the previous photo from the history window. Non-blocking; fails with `Unavailable` if no history is held. |
| `SetTargetSize` | `(uu) → ()` | Updates `(w, h)`, re-registers with server, flushes cache. |
| `SetEmbedQr` | `(b) → ()` | Toggles QR embed, re-registers, flushes. |

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

- `www_session` is the lowest layer: pure HTTP, no threads of its own. Owns two `CURL *` handles — `curl_fetch` (used by the cache worker thread) and `curl_ctrl` (used by the dbus thread for setters). They are separate so a slow image fetch does not block a config change or vice versa; libcurl easy handles are not thread-safe, so sharing one would require a mutex across the whole fetch.
- `cache` sits above `www_session`. It owns the worker thread, the ring, a mutex+condvar, and a generation counter. It borrows the `www_session *` (does not own it).
- `dbus` sits above both. Method handlers call into `cache` (for `GetPhoto` / `GetPrevPhoto`) or `www_session` (for setters). The dbus layer owns nothing but the sd-bus slot/connection.

**Cache model: single ring + cursor.** The cache is a single `buf_cap = history_depth + 1 + cache_depth` ring indexed by monotonically-increasing logical positions (mod capacity). Three indices split it: `[head, cursor)` is history, `cursor` is the photo currently delivered (`-1` before the first delivery), `(cursor, tail)` is prefetched-but-not-yet-delivered. `GetPhoto` advances `cursor`; `GetPrevPhoto` retreats it. The worker fills from `tail`, and evicts `head` on overflow — but only when `head < cursor`, so it can never drop the photo the consumer is currently viewing. "prev then next" walks back over the same slot, so it returns the exact same photo. Going past the held history returns `Unavailable`.

**Two-phase init on `www_session`.** `pp_www_session_init` sets up the struct and CURL handles but does not touch the network. `pp_www_session_start` performs the initial `/client_register` and installs the invalidate callback. This split exists to break the bootstrap cycle: `cache` needs a `www_session *`, and `www_session`'s invalidate callback needs to point into `cache`. Order in `main.c`: ws_init → cache_init → ws_start(cb=pp_cache_invalidate, ud=cache).

**Invalidation via generation counter.** When a setter fires, `www_session` does `/client_register`, pushes config, installs the new id, and invokes the invalidate callback. `cache_invalidate` bumps a `uint64_t` generation under the cache mutex and drains the ring. The worker captures `gen` before each fetch and, on push, discards the result if the current gen no longer matches — this is the only mechanism that protects against "worker fetched with the old client_id and finished after the re-register."

**Atomics for the mutable config.** `target_w`, `target_h`, `embed_qr` in `www_session` are `atomic_uint` / `atomic_bool`. Setters use `atomic_exchange` to detect no-op writes. No mutex needed because these are only *read* inside `reregister`, where the new register call defines ordering against the server; there is no invariant between `target_w` and `target_h` that an intermediate read could violate. **Do not** add a mutex around these.

**`client_id` uses a mutex.** It's a string (longer than any atomic), and the fetch path needs a stable snapshot during one HTTP call. The pattern in `fetch_next`: lock, copy into a local buffer, unlock, then do IO with the local copy.

**Timeouts.** Both timeouts live in config and are applied on every `curl_easy_setopt` call (we `curl_easy_reset` per request). Worst-case shutdown latency is `request_timeout_s` for an in-flight fetch plus the 30s `GetPhoto` dbus-side wait; current config gives ~90s. sd-bus dispatch is single-threaded so there's no in-flight method handler at teardown — see the shutdown-safety discussion in git history if touching `pp_dbus_free`.

**Meta-fetch fallback.** If `/get_current_img_meta` fails but the image fetch succeeded, we serve the image with `"{}"` metadata rather than discarding it. The image is the expensive/important part.

**Build and deploy are driven by the user.** Do not run `make`, `make deploy`, or `apt install`/`add_sysroot_pkg.sh` yourself — propose changes, let the user run them, and wait for output.
