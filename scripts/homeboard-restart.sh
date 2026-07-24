#!/bin/bash
sudo systemctl restart \
    homeboard-ambience \
    homeboard-dbus-mqtt-bridge \
    homeboard-display-mgr \
    homeboard-doctor \
    homeboard-occupancy-sensor \
    homeboard-presence-service \
    homeboard-photo-provider
