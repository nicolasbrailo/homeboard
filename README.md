# Homeboard V2

V2 of Picture frame + home board: https://nicolasbrailo.github.io/blog/projects_texts/24homeboard.html

TODO: project picture

TODO: project description

# Build

1. You will need a picture frame. ./mount-designs have an option for a laser engraver; it consists of a main board to mount all of the circuits, and a frame panel, to mount the display to an Ikea picture frame. If using this, make sure your build is as flat as possible, as there isn't more than 20mm of space to play with (eg caps should be as horizontal as possible).
1. TODO: assembly instructions
1. TODO: Wiring instructions
1. For eInk wiring, check ./eink-write/README.md; you can change wiring, but you will need to udpate the pins (which are hardcoded in c today)
1. For mmWave sensor wiring, check ./occupancy-sensor-ld2410s/README.md
1. Recommended: [add capacitors to any voltage converter you have](https://nicolasbrailo.github.io/blog/2026/0423_HomeboardN1.html), especially if you are working close to the power limit of the system.

Rpi pinout: https://images.theengineeringprojects.com/image/webp/2021/03/raspberry-pi-zero-5.png.webp


# OS setup

1. Create Raspberry PI OS as usual.
1. Headless setup: write echo "batman:`echo 'mypassword' | openssl passwd -6 -stdin`" > bootfs/userconf.txt (or maybe rootfs? Try both just in case)
1. Also `touch bootfs/ssh` and `touch rootfs/ssh`
1. After bootup, ssh should be available
1. Set up ssh pub keys `ssh-copy-id batman@$IP`
1. Convenience tools: `sudo apt install vim tmux`
1. Groups you'll need: `sudo usermod -aG dialout,sudo,video,users,gpio,spi,systemd-journal $USER`
1. Configure UART for mmWave sensor:
    - Add `enable_uart=1` and `dtoverlay=disable-bt` to /boot/firmware/config.txt
    - Disable services that try to use UART: `sudo systemctl disable --now serial-getty@ttyAMA0.service serial-getty@serial0.service` and `sudo systemctl disable --now hciuart`
    - Remove any `console=serial*` from cmdline
    - Alternativelty, run all of these from `make -C occupancy-sensor-ld2410s config-target`
1. Optional: add `export PATH=$PATH:/home/$USER/homeboard/bin` to bashrc
1. Install project deps not statically linked in: `sudo apt install libmosquitto1`
1. Disable GUI: ```
sudo systemctl set-default multi-user.target
sudo systemctl disable lightdm
sudo rm /etc/systemd/system/getty@tty1.service.d/autologin.conf
sudo systemctl daemon-reload
sudo reboot
```
1. Make bootup faster, disable services we won't use: ```
sudo systemctl disable --now bluetooth.service wpa_supplicant.service
sudo systemctl disable --now lightdm.service
sudo systemctl disable --now plymouth-start.service plymouth-quit-wait.service
sudo systemctl disable --now glamor-test.service
sudo systemctl disable --now accounts-daemon.service
sudo systemctl disable --now packagekit.service
systemctl disable --now cloud-init-main cloud-init-local cloud-config cloud-final
sudo touch /etc/cloud/cloud-init.disabled
```

The target OS should now be ready to deploy xcompiled binaries:

# Project build

In the build machine:

1. Setup deps: `sudo apt-get install build-essential clang clang-format gcc-arm-linux-gnueabi gcc-arm-linux-gnueabihf`
1. Ensure common.mk has xcompile target (if not building locally) and setup the target IP in DEPLOY_TGT_HOST. Also update IP in root Makefile.
1. Build test project: `cd xcompile-test && make && file build/xcompile-test`
1. `make deploy` will deploy the entire project to the target, and it will install configs and dbus policies, but no systemd targets.

At this point, before installing systemd units, it's a good idea to test if services run as expected, and if all GPIO connections are set up properly. For this, you can launch each of the services in a tmux pane:

- homeboard-display-mgr; this will control the DRM
- homeboard-photo-provider; provides new pictures to the ambience service
- homeboard-occupancy-sensor-ld2410s; interfaces with mmWave sensor
- homeboard-presence-service; a layer on top of the occupancy sensor to determine when a person is close or not
- homeboard-dbus-mqtt-bridge; external MQTT interface to Homeboard
- homeboard-ambience; main slideshow, overlays, eInk management

If everything goes well, the target `make install-systemd` can install each service. Reboot to make sure nothing breaks.


# TODO

* Make the eink pins runtime config?
* eink, verify why partial update isn't working
* if eInk fails on startup then we never recover -> Should retry a few times?
* Need to handle ENOTCONN for photo client (any other risky call sites for ENOTCONN?)

