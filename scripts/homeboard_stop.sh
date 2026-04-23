#!/bin/bash
exec sudo systemctl stop \
    ambience \
    dbus-mqtt-bridge \
    display-mgr \
    occupancy-sensor \
    photo-provider
