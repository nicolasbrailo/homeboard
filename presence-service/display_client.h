#pragma once

// Outbound client for io.homeboard.Display.
// Object isn't thread safe due to underlying dbus usage.
// On / off return 0 on success, negative on failure (error logged).

struct DisplayClient;

struct DisplayClient *display_client_init(void);
void display_client_free(struct DisplayClient *c);

int display_client_on(struct DisplayClient *c);
int display_client_off(struct DisplayClient *c);
