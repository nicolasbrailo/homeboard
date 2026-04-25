#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <systemd/sd-event.h>

// Outbound client for io.homeboard.PhotoProvider. Owns its own system-bus
// connection (attached to the shared sd_event) and uses it for all calls.
//
// Fetch is async (sd_bus_call_method_async); the reply lands on the event
// thread when the dispatch loop processes it. Only one in-flight fetch
// at a time — issuing a new one cancels the prior.

struct PhotoClient;

// Fired on the dispatch thread when an async fetch completes.
//   fd:   open file descriptor for the JPEG, or -1 on failure. Borrowed
//         for the duration of the callback (sd_bus closes it after the
//         callback returns); duplicate it if you need to keep it.
//   meta: borrowed metadata string, or NULL on failure. Valid only during
//         the callback.
typedef void (*photo_client_fetch_cb)(int fd, const char *meta, void *ud);

// `event` is borrowed; the caller owns it and must outlive the client.
struct PhotoClient *photo_client_init(sd_event *event);
void photo_client_free(struct PhotoClient *pc);

// Issue an async GetPhoto (or GetPrevPhoto if `prev`). Cancels any prior
// in-flight request. Returns 0 if the request was queued, -1 on failure
// (the callback will not fire in that case).
int photo_client_fetch_async(struct PhotoClient *pc, bool prev,
                             photo_client_fetch_cb cb, void *ud);

// Cancel any in-flight fetch. The callback will not fire afterwards.
void photo_client_cancel_fetch(struct PhotoClient *pc);

// Synchronous config calls. Use when rotation toggles between
// portrait/landscape (set_target_size) or to retune the QR overlay
// (set_embed_qr). push_config does both. All return 0 on success, -1 on
// failure (warning logged).
int photo_client_set_target_size(struct PhotoClient *pc, uint32_t w,
                                 uint32_t h);
int photo_client_set_embed_qr(struct PhotoClient *pc, bool embed_qr);
int photo_client_push_config(struct PhotoClient *pc, uint32_t w, uint32_t h,
                             bool embed_qr);
