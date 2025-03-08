#!/usr/bin/bash
set -euo pipefail

HOMEBOARD_ROOT=/home/batman/homeboard

function install_systemd_svc() {
  svc_name="$1"
  if [ -f "/etc/systemd/system/$svc_name.service" ]; then
    echo "$svc_name is a service, no need to install in systemd"
  else
    echo "Installing $svc_name service"
    sudo ln -s "/etc/systemd/system/$svc_name.service" "$HOMEBOARD_ROOT/cfg/$svc_name.service"
    sudo systemctl daemon-reload
    sudo systemctl enable "$svc_name"
    sudo systemctl start "$svc_name"
    sudo systemctl status "$svc_name"
  fi
}


[ -d "$HOMEBOARD_ROOT" ] || { echo "Can't find Homeboard root at $HOMEBOARD_ROOT"; exit 1; }
[ $(which wayfire) ] || { echo "Wayfire isn't installed"; exit 1; }

echo "This script will install Homeboard services"

install_systemd_svc wayfire
install_systemd_svc pipresencemonsvc

