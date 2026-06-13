#!/bin/bash
sudo systemctl stop \
    homeboard-ambience \
    homeboard-dbus-mqtt-bridge \
    homeboard-display-mgr \
    homeboard-occupancy-sensor \
    homeboard-presence-service \
    homeboard-photo-provider
