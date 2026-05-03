#include "mqtt_claim.h"

#include "host_info.h"

#include <json-c/json.h>
#include <mosquitto.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define CLAIM_WAIT_MS 2000
#define CLAIM_TOTAL_TIMEOUT_S 10

struct rc_mqtt_claim {
  char bridge_info_topic[160];
  struct rc_host_info host_info;
  char info_payload[RC_HOST_INFO_JSON_MAX];
  size_t info_payload_len;
  bool claimed;
};

struct claim_state {
  const char *bridge_info_topic;
  const char *our_machine_id;
  bool resolved;
  bool conflict;
  bool transport_error;
  long long subscribed_at_ms;
  char *conflict_summary;
  size_t conflict_summary_sz;
};

static long long monotonic_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void inspect_retained(struct claim_state *s,
                             const struct mosquitto_message *msg) {
  if (!msg->payload || msg->payloadlen <= 0) {
    // Empty retained payload = previous owner cleared it. Free to claim.
    return;
  }
  struct json_tokener *tok = json_tokener_new();
  if (!tok)
    return;
  struct json_object *root =
      json_tokener_parse_ex(tok, (const char *)msg->payload, msg->payloadlen);
  json_tokener_free(tok);
  if (!root) {
    fprintf(stderr,
            "bridge_info: malformed JSON in retained payload; overwriting\n");
    return;
  }
  struct json_object *o;
  const char *other_machine_id = NULL;
  const char *other_hostname = "";
  const char *other_ip = "";
  const char *other_model = "";
  if (json_object_object_get_ex(root, "machine_id", &o))
    other_machine_id = json_object_get_string(o);
  if (json_object_object_get_ex(root, "hostname", &o)) {
    const char *v = json_object_get_string(o);
    if (v)
      other_hostname = v;
  }
  if (json_object_object_get_ex(root, "ip", &o)) {
    const char *v = json_object_get_string(o);
    if (v)
      other_ip = v;
  }
  if (json_object_object_get_ex(root, "host_model", &o)) {
    const char *v = json_object_get_string(o);
    if (v)
      other_model = v;
  }

  if (!other_machine_id || !*other_machine_id) {
    fprintf(stderr,
            "bridge_info: retained payload missing machine_id; overwriting\n");
    json_object_put(root);
    return;
  }
  if (strcmp(other_machine_id, s->our_machine_id) == 0) {
    printf("bridge_info: stale claim from previous run on this machine "
           "(machine_id=%s); overwriting\n",
           other_machine_id);
    json_object_put(root);
    return;
  }
  s->conflict = true;
  if (s->conflict_summary && s->conflict_summary_sz > 0) {
    snprintf(s->conflict_summary, s->conflict_summary_sz,
             "machine_id=%s hostname=%s ip=%s model=%s", other_machine_id,
             other_hostname, other_ip, other_model);
  }
  json_object_put(root);
}

static void on_connect(struct mosquitto *mosq, void *obj, int rc) {
  struct claim_state *s = obj;
  if (rc != 0) {
    fprintf(stderr, "MQTT CONNACK failed: %s\n", mosquitto_connack_string(rc));
    s->transport_error = true;
    s->resolved = true;
    return;
  }
  printf("MQTT connected\n");
  int r = mosquitto_subscribe(mosq, NULL, s->bridge_info_topic, 0);
  if (r != MOSQ_ERR_SUCCESS) {
    fprintf(stderr, "subscribe(%s): %s\n", s->bridge_info_topic,
            mosquitto_strerror(r));
    s->transport_error = true;
    s->resolved = true;
    return;
  }
  s->subscribed_at_ms = monotonic_ms();
}

static void on_message(struct mosquitto *mosq, void *obj,
                       const struct mosquitto_message *msg) {
  (void)mosq;
  struct claim_state *s = obj;
  if (!msg->topic || s->resolved)
    return;
  if (strcmp(msg->topic, s->bridge_info_topic) != 0)
    return;
  s->resolved = true;
  inspect_retained(s, msg);
}

struct rc_mqtt_claim *rc_mqtt_claim_new(const char *topic_prefix) {
  struct rc_mqtt_claim *c = calloc(1, sizeof(*c));
  if (!c)
    return NULL;
  snprintf(c->bridge_info_topic, sizeof(c->bridge_info_topic),
           "%sstate/bridge_info", topic_prefix);

  if (rc_host_info_collect(&c->host_info) < 0) {
    fprintf(
        stderr,
        "Failed to read /etc/machine-id; cannot detect prefix collisions\n");
    free(c);
    return NULL;
  }
  printf("Host info: machine_id=%s hostname=%s ip=%s model=%s\n",
         c->host_info.machine_id, c->host_info.hostname,
         c->host_info.ip[0] ? c->host_info.ip : "(none)",
         c->host_info.host_model);

  int n = rc_host_info_format_json(&c->host_info, c->info_payload,
                                   sizeof(c->info_payload));
  if (n < 0) {
    n = snprintf(c->info_payload, sizeof(c->info_payload),
                 "{\"machine_id\":\"%s\"}", c->host_info.machine_id);
  }
  c->info_payload_len = (n > 0) ? (size_t)n : 0;
  return c;
}

void rc_mqtt_claim_free(struct rc_mqtt_claim *c, struct mosquitto *mosq) {
  if (!c)
    return;
  if (mosq && c->claimed) {
    // Clear retained bridge_info so the next bridge starting on this prefix
    // (same or different machine) sees a free slot.
    mosquitto_publish(mosq, NULL, c->bridge_info_topic, 0, NULL, 0, true);
  }
  free(c);
}

void rc_mqtt_claim_republish(const struct rc_mqtt_claim *c,
                             struct mosquitto *mosq) {
  if (!c || !mosq)
    return;
  if (c->info_payload_len == 0) {
    fprintf(stderr, "Failed to get host info, will not publish. Clients may "
                    "conflict if a name is shared\n");
    return;
  }
  int r = mosquitto_publish(mosq, NULL, c->bridge_info_topic,
                            (int)c->info_payload_len, c->info_payload, 0, true);
  if (r != MOSQ_ERR_SUCCESS) {
    fprintf(stderr, "host info publish(%s): %s\n", c->bridge_info_topic,
            mosquitto_strerror(r));
  }
}

int rc_mqtt_claim_run(struct rc_mqtt_claim *c, struct mosquitto *mosq,
                      char *conflict_summary, size_t conflict_summary_sz) {
  if (conflict_summary && conflict_summary_sz > 0)
    conflict_summary[0] = '\0';

  struct claim_state s = {
      .bridge_info_topic = c->bridge_info_topic,
      .our_machine_id = c->host_info.machine_id,
      .conflict_summary = conflict_summary,
      .conflict_summary_sz = conflict_summary_sz,
  };

  // Take over callbacks for the duration. Caller is responsible for
  // reinstalling its own afterward.
  mosquitto_user_data_set(mosq, &s);
  mosquitto_connect_callback_set(mosq, on_connect);
  mosquitto_message_callback_set(mosq, on_message);

  printf("Claiming topic '%s'...\n", c->bridge_info_topic);
  long long start_ms = monotonic_ms();
  while (!s.resolved) {
    long long now_ms = monotonic_ms();
    if (s.subscribed_at_ms != 0 &&
        now_ms - s.subscribed_at_ms >= CLAIM_WAIT_MS) {
      // No retained message arrived in the wait window: topic is free.
      s.resolved = true;
      break;
    }
    if (now_ms - start_ms >= CLAIM_TOTAL_TIMEOUT_S * 1000) {
      fprintf(stderr, "Claim phase for '%s' timed out after %ds\n",
              c->bridge_info_topic, CLAIM_TOTAL_TIMEOUT_S);
      return RC_MQTT_CLAIM_TRANSPORT;
    }
    int lr = mosquitto_loop(mosq, 100, 1);
    if (lr != MOSQ_ERR_SUCCESS) {
      fprintf(stderr, "MQTT loop during claim: %s\n", mosquitto_strerror(lr));
      return RC_MQTT_CLAIM_TRANSPORT;
    }
  }

  if (s.transport_error)
    return RC_MQTT_CLAIM_TRANSPORT;
  if (s.conflict)
    return RC_MQTT_CLAIM_CONFLICT;

  int r = mosquitto_publish(mosq, NULL, c->bridge_info_topic,
                            (int)c->info_payload_len, c->info_payload, 0, true);
  if (r != MOSQ_ERR_SUCCESS) {
    fprintf(stderr, "publish(%s): %s\n", c->bridge_info_topic,
            mosquitto_strerror(r));
    return RC_MQTT_CLAIM_TRANSPORT;
  }
  c->claimed = true;
  printf("Claim accepted for '%s'\n", c->bridge_info_topic);
  return RC_MQTT_CLAIM_OK;
}
