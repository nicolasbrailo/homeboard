# Homeboard V2

V2 of Picture frame + home board: https://nicolasbrailo.github.io/blog/projects_texts/24homeboard.html

This version has no Wayland dependencies, instead writing to the DRM for image rendering.


# OS setup

1. Create Raspberry PI OS as usual.
1. Headless setup: write echo "batman:`echo 'mypassword' | openssl passwd -6 -stdin`" > bootfs/userconf.txt (or maybe rootfs? Try both just in case)
1. Also `touch bootfs/ssh` and `touch rootfs/ssh`
1. After bootup, ssh should be available
1. Set up ssh pub keys `ssh-copy-id batman@$IP`
1. Convenience tools: `sudo apt install vim tmux`
1. Disable GUI: ```
sudo systemctl set-default multi-user.target
sudo systemctl disable lightdm

# Disable autologin
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
1. At this point, before installing systemd units, it's a good idea to test if services run as expected, and if all GPIO connections are set up properly.

The target should be ready to run now.



# TODO

* Make the eink pins runtime config?
* eink, verify why partial update isn't working
* if eInk fails on startup then we never recover -> Should retry a few times?

