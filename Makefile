TARGET_IP=StonebakedMargheritaHomeboard

all: build

.PHONY: \
	build \
	clean \
	ambiencesvc/ambiencesvc \
	hackswaytext/hackswaytext \
	mostlyhackswayimg/hackswayimg \
	pigpiomon/gpiomon \
	pipresencemonsvc/pipresencemonsvc \
	wl-display-toggle/wl-display-toggle
ambiencesvc/ambiencesvc:
	make -C ambiencesvc
hackswaytext/hackswaytext:
	make -C hackswaytext
mostlyhackswayimg/hackswayimg:
	make -C mostlyhackswayimg
pigpiomon/gpiomon:
	make -C pigpiomon
pipresencemonsvc/pipresencemonsvc:
	make -C pipresencemonsvc
wl-display-toggle/wl-display-toggle:
	make -C wl-display-toggle

clean:
	rm -rf build
	make -C ambiencesvc clean
	make -C hackswaytext clean
	make -C mostlyhackswayimg clean
	make -C pigpiomon clean
	make -C pipresencemonsvc clean
	make -C wl-display-toggle clean

build: \
		ambiencesvc/ambiencesvc \
		hackswaytext/hackswaytext \
		mostlyhackswayimg/hackswayimg \
		pigpiomon/gpiomon \
		pipresencemonsvc/pipresencemonsvc \
		wl-display-toggle/wl-display-toggle
	
	rm -rf build/cfg build/scripts build/stockimgs
	mkdir -p build/bin
	mkdir -p build/cfg
	
	cp pigpiomon/gpiomon build/bin
	cp wl-display-toggle/wl-display-toggle build/bin
	cp mostlyhackswayimg/hackswayimg build/bin
	cp pipresencemonsvc/pipresencemonsvc build/bin
	cp ambiencesvc/ambiencesvc build/bin
	cp hackswaytext/hackswaytext build/bin
	
	cp -r stockimgs/ build/
	cp -r scripts/ build/
	cp -r cfg/ build/
	cp ambiencesvc/config.json build/cfg/ambiencesvc.json
	cp pipresencemonsvc/pipresencemon.cfg build/cfg/pipresencemon.cfg

deploytgt: build
	rsync --recursive --verbose ./build/* batman@$(TARGET_IP):/home/batman/homeboard/

install_sysroot_deps:
	make -C wl-display-toggle install_sysroot_deps
	make -C pipresencemonsvc install_sysroot_deps
	make -C ambiencesvc install_sysroot_deps
	make -C pi_gpio_mon install_sysroot_deps
	make -C hackswaytext install_sysroot_deps
	make -C mostlyhackswayimg install_sysroot_deps

install_system_deps:
	sudo apt-get -y install clang-format
	# wayland headers
	sudo apt-get -y install libwayland-dev
	# wlroots protocol files, needed for wlr-output-power-management-unstable-v1.xml
	sudo apt-get -y install libwlroots-dev
	# Install protocols.xml into /usr/share/wayland-protocols, not sure if needed
	sudo apt-get -y install wayland-protocols

.PHONY: setup-ssh
KEY_PATH="$(HOME)/.ssh/id_rsa.pub"
setup-ssh:
	if [ ! -f $(KEY_PATH) ]; then \
		echo "ssh pub key not found in $(KEY_PATH), run 'ssh-keygen -t rsa -b 4096'"; \
		exit 1; \
	fi
	ssh-copy-id batman@$(TARGET_IP)

