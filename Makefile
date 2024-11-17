
all: wl-display-toggle/wl-display-toggle pipresencemon/pipresencemon hackswayimg/hackimg

build: wl-display-toggle/wl-display-toggle pipresencemon/pipresencemon hackswayimg/hackimg
	mkdir -p build/bin
	cp pipresencemon/pipresencemon hackswayimg/hackimg wl-display-toggle/wl_display_toggle build/bin/
	rm -rf build/cfg build/scripts
	cp -r cfg/ build/
	cp -r scripts/ build/
	mkdir -p build/stockimgs
	cp -r stockimgs/*.jpg build/stockimgs

deploytgt: build
	rsync --recursive --verbose ./build/* batman@10.0.0.146:/home/batman/homeboard/

wl-display-toggle/wl-display-toggle:
	make -C wl-display-toggle

pipresencemon/pipresencemon:
	make -C pipresencemon

hackswayimg/hackimg:
	make -C hackswayimg
