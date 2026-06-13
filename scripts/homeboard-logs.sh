#!/bin/bash
journalctl -f \
    -u homeboard-ambience \
    -u homeboard-dbus-mqtt-bridge \
    -u homeboard-display-mgr \
    -u homeboard-occupancy-sensor \
    -u homeboard-presence-service \
    -u homeboard-photo-provider
    "$@"
