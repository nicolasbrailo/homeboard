
TARGET_IP=10.0.0.93

all: wl-display-toggle/wl-display-toggle pipresencemon/pipresencemon hackswayimg/hackimg pi_gpio_mon/gpiomon hackwaytext/hackwaytext

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



