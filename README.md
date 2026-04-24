# Homeboard V2

V2 of Picture frame + home board: https://nicolasbrailo.github.io/blog/projects_texts/24homeboard.html

This version has no Wayland dependencies, instead writing to the DRM for image rendering.


# OS setup

Create user, disable desktop:

```
sudo adduser batman
sudo usermod -aG sudo batman

sudo systemctl set-default multi-user.target
sudo systemctl disable lightdm

# Disable autologin
sudo rm /etc/systemd/system/getty@tty1.service.d/autologin.conf
sudo systemctl daemon-reload
sudo reboot
```

After reboot, remove default user (log in as new user)

```
sudo killall -u pi
sudo userdel -r pi
```

Setup ssh keys:

```
ssh-copy-id batman@$IP
```

# Project build

- Setup deps: `sudo apt-get install build-essential clang clang-format gcc-arm-linux-gnueabi gcc-arm-linux-gnueabihf`
- Build test project: `cd xcompile-test && make && file build/xcompile-test`
- `make deploy`

This should verify the full dev cycle works (cross compile, deploy to target). Try running the binary on the target, too.



# TODO

* Make the eink pins runtime config?
* eink, verify why partial update isn't working
* Add announcement overlay (overlay text on top of picture, with timeout)
* Use sides of the display for info, eg weather service
* Refactor ambience service to use a single dbus interface, to clean up the NULL sender from the mqtt bridge
* if eInk fails on startup then we never recover -> Should retry a few times?
* Need to re-register when screen wakes up; the service is persistent on the homeboard, but the server may have evicted the client and the settings won't persist

