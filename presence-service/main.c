#include "config.h"
#include "dbus.h"
#include "display_client.h"
#include "presence.h"

#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>

static volatile sig_atomic_t g_quit;

static void sig_handler(int sig) {
  (void)sig;
  g_quit = 1;
}

struct ctx {
  struct Presence *presence;
  struct PresenceDBus *dbus;
  struct DisplayClient *display;
};

static void on_presence_change(void *ud, bool present) {
  struct ctx *c = ud;
  printf("Presence=%s\n", present ? "true" : "false");
  // Drive the display in lockstep with the presence transition: panel on
  // before announcement, panel off after, so subscribers reacting to
  // PresenceChanged(true) see the display already lit.
  if (present)
    display_client_on(c->display);
  presence_dbus_emit_changed(c->dbus, present);
  if (!present)
    display_client_off(c->display);
}

static void on_force_on(void *ud) {
  presence_force_on(((struct ctx *)ud)->presence);
}
static void on_force_off(void *ud) {
  presence_force_off(((struct ctx *)ud)->presence);
}

static void on_sensor_report(void *ud, bool occupied) {
  presence_on_report(((struct ctx *)ud)->presence, occupied);
}

static void on_sensor_updown(void *ud, bool up) {
  struct ctx *c = ud;
  if (up)
    presence_on_sensor_up(c->presence);
  else
    presence_on_sensor_down(c->presence);
}

static struct presence_dbus_cbs g_dbus_cbs = {
    .on_force_on = on_force_on,
    .on_force_off = on_force_off,
    .on_sensor_report = on_sensor_report,
    .on_sensor_updown = on_sensor_updown,
};

int main(int argc, char *argv[]) {
  if (argc < 2) {
    fprintf(stderr, "Usage: %s <config.json>\n", argv[0]);
    return 1;
  }

  struct presence_config cfg;
  if (presence_config_load(argv[1], &cfg) < 0)
    return 1;

  struct ctx c = {
      .presence = presence_init(cfg.sensor_stabilization_delay_secs,
                                cfg.hysteresis_occupied, cfg.hysteresis_vacant,
                                on_presence_change, &c),
      .dbus = presence_dbus_init(&g_dbus_cbs, &c),
      .display = display_client_init(),
  };

  int ret = 0;
  if (!c.presence || !c.dbus || !c.display) {
    fprintf(stderr, "init failed\n");
    ret = 1;
    goto exit;
  }

  // Force display off at startup: presence-service is the sole owner of
  // Display.On/Off, so the display must reflect "no presence" until we
  // decide otherwise.
  display_client_off(c.display);

  signal(SIGTERM, sig_handler);
  signal(SIGINT, sig_handler);
  printf("Service is now running\n");
  while (!g_quit) {
    if (presence_dbus_run_once(c.dbus) < 0)
      break;
  }

exit:
  presence_free(c.presence);
  presence_dbus_free(c.dbus);

  if (c.display) {
    // Best-effort: leave the display off as we exit so the panel doesn't
    // stay lit after presence-service goes away.
    display_client_off(c.display);
    display_client_free(c.display);
  }

  return ret;
}
