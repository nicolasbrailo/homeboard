#!/bin/bash
exec journalctl -f \
    -u ambience \
    -u dbus-mqtt-bridge \
    -u display-mgr \
    -u occupancy-sensor \
    -u photo-provider \
    "$@"
