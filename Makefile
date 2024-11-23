
all: wl-display-toggle/wl-display-toggle pipresencemon/pipresencemon hackswayimg/hackimg pi_gpio_mon/gpiomon

build: wl-display-toggle/wl-display-toggle pipresencemon/pipresencemon hackswayimg/hackimg pi_gpio_mon/gpiomon
	mkdir -p build/bin
	cp pipresencemon/pipresencemon hackswayimg/hackimg wl-display-toggle/wl_display_toggle pi_gpio_mon/gpiomon build/bin/
	rm -rf build/cfg build/scripts
	cp -r cfg/ build/
	cp -r scripts/ build/
	mkdir -p build/stockimgs
	cp -r stockimgs/*.jpg build/stockimgs

deploytgt: build
	rsync --recursive --verbose ./build/* batman@10.0.0.146:/home/batman/homeboard/

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

