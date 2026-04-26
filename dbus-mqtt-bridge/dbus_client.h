#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <systemd/sd-bus.h>

struct rc_dbus;

typedef void (*rc_dbus_occupancy_cb)(bool occupied, uint32_t distance_cm,
                                     void *ud);
typedef void (*rc_dbus_displayed_photo_cb)(const char *meta, void *ud);
typedef void (*rc_dbus_slideshow_active_cb)(bool active, void *ud);

struct rc_dbus *rc_dbus_init(rc_dbus_occupancy_cb on_occupancy,
                             rc_dbus_displayed_photo_cb on_displayed_photo,
                             rc_dbus_slideshow_active_cb on_slideshow_active,
                             void *ud);
void rc_dbus_free(struct rc_dbus *d);

sd_bus *rc_dbus_bus(struct rc_dbus *d);

int rc_dbus_ambience_call_void(struct rc_dbus *d, const char *method);
int rc_dbus_ambience_set_transition_time(struct rc_dbus *d, uint32_t secs);
int rc_dbus_ambience_set_render_config(struct rc_dbus *d, uint32_t rotation,
                                       const char *interp, const char *h_align,
                                       const char *v_align);
int rc_dbus_ambience_set_remote_control_server(struct rc_dbus *d,
                                               const char *url,
                                               const char *qr_img);
int rc_dbus_presence_call_void(struct rc_dbus *d, const char *method);
int rc_dbus_photo_set_embed_qr(struct rc_dbus *d, bool on);
int rc_dbus_photo_set_target_size(struct rc_dbus *d, uint32_t w, uint32_t h);
