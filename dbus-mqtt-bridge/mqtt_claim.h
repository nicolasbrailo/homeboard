#pragma once

#include <stddef.h>

#include <jpeg_render/img_render.h>

struct mosquitto;
struct rc_mqtt_claim;

#define RC_MQTT_CLAIM_OK 0
#define RC_MQTT_CLAIM_CONFLICT (-1)
#define RC_MQTT_CLAIM_TRANSPORT (-2)

// Allocate claim state. Reads host info and pre-formats both the online and
// offline JSON payloads for the merged state/bridge topic. Returns NULL if
// host info cannot be collected.
// The instance of mosq must be valid but not connected yet (the LWT needs to be
// set before calling mosquitto_connect)
struct rc_mqtt_claim *rc_mqtt_claim_new(const char *topic_prefix,
                                        struct mosquitto *mosq);

// Free claim state. If `mosq` is non-NULL AND the claim was previously
// successful, also publishes our offline payload retained so consumers see
// the device as last-known-offline. Safe to call with a NULL claim or in any
// failed-init state.
void rc_mqtt_claim_free(struct rc_mqtt_claim *c, struct mosquitto *mosq);

// Synchronously verify no other machine currently owns the state/bridge
// topic and publish our retained online claim.
//
// `mosq` must already have had mosquitto_connect() called on it (TCP-level)
// but CONNACK may still be pending. The function takes over the connect and
// message callbacks plus user_data on `mosq` for the duration of the call;
// the caller MUST reinstall its own callbacks and user_data afterwards.
//
// On RC_MQTT_CLAIM_CONFLICT, `conflict_summary` is filled with a human-
// readable description of the colliding host. On other return values it is
// left empty.
int rc_mqtt_claim_run(struct rc_mqtt_claim *c, struct mosquitto *mosq,
                      char *conflict_summary, size_t conflict_summary_sz);

// Republish the online payload. Call from on_connect_cb on reconnect, since
// the broker will have applied the LWT (offline payload) when our previous
// session dropped, and we need to overwrite it with the live online state.
void rc_mqtt_claim_publish_online(const struct rc_mqtt_claim *c,
                                  struct mosquitto *mosq);

// Update the img_render_cfg embedded in the claim payload and (if `mosq` is
// non-NULL and the claim is live) republish the retained online payload so
// subscribers see the new values immediately.
void rc_mqtt_claim_set_render_cfg(struct rc_mqtt_claim *c,
                                  struct mosquitto *mosq,
                                  const struct img_render_cfg *cfg);
