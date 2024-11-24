mkdir -p logs
WAYLAND_DISPLAY=wayland-1 /home/batman/homeboard/bin/pipresencemon /home/batman/homeboard/cfg/pipresencemon.cfg | tee logs/pipresencemon.log

