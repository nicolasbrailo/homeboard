BIN_NAME = homeboard-doctor

# Mirror common.mk's deploy variables. This service is a bash script (not
# cross-compiled) and runs as root, so it doesn't include common.mk / the C
# build rules; it just provides the same target names the top-level Makefile
# loops over (all/clean/format/deploy-bin/deploy-config/deploy-dbus-policy/
# install-systemd).
LOCAL_BUILD ?= 0
ifeq ($(LOCAL_BUILD),1)
DEPLOY_TGT_HOST = batman@127.0.0.1
else
DEPLOY_TGT_HOST = batman@10.0.0.93
endif
DEPLOY_TGT_DIR = /home/batman/homeboard
SSH = ssh $(DEPLOY_TGT_HOST)

.PHONY: all clean format deploy-bin deploy-config deploy-dbus-policy \
        install-deps install-systemd

# "Build" = syntax-check the script.
all:
	bash -n $(BIN_NAME)

clean:
	@:

format:
	@command -v shfmt >/dev/null 2>&1 && shfmt -w $(BIN_NAME) || echo "shfmt not found, skipping"

deploy-bin:
	scp $(BIN_NAME) $(DEPLOY_TGT_HOST):$(DEPLOY_TGT_DIR)/bin/$(BIN_NAME)
	$(SSH) chmod +x $(DEPLOY_TGT_DIR)/bin/$(BIN_NAME)

deploy-config:
	scp config.json $(DEPLOY_TGT_HOST):$(DEPLOY_TGT_DIR)/etc/$(BIN_NAME).json

# Doctor owns no D-Bus name (it only sends to io.homeboard.Ambience, which the
# default policy already allows), so there is no policy to install.
deploy-dbus-policy:
	@echo "No dbus policy for $(BIN_NAME)"

# jq + mosquitto_pub are runtime deps of the script.
install-deps:
	$(SSH) 'sudo -S apt-get install -y jq mosquitto-clients'

install-systemd:
	@mkdir -p build
	{ \
		echo "[Unit]"; \
		echo "Description=homeboard $(BIN_NAME) (host/network health + self-heal)"; \
		echo "After=network.target dbus.service"; \
		echo ""; \
		echo "[Service]"; \
		echo "Type=simple"; \
		echo "ExecStart=$(DEPLOY_TGT_DIR)/bin/$(BIN_NAME) $(DEPLOY_TGT_DIR)/etc/$(BIN_NAME).json"; \
		echo "Restart=always"; \
		echo "RestartSec=5"; \
		echo "# Runs as root: needs to write /sys/bus/usb/.../ (un)bind to bounce the NIC."; \
		echo "User=root"; \
		echo "# Never let the OOM killer pick the health monitor."; \
		echo "OOMScoreAdjust=-500"; \
		echo ""; \
		echo "[Install]"; \
		echo "WantedBy=multi-user.target"; \
	} > build/$(BIN_NAME).service
	scp build/$(BIN_NAME).service $(DEPLOY_TGT_HOST):/tmp/$(BIN_NAME).service
	$(SSH) 'sudo -S install -m 644 -o root -g root /tmp/$(BIN_NAME).service /etc/systemd/system/$(BIN_NAME).service && rm /tmp/$(BIN_NAME).service && sudo -S systemctl daemon-reload && sudo -S systemctl enable $(BIN_NAME).service && sudo -S systemctl restart $(BIN_NAME).service'
