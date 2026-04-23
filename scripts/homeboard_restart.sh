#!/bin/bash
exec sudo systemctl restart \
    ambience \
    dbus-mqtt-bridge \
    display-mgr \
    occupancy-sensor \
    photo-provider
