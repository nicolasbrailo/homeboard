#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <systemd/sd-bus.h>

#include "dbus.h"
#include "drm.h"

static sd_bus *g_bus;
static sd_bus_slot *g_vtable_slot;
static sd_bus_track *g_lease_track;

static struct DRM_state *g_drm;
static bool g_display_on;
static pid_t g_lease_pid;

static void set_display_on(bool on) {
  if (g_display_on == on)
    return;
  if (on)
    drm_display_on(g_drm);
  else
    drm_display_off(g_drm);
  g_display_on = on;
  printf("Display switching %s\n", on ? "on" : "off");

  // Emit new state on dbus
  int r = sd_bus_emit_signal(g_bus, DBUS_PATH, DBUS_INTERFACE, "StateChanged", "s", on ? "on" : "off");
  if (r < 0)
    fprintf(stderr, "sd_bus_emit_signal: %s\n", strerror(-r));
}

static int method_on(sd_bus_message *m, void *ud, sd_bus_error *err) {
  (void)ud;
  (void)err;
  set_display_on(true);
  return sd_bus_reply_method_return(m, NULL);
}

static int method_off(sd_bus_message *m, void *ud, sd_bus_error *err) {
  (void)ud;
  (void)err;
  set_display_on(false);
  return sd_bus_reply_method_return(m, NULL);
}

static int method_status(sd_bus_message *m, void *ud, sd_bus_error *err) {
  (void)ud;
  (void)err;
  return sd_bus_reply_method_return(m, "s", g_display_on ? "on" : "off");
}

static pid_t sender_pid(sd_bus_message *m) {
  sd_bus_creds *creds = NULL;
  pid_t pid = 0;
  if (sd_bus_query_sender_creds(m, SD_BUS_CREDS_PID, &creds) >= 0) {
    sd_bus_creds_get_pid(creds, &pid);
    sd_bus_creds_unref(creds);
  }
  return pid;
}

static int on_lease_track_drop(sd_bus_track *track, void *ud) {
  (void)track;
  (void)ud;
  printf("Lease holder (pid %d) disappeared without releasing. Cleaning up.\n", g_lease_pid);
  sd_bus_track_unref(g_lease_track);
  g_lease_track = NULL;
  g_lease_pid = 0;
  return 0;
}

static int method_acquire_lease(sd_bus_message *m, void *ud, sd_bus_error *err) {
  (void)ud;
  const char *sender = sd_bus_message_get_sender(m);
  if (!sender)
    return sd_bus_error_set(err, "io.homeboard.Display.Error.NoSender", "cannot identify sender");

  pid_t req_pid = sender_pid(m);

  if (g_lease_track && !sd_bus_track_contains(g_lease_track, sender)) {
    printf("Lease requested by PID %d, but already owned by PID %d\n", req_pid, g_lease_pid);
    return sd_bus_error_setf(err, "io.homeboard.Display.Error.LeaseHeld", "lease held by another client");
  }

  if (!g_lease_track) {
    int r = sd_bus_track_new(g_bus, &g_lease_track, on_lease_track_drop, NULL);
    if (r < 0)
      return sd_bus_error_set_errno(err, -r);
    r = sd_bus_track_add_sender(g_lease_track, m);
    if (r < 0) {
      sd_bus_track_unref(g_lease_track);
      g_lease_track = NULL;
      return sd_bus_error_set_errno(err, -r);
    }
    g_lease_pid = req_pid;
  }

  const struct fb_info *fbi = drm_get_fb_info(g_drm);
  printf("Lease granted to %s (pid %d)\n", sender, req_pid);

  return sd_bus_reply_method_return(m, "huuuuu", drm_get_dmabuf_fd(g_drm), fbi->width, fbi->height, fbi->stride,
                                    fbi->format, fbi->size);
}

static int method_release_lease(sd_bus_message *m, void *ud, sd_bus_error *err) {
  (void)ud;
  const char *sender = sd_bus_message_get_sender(m);
  if (!g_lease_track || !sender || !sd_bus_track_contains(g_lease_track, sender)) {
    printf("PID %d requested lease release, but is not the owner (owner is PID %d)\n", sender_pid(m), g_lease_pid);
    return sd_bus_error_set(err, "io.homeboard.Display.Error.NotLeaseHolder", "caller does not hold the lease");
  }

  sd_bus_track_unref(g_lease_track);
  g_lease_track = NULL;
  printf("lease released by %s (pid %d)\n", sender, g_lease_pid);
  g_lease_pid = 0;
  return sd_bus_reply_method_return(m, NULL);
}

static const sd_bus_vtable g_vtable[] = {
    SD_BUS_VTABLE_START(0),
    SD_BUS_METHOD("On", "", "", method_on, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("Off", "", "", method_off, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("Status", "", "s", method_status, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("AcquireLease", "", "huuuuu", method_acquire_lease, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("ReleaseLease", "", "", method_release_lease, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_SIGNAL("StateChanged", "s", 0),
    SD_BUS_VTABLE_END,
};

int dbus_init(struct DRM_state *drm) {
  g_drm = drm;
  g_display_on = drm_is_display_on(drm);

  int r = sd_bus_open_system(&g_bus);
  if (r < 0) {
    fprintf(stderr, "sd_bus_open_system: %s\n", strerror(-r));
    return -1;
  }

  r = sd_bus_add_object_vtable(g_bus, &g_vtable_slot, DBUS_PATH, DBUS_INTERFACE, g_vtable, NULL);
  if (r < 0) {
    fprintf(stderr, "sd_bus_add_object_vtable: %s\n", strerror(-r));
    return -1;
  }

  r = sd_bus_request_name(g_bus, DBUS_SERVICE, 0);
  if (r < 0) {
    fprintf(stderr, "sd_bus_request_name(%s): %s\n", DBUS_SERVICE, strerror(-r));
    return -1;
  }

  return 0;
}

void dbus_free(void) {
  sd_bus_track_unref(g_lease_track);
  g_lease_track = NULL;
  sd_bus_slot_unref(g_vtable_slot);
  g_vtable_slot = NULL;
  if (g_bus) {
    sd_bus_flush_close_unref(g_bus);
    g_bus = NULL;
  }
}

int dbus_run_once(int timeout_ms) {
  int r = sd_bus_process(g_bus, NULL);
  if (r < 0)
    return r;
  if (r > 0)
    return 0;
  r = sd_bus_wait(g_bus, (uint64_t)timeout_ms * 1000ULL);
  if (r < 0 && -r != EINTR)
    return r;
  return 0;
}
