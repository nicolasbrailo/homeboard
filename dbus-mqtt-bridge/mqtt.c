#include "mqtt.h"

#include "config.h"

#include <errno.h>
#include <mosquitto.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define RECONNECT_THROTTLE_S 30

struct rc_mqtt {
  struct mosquitto *mosq;
  rc_mqtt_cmd_cb on_cmd;
  void *ud;
  char topic_prefix[64];
  char cmd_prefix[96];
  size_t cmd_prefix_len;
  char bridge_state_topic[128];
  char host[128];
  uint16_t port;
  uint16_t keepalive_s;
  time_t next_reconnect_ts;
};

static time_t monotonic_now(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return ts.tv_sec;
}

static void try_connect(struct rc_mqtt *m) {
  time_t now = monotonic_now();
  if (now < m->next_reconnect_ts)
    return;
  m->next_reconnect_ts = now + RECONNECT_THROTTLE_S;

  int r;
  if (mosquitto_socket(m->mosq) < 0)
    r = mosquitto_connect(m->mosq, m->host, m->port, m->keepalive_s);
  else
    r = mosquitto_reconnect(m->mosq);
  if (r != MOSQ_ERR_SUCCESS)
    fprintf(stderr, "MQTT connect to %s:%u failed: %s\n", m->host, m->port, mosquitto_strerror(r));
}

static void on_connect_cb(struct mosquitto *mosq, void *obj, int rc) {
  struct rc_mqtt *m = obj;
  if (rc != 0) {
    fprintf(stderr, "MQTT CONNACK failed: %s\n", mosquitto_connack_string(rc));
    return;
  }
  printf("MQTT connected\n");

  char sub[128];
  snprintf(sub, sizeof(sub), "%scmd/#", m->topic_prefix);
  int r = mosquitto_subscribe(mosq, NULL, sub, 0);
  if (r != MOSQ_ERR_SUCCESS)
    fprintf(stderr, "mosquitto_subscribe(%s): %s\n", sub, mosquitto_strerror(r));

  mosquitto_publish(mosq, NULL, m->bridge_state_topic, 6, "online", 0, true);
}

static void on_disconnect_cb(struct mosquitto *mosq, void *obj, int rc) {
  (void)mosq;
  (void)obj;
  fprintf(stderr, "MQTT disconnected (rc=%d)\n", rc);
}

static void on_message_cb(struct mosquitto *mosq, void *obj, const struct mosquitto_message *msg) {
  (void)mosq;
  struct rc_mqtt *m = obj;
  if (!msg->topic)
    return;
  size_t tlen = strlen(msg->topic);
  if (tlen <= m->cmd_prefix_len || memcmp(msg->topic, m->cmd_prefix, m->cmd_prefix_len) != 0)
    return;
  const char *suffix = msg->topic + m->cmd_prefix_len;
  const char *payload = msg->payload ? (const char *)msg->payload : "";
  size_t plen = msg->payloadlen > 0 ? (size_t)msg->payloadlen : 0;
  m->on_cmd(suffix, payload, plen, m->ud);
}

struct rc_mqtt *rc_mqtt_init(const struct rc_config *cfg, rc_mqtt_cmd_cb on_cmd, void *ud) {
  mosquitto_lib_init();

  struct rc_mqtt *m = calloc(1, sizeof(*m));
  if (!m)
    return NULL;
  m->on_cmd = on_cmd;
  m->ud = ud;
  strncpy(m->topic_prefix, cfg->topic_prefix, sizeof(m->topic_prefix) - 1);
  snprintf(m->cmd_prefix, sizeof(m->cmd_prefix), "%scmd/", m->topic_prefix);
  m->cmd_prefix_len = strlen(m->cmd_prefix);
  snprintf(m->bridge_state_topic, sizeof(m->bridge_state_topic), "%sstate/bridge", m->topic_prefix);

  m->mosq = mosquitto_new(cfg->mqtt_client_id[0] ? cfg->mqtt_client_id : NULL, true, m);
  if (!m->mosq) {
    fprintf(stderr, "mosquitto_new: %s\n", strerror(errno));
    free(m);
    mosquitto_lib_cleanup();
    return NULL;
  }

  if (cfg->mqtt_user[0]) {
    mosquitto_username_pw_set(m->mosq, cfg->mqtt_user, cfg->mqtt_pass[0] ? cfg->mqtt_pass : NULL);
    printf("Set mqtt user %s\n", cfg->mqtt_user);
  }

  mosquitto_connect_callback_set(m->mosq, on_connect_cb);
  mosquitto_disconnect_callback_set(m->mosq, on_disconnect_cb);
  mosquitto_message_callback_set(m->mosq, on_message_cb);

  int r = mosquitto_will_set(m->mosq, m->bridge_state_topic, 7, "offline", 0, true);
  if (r != MOSQ_ERR_SUCCESS)
    fprintf(stderr, "mosquitto_will_set: %s\n", mosquitto_strerror(r));

  strncpy(m->host, cfg->mqtt_host, sizeof(m->host) - 1);
  m->port = cfg->mqtt_port;
  m->keepalive_s = cfg->mqtt_keepalive_s;
  m->next_reconnect_ts = 0;

  try_connect(m);
  return m;
}

void rc_mqtt_free(struct rc_mqtt *m) {
  if (!m)
    return;
  if (m->mosq) {
    mosquitto_publish(m->mosq, NULL, m->bridge_state_topic, 7, "offline", 0, true);
    mosquitto_loop_write(m->mosq, 1);
    mosquitto_disconnect(m->mosq);
    mosquitto_destroy(m->mosq);
  }
  mosquitto_lib_cleanup();
  free(m);
}

int rc_mqtt_socket(struct rc_mqtt *m) { return mosquitto_socket(m->mosq); }

bool rc_mqtt_want_write(struct rc_mqtt *m) { return mosquitto_want_write(m->mosq); }

int rc_mqtt_loop_read(struct rc_mqtt *m) {
  int r = mosquitto_loop_read(m->mosq, 1);
  if (r != MOSQ_ERR_SUCCESS) {
    fprintf(stderr, "mosquitto_loop_read: %s\n", mosquitto_strerror(r));
    try_connect(m);
  }
  return r;
}

int rc_mqtt_loop_write(struct rc_mqtt *m) {
  int r = mosquitto_loop_write(m->mosq, 1);
  if (r != MOSQ_ERR_SUCCESS)
    fprintf(stderr, "mosquitto_loop_write: %s\n", mosquitto_strerror(r));
  return r;
}

int rc_mqtt_loop_misc(struct rc_mqtt *m) {
  if (mosquitto_socket(m->mosq) < 0) {
    try_connect(m);
    return 0;
  }
  int r = mosquitto_loop_misc(m->mosq);
  if (r == MOSQ_ERR_NO_CONN)
    try_connect(m);
  return r;
}

int rc_mqtt_publish(struct rc_mqtt *m, const char *topic_suffix, const char *payload, size_t len, bool retain) {
  char topic[192];
  snprintf(topic, sizeof(topic), "%s%s", m->topic_prefix, topic_suffix);
  int r = mosquitto_publish(m->mosq, NULL, topic, (int)len, payload, 0, retain);
  if (r != MOSQ_ERR_SUCCESS)
    fprintf(stderr, "mosquitto_publish(%s): %s\n", topic, mosquitto_strerror(r));
  return r == MOSQ_ERR_SUCCESS ? 0 : -1;
}
