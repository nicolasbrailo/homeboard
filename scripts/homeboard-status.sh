#!/bin/bash
GREEN=$'\e[32m'
RED=$'\e[31m'
RESET=$'\e[0m'

for svc in \
    homeboard-ambience \
    homeboard-dbus-mqtt-bridge \
    homeboard-display-mgr \
    homeboard-occupancy-sensor \
    homeboard-photo-provider
do
    state=$(systemctl is-active "$svc")
    pid=$(systemctl show -p MainPID --value "$svc")
    case "$state" in
        active)     color=$GREEN ;;
        *)          color=$RED ;;
    esac
    if [ "$pid" != "0" ]; then
        cpu=$(ps -p "$pid" -o %cpu= 2>/dev/null | xargs)
        printf "%-32s ${color}%-10s${RESET} pid=%-7s cpu=%s%%\n" "$svc" "$state" "$pid" "$cpu"
    else
        printf "%-32s ${color}%s${RESET}\n" "$svc" "$state"
    fi
done
