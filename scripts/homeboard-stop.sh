#!/bin/bash
sudo systemctl stop \
    -u homeboard-ambience \
    -u homeboard-dbus-mqtt-bridge \
    -u homeboard-display-mgr \
    -u homeboard-occupancy-sensor \
    -u homeboard-photo-provider
