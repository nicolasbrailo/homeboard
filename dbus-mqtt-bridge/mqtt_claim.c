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

// Buffer big enough for host_info plus the appended render_cfg fields
// (rotation, interp, h_align, v_align — ~90 bytes worst case).
#define RC_CLAIM_PAYLOAD_MAX (RC_HOST_INFO_JSON_MAX + 128)

struct rc_mqtt_claim {
  char topic[160];
  struct rc_host_info host_info;
  struct img_render_cfg render_cfg;
  uint32_t display_w_px;
  uint32_t display_h_px;
  char online_payload[RC_CLAIM_PAYLOAD_MAX];
  size_t online_payload_len;
  char offline_payload[RC_CLAIM_PAYLOAD_MAX];
  size_t offline_payload_len;
  bool claimed;
};

// Format the full online claim payload: host info JSON with the render_cfg
// fields spliced in before the closing brace. Returns bytes written, or -1
// on failure (host info formatter failed, or buffer too small).
static int format_online_payload(const struct rc_mqtt_claim *c, char *buf,
                                 size_t buf_sz) {
  int n = rc_host_info_format_online_json(&c->host_info, buf, buf_sz);
  if (n <= 0 || buf[n - 1] != '}')
    return -1;
  int extra =
      snprintf(buf + n - 1, buf_sz - (size_t)(n - 1),
               ",\"rotation\":%u,\"interp\":\"%s\","
               "\"h_align\":\"%s\",\"v_align\":\"%s\",\"display_w_px\":%u,"
               "\"display_h_px\":%u}",
               (unsigned)c->render_cfg.rot,
               img_render_cfg_interpolation_name(c->render_cfg.interp),
               img_render_cfg_horizontal_align_name(c->render_cfg.h_align),
               img_render_cfg_vertical_align_name(c->render_cfg.v_align),
               c->display_w_px, c->display_h_px);
  if (extra < 0 || (size_t)(n - 1) + (size_t)extra >= buf_sz)
    return -1;
  return n - 1 + extra;
}

struct claim_state {
  const char *topic;
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
            "state/bridge: malformed JSON in retained payload; overwriting\n");
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
            "state/bridge: retained payload missing machine_id; overwriting\n");
    json_object_put(root);
    return;
  }
  if (strcmp(other_machine_id, s->our_machine_id) == 0) {
    printf("state/bridge: stale claim from previous run on this machine "
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
  int r = mosquitto_subscribe(mosq, NULL, s->topic, 0);
  if (r != MOSQ_ERR_SUCCESS) {
    fprintf(stderr, "subscribe(%s): %s\n", s->topic, mosquitto_strerror(r));
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
  if (strcmp(msg->topic, s->topic) != 0)
    return;
  s->resolved = true;
  inspect_retained(s, msg);
}

struct rc_mqtt_claim *rc_mqtt_claim_new(const char *topic_prefix,
                                        struct mosquitto *mosq) {
  struct rc_mqtt_claim *c = calloc(1, sizeof(*c));
  if (!c)
    return NULL;
  snprintf(c->topic, sizeof(c->topic), "%sstate/bridge", topic_prefix);

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

  // Default render_cfg matches the parser's fallback for unknown inputs:
  // ROT_0 / BILINEAR / CENTER / CENTER. Callers should overwrite via
  // rc_mqtt_claim_set_render_cfg once the real cfg is known.
  c->render_cfg.rot = ROT_0;
  c->render_cfg.interp = INTERP_BILINEAR;
  c->render_cfg.h_align = HORIZONTAL_ALIGN_CENTER;
  c->render_cfg.v_align = VERTICAL_ALIGN_CENTER;
  c->display_w_px = 0;
  c->display_h_px = 0;

  int n =
      format_online_payload(c, c->online_payload, sizeof(c->online_payload));
  if (n < 0) {
    n = snprintf(c->online_payload, sizeof(c->online_payload),
                 "{\"state\":\"online\",\"machine_id\":\"%s\"}",
                 c->host_info.machine_id);
  }
  c->online_payload_len = (n > 0) ? (size_t)n : 0;

  // The offline payload carries only immutable identity fields. Once the
  // bridge stops serving, anything else (ip, render_cfg, ...) is a
  // frozen-at-shutdown value that would otherwise be presented as
  // authoritative — so we strip those out and use the same payload for
  // both the LWT (crash path) and the graceful-shutdown publish in
  // rc_mqtt_claim_free.
  n = rc_host_info_format_offline_json(&c->host_info, c->offline_payload,
                                       sizeof(c->offline_payload));
  if (n < 0) {
    n = snprintf(c->offline_payload, sizeof(c->offline_payload),
                 "{\"state\":\"offline\",\"machine_id\":\"%s\"}",
                 c->host_info.machine_id);
  }
  c->offline_payload_len = (n > 0) ? (size_t)n : 0;

  // MQTT 3.1.1 latches the LWT from the CONNECT packet and offers no way
  // to update it on a live session, so it MUST be set before connecting.
  // Since our offline payload is already immutable-only, we can hand it
  // straight to mosquitto without a separate buffer.
  int r = mosquitto_will_set(mosq, c->topic, (int)c->offline_payload_len,
                             c->offline_payload, 0, true);
  if (r != MOSQ_ERR_SUCCESS)
    fprintf(stderr, "No LWT, mosquitto_will_set: %s\n", mosquitto_strerror(r));

  return c;
}

void rc_mqtt_claim_free(struct rc_mqtt_claim *c, struct mosquitto *mosq) {
  if (!c)
    return;
  if (mosq && c->claimed && c->offline_payload_len > 0) {
    // Leave a last-known-offline record retained on graceful shutdown.
    // Same payload shape as the LWT, so the offline view of a device is
    // identical regardless of whether it died gracefully or crashed.
    mosquitto_publish(mosq, NULL, c->topic, (int)c->offline_payload_len,
                      c->offline_payload, 0, true);
  }
  free(c);
}

void rc_mqtt_claim_publish_online(const struct rc_mqtt_claim *c,
                                  struct mosquitto *mosq) {
  if (!c || !mosq)
    return;
  if (c->online_payload_len == 0) {
    fprintf(stderr, "Failed to format host info, will not publish. Clients may "
                    "conflict if a name is shared\n");
    return;
  }
  int r = mosquitto_publish(mosq, NULL, c->topic, (int)c->online_payload_len,
                            c->online_payload, 0, true);
  if (r != MOSQ_ERR_SUCCESS) {
    fprintf(stderr, "online publish(%s): %s\n", c->topic,
            mosquitto_strerror(r));
  }
}

void rc_mqtt_claim_set_render_cfg(struct rc_mqtt_claim *c,
                                  struct mosquitto *mosq,
                                  const struct img_render_cfg *cfg,
                                  uint32_t display_w_px,
                                  uint32_t display_h_px) {
  if (!c || !cfg)
    return;
  c->render_cfg = *cfg;
  c->display_w_px = display_w_px;
  c->display_h_px = display_h_px;

  // Only the online payload carries render_cfg; the offline payload is
  // immutable-only by design and does not need refreshing here.
  int n =
      format_online_payload(c, c->online_payload, sizeof(c->online_payload));
  c->online_payload_len = (n > 0) ? (size_t)n : 0;

  if (mosq && c->claimed && c->online_payload_len > 0) {
    int r = mosquitto_publish(mosq, NULL, c->topic, (int)c->online_payload_len,
                              c->online_payload, 0, true);
    if (r != MOSQ_ERR_SUCCESS) {
      fprintf(stderr, "render_cfg republish(%s): %s\n", c->topic,
              mosquitto_strerror(r));
    }
  }
}

int rc_mqtt_claim_run(struct rc_mqtt_claim *c, struct mosquitto *mosq,
                      char *conflict_summary, size_t conflict_summary_sz) {
  if (conflict_summary && conflict_summary_sz > 0)
    conflict_summary[0] = '\0';

  struct claim_state s = {
      .topic = c->topic,
      .our_machine_id = c->host_info.machine_id,
      .conflict_summary = conflict_summary,
      .conflict_summary_sz = conflict_summary_sz,
  };

  // Take over callbacks for the duration. Caller is responsible for
  // reinstalling its own afterward.
  mosquitto_user_data_set(mosq, &s);
  mosquitto_connect_callback_set(mosq, on_connect);
  mosquitto_message_callback_set(mosq, on_message);

  printf("Claiming topic '%s'...\n", c->topic);
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
      fprintf(stderr, "Claim phase for '%s' timed out after %ds\n", c->topic,
              CLAIM_TOTAL_TIMEOUT_S);
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

  int r = mosquitto_publish(mosq, NULL, c->topic, (int)c->online_payload_len,
                            c->online_payload, 0, true);
  if (r != MOSQ_ERR_SUCCESS) {
    fprintf(stderr, "publish(%s): %s\n", c->topic, mosquitto_strerror(r));
    return RC_MQTT_CLAIM_TRANSPORT;
  }
  c->claimed = true;
  printf("Claim accepted for '%s'\n", c->topic);
  return RC_MQTT_CLAIM_OK;
}
