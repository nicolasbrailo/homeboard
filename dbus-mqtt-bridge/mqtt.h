#pragma once

#include <stdbool.h>
#include <stddef.h>

struct rc_mqtt;
struct rc_config;

typedef void (*rc_mqtt_cmd_cb)(const char *topic_suffix, const char *payload,
                               size_t len, void *ud);
typedef void (*rc_mqtt_active_server_cb)(const char *url, const char *qr_img,
                                         void *ud);

struct rc_mqtt *rc_mqtt_init(const struct rc_config *cfg, rc_mqtt_cmd_cb on_cmd,
                             rc_mqtt_active_server_cb on_active_server,
                             void *ud);
void rc_mqtt_free(struct rc_mqtt *m);

int rc_mqtt_socket(struct rc_mqtt *m);
bool rc_mqtt_want_write(struct rc_mqtt *m);

int rc_mqtt_loop_read(struct rc_mqtt *m);
int rc_mqtt_loop_write(struct rc_mqtt *m);
int rc_mqtt_loop_misc(struct rc_mqtt *m);

int rc_mqtt_publish(struct rc_mqtt *m, const char *topic_suffix,
                    const char *payload, size_t len, bool retain);
