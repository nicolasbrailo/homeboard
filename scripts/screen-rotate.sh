#!/usr/bin/bash

set -euo pipefail

CFGF=~/.config/wayfire.ini
DIRECTION=180

cat << EOF > "$CFGF"
[output:HDMI-A-1]
mode = 1920X1080@60.00
position = 0,0
transform = $DIRECTION
EOF


