.PHONY: clean xcompile-start xcompile-end format

DEPLOY_TGT_HOST=batman@10.0.0.78
DEPLOY_TGT_DIR=/home/batman/homeboard/
DEPLOY_USER=batman
SSH ?= ssh $(DEPLOY_TGT_HOST)

XCOMPILE+=\
	-target arm-linux-gnueabihf \
	-mcpu=arm1176jzf-s \
	--sysroot ~/src/xcomp-rpiz-env/mnt/  \
	-isystem ~/src/xcomp-rpiz-env/mnt/usr/include/libdrm

CFLAGS+=\
	$(XCOMPILE) \
	-fdiagnostics-color=always \
	-ffunction-sections -fdata-sections \
	-ggdb -O3 \
	-std=gnu99 \
	-Wall -Werror -Wextra -Wpedantic \
	-Wendif-labels \
	-Wfloat-equal \
	-Wformat=2 \
	-Wimplicit-fallthrough \
	-Winit-self \
	-Winvalid-pch \
	-Wmissing-field-initializers \
	-Wmissing-include-dirs \
	-Woverflow \
	-Wpointer-arith \
	-Wredundant-decls \
	-Wstrict-aliasing=2 \
	-Wundef \
	-Wuninitialized \
	-Wno-strict-prototypes \
	-Wno-unused-function \
	-Wno-unused-parameter \
	-Wno-c23-extensions \

xcompile-start:
	../rpiz-xcompile/mount_rpy_root.sh ~/src/xcomp-rpiz-env

xcompile-end:
	../rpiz-xcompile/umount_rpy_root.sh ~/src/xcomp-rpiz-env

clean:
	rm -rf build

format:
	clang-format -i $(SRCS) $(wildcard *.h)

OBJS = $(patsubst %.c,build/%.o,$(notdir $(SRCS)))
VPATH = $(sort $(dir $(SRCS)))

build/%.o: %.c
	mkdir -p build
	@if [ ! -d ~/src/xcomp-rpiz-env/mnt/lib/raspberrypi-sys-mods ]; then \
		echo "xcompiler sysroot not detected, try 'make xcompile-start'"; \
		exit 1; \
	fi ;
	clang $(CFLAGS) $< -c -o $@

.PHONY: $(BIN_NAME)
$(BIN_NAME):build/$(BIN_NAME)
build/$(BIN_NAME): $(OBJS)
	clang $(CFLAGS) $^ $(LDFLAGS) -o $@

deploy-bin: build/$(BIN_NAME)
	scp ./build/$(BIN_NAME) $(DEPLOY_TGT_HOST):$(DEPLOY_TGT_DIR)/bin/
deploy-config:
	@if [ -f config.json ]; then \
		scp config.json $(DEPLOY_TGT_HOST):$(DEPLOY_TGT_DIR)/etc/$(BIN_NAME).json; \
	else \
		echo "Skipping config for $(BIN_NAME)"; \
	fi
deploy-dbus-policy:
	@if [ -n "$(DBUS_POLICY)" ]; then \
		scp $(DBUS_POLICY) $(DEPLOY_TGT_HOST):/tmp/$(DBUS_POLICY) && \
		$(SSH) 'sudo -S install -m 644 -o root -g root /tmp/$(DBUS_POLICY) /etc/dbus-1/system.d/$(DBUS_POLICY) && rm /tmp/$(DBUS_POLICY) && sudo systemctl reload dbus'; \
	else \
		echo "Skipping dbus policy for $(BIN_NAME)"; \
	fi

.PHONY: install-systemd
install-systemd:
	@mkdir -p build
	@if [ -f config.json ]; then \
		EXEC_CMD="$(DEPLOY_TGT_DIR)/bin/$(BIN_NAME) $(DEPLOY_TGT_DIR)/etc/$(BIN_NAME).json"; \
	else \
		EXEC_CMD="$(DEPLOY_TGT_DIR)/bin/$(BIN_NAME)"; \
	fi; \
	DEP_UNITS=""; \
	for dep in $(SERVICE_DEPS); do \
		if [ -z "$$DEP_UNITS" ]; then DEP_UNITS="$$dep.service"; \
		else DEP_UNITS="$$DEP_UNITS $$dep.service"; fi; \
	done; \
	{ \
		echo "[Unit]"; \
		echo "Description=homeboard $(BIN_NAME)"; \
		if [ -n "$$DEP_UNITS" ]; then \
			echo "After=network.target dbus.service $$DEP_UNITS"; \
			echo "Wants=$$DEP_UNITS"; \
		else \
			echo "After=network.target dbus.service"; \
		fi; \
		echo ""; \
		echo "[Service]"; \
		echo "Type=simple"; \
		echo "User=$(DEPLOY_USER)"; \
		echo "ExecStart=$$EXEC_CMD"; \
		echo "Restart=on-failure"; \
		echo "RestartSec=5"; \
		echo ""; \
		echo "[Install]"; \
		echo "WantedBy=multi-user.target"; \
	} > build/$(BIN_NAME).service
	scp build/$(BIN_NAME).service $(DEPLOY_TGT_HOST):/tmp/$(BIN_NAME).service
	$(SSH) 'sudo install -m 644 -o root -g root /tmp/$(BIN_NAME).service /etc/systemd/system/$(BIN_NAME).service && rm /tmp/$(BIN_NAME).service && sudo systemctl daemon-reload && sudo systemctl enable $(BIN_NAME).service && sudo systemctl restart $(BIN_NAME).service'

