#!/bin/bash
sudo systemctl restart \
    homeboard-ambience \
    homeboard-dbus-mqtt-bridge \
    homeboard-display-mgr \
    homeboard-occupancy-sensor \
    homeboard-photo-provider
