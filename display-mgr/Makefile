BIN_NAME = homeboard-display-mgr
SRCS = main.c drm.c dbus.c
LDFLAGS += -Wl,-Bstatic -ldrm -Wl,-Bdynamic -lsystemd -lpthread
DBUS_POLICY = io.homeboard.Display.conf
all: $(BIN_NAME)

include ../common.mk

SSH = ssh $(DEPLOY_TGT_HOST)

install_sysroot_deps:
	../rpiz-xcompile/add_sysroot_pkg.sh ~/src/xcomp-rpiz-env http://raspbian.raspberrypi.com/raspbian/pool/main/libd/libdrm/libdrm-dev_2.4.131-1+rpi1_armhf.deb
	../rpiz-xcompile/add_sysroot_pkg.sh ~/src/xcomp-rpiz-env http://raspbian.raspberrypi.com/raspbian/pool/main/s/systemd/libsystemd-dev_252.39-1~deb12u1+rpi1_armhf.deb
	../rpiz-xcompile/add_sysroot_pkg.sh ~/src/xcomp-rpiz-env http://raspbian.raspberrypi.com/raspbian/pool/main/libc/libcap2/libcap-dev_2.66-4+deb12u2_armhf.deb

set_perms:
	ssh "$(DEPLOY_TGT_HOST)" sudo usermod -aG video batman
	ssh "$(DEPLOY_TGT_HOST)" "sudo setcap cap_sys_admin+ep display-mgr"
