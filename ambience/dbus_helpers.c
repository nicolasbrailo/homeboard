#include "dbus_helpers.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

bool is_service_up(sd_bus *bus, const char *svc_name) {
  sd_bus_error err = SD_BUS_ERROR_NULL;
  sd_bus_message *reply = NULL;
  const char *owner = NULL;
  int r = sd_bus_call_method(bus, "org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus",
                             "GetNameOwner", &err, &reply, "s", svc_name);
  if (r < 0) {
    if (sd_bus_error_has_name(&err, "org.freedesktop.DBus.Error.NameHasNoOwner")) {
      return 0;
    } else {
      fprintf(stderr, "GetNameOwner(%s) failed: %s\n", svc_name, err.message ? err.message : strerror(-r));
      return 0;
    }
  } else {
    r = sd_bus_message_read(reply, "s", &owner);
    if (r >= 0) {
      printf("Service %s offered by %s\n", svc_name, owner);
    }
    return true;
  }
  sd_bus_error_free(&err);
  sd_bus_message_unref(reply);
}

struct updown_ctx {
  service_updown_cb cb;
  void *ud;
};

static int service_updown_trampoline(sd_bus_message *m, void *p, sd_bus_error *err) {
  (void)err;
  struct updown_ctx *ctx = p;
  const char *name = NULL, *old_owner = NULL, *new_owner = NULL;
  int r = sd_bus_message_read(m, "sss", &name, &old_owner, &new_owner);
  if (r < 0)
    return 0;
  if (new_owner && new_owner[0] == '\0') {
    fprintf(stderr, "WARNING: %s disappeared from the bus\n", name);
    ctx->cb(ctx->ud, false);
  } else if (old_owner && old_owner[0] == '\0') {
    printf("%s appeared on the bus (owner=%s)\n", name, new_owner);
    ctx->cb(ctx->ud, true);
  }
  return 0;
}

static void free_updown_ctx(void *p) { free(p); }

sd_bus_slot *on_service_updown(sd_bus *bus, const char *svc_name, service_updown_cb cb, void *ud) {
  struct updown_ctx *ctx = malloc(sizeof(*ctx));
  if (!ctx) {
    fprintf(stderr, "on_service_updown: out of memory\n");
    return NULL;
  }
  ctx->cb = cb;
  ctx->ud = ud;

  char match[256];
  snprintf(match, sizeof(match),
           "type='signal',sender='org.freedesktop.DBus',interface='org.freedesktop.DBus',"
           "member='NameOwnerChanged',arg0='%s'",
           svc_name);

  sd_bus_slot *slot = NULL;
  int r = sd_bus_add_match(bus, &slot, match, service_updown_trampoline, ctx);
  if (r < 0) {
    fprintf(stderr, "sd_bus_add_match NameOwnerChanged: %s\n", strerror(-r));
    free(ctx);
    return NULL;
  }

  r = sd_bus_slot_set_destroy_callback(slot, free_updown_ctx);
  if (r < 0) {
    fprintf(stderr, "sd_bus_slot_set_destroy_callback: %s\n", strerror(-r));
    sd_bus_slot_unref(slot);
    free(ctx);
    return NULL;
  }

  return slot;
}
