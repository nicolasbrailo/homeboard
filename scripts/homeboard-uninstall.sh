#!/bin/bash
set -u

SERVICES=(
    homeboard-ambience \
    homeboard-dbus-mqtt-bridge \
    homeboard-display-mgr \
    homeboard-occupancy-sensor \
    homeboard-photo-provider
)

for svc in "${SERVICES[@]}"; do
    sudo systemctl stop "$svc.service" 2>/dev/null || true
    sudo systemctl disable "$svc.service" 2>/dev/null || true
    sudo rm -f "/etc/systemd/system/$svc.service"
done

sudo systemctl daemon-reload
sudo systemctl reset-failed
