
TARGET_IP=10.0.0.93

all: wl-display-toggle/wl-display-toggle pipresencemon/pipresencemon hackswayimg/hackimg pi_gpio_mon/gpiomon hackwaytext/hackwaytext

install_sysroot_deps:
	make -C wl-display-toggle install_sysroot_deps
	make -C pipresencemon install_sysroot_deps
	make -C pi_gpio_mon install_sysroot_deps
	make -C hackwaytext install_sysroot_deps
	make -C hackswayimg install_sysroot_deps

install_system_deps:
	sudo apt-get -y install clang-format
	# wayland headers
	sudo apt-get -y install libwayland-dev
	# wlroots protocol files, needed for wlr-output-power-management-unstable-v1.xml
	sudo apt-get -y install libwlroots-dev
	# Install protocols.xml into /usr/share/wayland-protocols, not sure if needed
	sudo apt-get -y install wayland-protocols

build: wl-display-toggle/wl-display-toggle pipresencemon/pipresencemon hackswayimg/hackimg pi_gpio_mon/gpiomon hackwaytext/hackwaytext
	mkdir -p build/bin
	cp pipresencemon/pipresencemon \
		 hackswayimg/hackimg \
		 wl-display-toggle/wl_display_toggle \
		 pi_gpio_mon/gpiomon \
		 hackwaytext/hackwaytext \
		 	 build/bin/
	rm -rf build/cfg build/scripts
	cp -r cfg/ build/
	cp -r scripts/ build/
	mkdir -p build/stockimgs
	cp -r stockimgs/*.jpg build/stockimgs

deploytgt: build
	rsync --recursive --verbose ./build/* batman@$(TARGET_IP):/home/batman/homeboard/

.PHONY: setup-ssh
KEY_PATH="$(HOME)/.ssh/id_rsa.pub"
setup-ssh:
	if [ ! -f $(KEY_PATH) ]; then \
		echo "ssh pub key not found in $(KEY_PATH), run 'ssh-keygen -t rsa -b 4096'"; \
		exit 1; \
	fi
	ssh-copy-id batman@$(TARGET_IP)

.PHONY: wl-display-toggle/wl-display-toggle
wl-display-toggle/wl-display-toggle:
	make -C wl-display-toggle

.PHONY: pipresencemon/pipresencemon
pipresencemon/pipresencemon:
	make -C pipresencemon

.PHONY: hackswayimg/hackimg
hackswayimg/hackimg:
	make -C hackswayimg

.PHONY: pi_gpio_mon/gpiomon
pi_gpio_mon/gpiomon:
	make -C pi_gpio_mon

.PHONY: hackwaytext/hackwaytext
hackwaytext/hackwaytext:
	make -C hackwaytext



