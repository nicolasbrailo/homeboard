#pragma once

#include <stddef.h>

struct mosquitto;
struct rc_mqtt_claim;

#define RC_MQTT_CLAIM_OK 0
#define RC_MQTT_CLAIM_CONFLICT (-1)
#define RC_MQTT_CLAIM_TRANSPORT (-2)

// Allocate claim state. Reads /host info, and prepares for MQTT deliver.
// Returns NULL if host info cannot be collected.
struct rc_mqtt_claim *rc_mqtt_claim_new(const char *topic_prefix);

// Free claim state. If `mosq` is non-NULL AND the claim was previously
// successful, also publishes an empty retained bridge_info to clear our
// claim, so the next bridge starting on this prefix sees a free slot. Safe
// to call with a NULL claim or in any failed-init state.
void rc_mqtt_claim_free(struct rc_mqtt_claim *c, struct mosquitto *mosq);

// Synchronously verify no other machine currently owns the bridge_info
// topic and publish our retained claim.
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

// Republish our retained bridge_info. Call from on_connect_cb on reconnect
// after the LWT may have cleared our retained payload from the broker.
void rc_mqtt_claim_republish(const struct rc_mqtt_claim *c,
                             struct mosquitto *mosq);
