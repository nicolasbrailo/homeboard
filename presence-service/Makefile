BIN_NAME = homeboard-presence-service
DBUS_POLICY = io.homeboard.Presence.conf
SERVICE_DEPS = homeboard-display-mgr homeboard-occupancy-sensor
SRCS = main.c presence.c dbus.c display_client.c config.c \
       ../lib/dbus_helpers/dbus_helpers.c
CFLAGS += -I../lib/
LDFLAGS += -Wl,-Bstatic -ljson-c -Wl,-Bdynamic -lsystemd -lpthread

all: $(BIN_NAME)

include ../common.mk

SSH = ssh $(DEPLOY_TGT_HOST)
tgt-force-on:
	$(SSH) 'busctl --system call io.homeboard.Presence /io/homeboard/Presence io.homeboard.Presence1 ForceOn'
tgt-force-off:
	$(SSH) 'busctl --system call io.homeboard.Presence /io/homeboard/Presence io.homeboard.Presence1 ForceOff'
dbus-listen:
	$(SSH) "dbus-monitor --system \"type='signal',interface='io.homeboard.Presence1'\""
