XDG_RUNTIME_DIR=/home/batman/run DISPLAY="" WAYLAND_DISPLAY=wayland-1 \
        /usr/lib/arm-linux-gnueabihf/ld-linux-armhf.so.3 \
                /home/batman/homeboard/bin/hackswayimg \
                /home/batman/homeboard/cfg/mostlyhackedswayimg.cfg \
                $@

