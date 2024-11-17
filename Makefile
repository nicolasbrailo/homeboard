
all: wl-display-toggle/wl-display-toggle pipresencemon/pipresencemon hackswayimg/hackimg

build: wl-display-toggle/wl-display-toggle pipresencemon/pipresencemon hackswayimg/hackimg
	mkdir -p build/bin
	cp pipresencemon/pipresencemon hackswayimg/hackimg wl-display-toggle/wl_display_toggle build/bin/

wl-display-toggle/wl-display-toggle:
	make -C wl-display-toggle

pipresencemon/pipresencemon:
	make -C pipresencemon

hackswayimg/hackimg:
	make -C hackswayimg