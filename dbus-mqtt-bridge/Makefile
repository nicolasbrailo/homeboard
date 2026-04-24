BIN_NAME = dbus-mqtt-bridge
SERVICE_DEPS = ambience
SRCS = main.c config.c mqtt.c dbus_client.c
LDFLAGS += -Wl,-Bstatic -ljson-c \
           -Wl,-Bdynamic -lmosquitto -lsystemd

all: $(BIN_NAME)

include ../common.mk

install_sysroot_deps:
	../rpiz-xcompile/add_sysroot_pkg.sh ~/src/xcomp-rpiz-env http://raspbian.raspberrypi.com/raspbian/pool/main/m/mosquitto/libmosquitto1_2.0.11-1+deb11u2_armhf.deb
	../rpiz-xcompile/add_sysroot_pkg.sh ~/src/xcomp-rpiz-env http://raspbian.raspberrypi.com/raspbian/pool/main/m/mosquitto/libmosquitto-dev_2.0.11-1+deb11u2_armhf.deb
	../rpiz-xcompile/add_sysroot_pkg.sh ~/src/xcomp-rpiz-env http://raspbian.raspberrypi.com/raspbian/pool/main/o/openssl/libssl1.1_1.1.1w-0+deb11u5_armhf.deb
