.PHONY: clean deploytgt

all: hackswayimg

clean:
	rm -rf build hackswayimg

deploytgt: hackswayimg
	scp ./hackswayimg batman@StonebakedMargheritaHomeboard:/home/batman/hackswayimg


SYSROOT=/home/batman/src/xcomp-rpiz-env/mnt/
XCOMPILE=\
	-target arm-linux-gnueabihf \
	-mcpu=arm1176jzf-s \
	--sysroot $(SYSROOT)

# There is a linker option that messes with the loader, but I'm not sure which one
# This will force the right loder to be used
PI_LOADER=-Wl,--dynamic-linker=/usr/lib/arm-linux-gnueabihf/ld-linux-armhf.so.3

# Use XCOMPILE=/ for local build includes. You may need SYSROOT=/ too to regen wayland protos
#XCOMPILE=
#SYSROOT=
#PI_LOADER=
#X86_LIBS=-lsharpyuv -lbz2

SWAYBUILDEFS=\
	-DHAVE_INOTIFY=1 \
	-DHAVE_LIBJPEG=1 \
	-DHAVE_LIBPNG=1 \
	-DAPP_NAME='"hackswayimg"' \
	-DAPP_VERSION='"0.0.0"' \

#	-Wfloat-equal \

CFLAGS= \
	$(XCOMPILE) \
	$(SWAYBUILDEFS) \
	-I. \
	-I./src/ \
	-isystem ./build \
	-isystem $(SYSROOT)/usr/include/freetype2 \
	-isystem $(SYSROOT)/usr/include/json-c \
	-fdiagnostics-color=always \
	-ffunction-sections -fdata-sections \
	-ggdb -O3 \
	-std=gnu99 \
	-Wall -Werror -Wextra -Wpedantic \
	-Wendif-labels \
	-Wformat=2 \
	-Wimplicit-fallthrough \
	-Winit-self \
	-Winvalid-pch \
	-Wmissing-field-initializers \
	-Wmissing-include-dirs \
	-Wno-strict-prototypes \
	-Wno-unused-function \
	-Wno-unused-parameter \
	-Woverflow \
	-Wpointer-arith \
	-Wredundant-decls \
	-Wstrict-aliasing=2 \
	-Wundef \
	-Wuninitialized \
	-pthread \

# static linking like this seems to mess up loader, so run with /lib/ld-linux-armhf.so.3 ./hackswayimg
LDFLAGS=\
	-L $(SYSROOT)/usr/lib/arm-linux-gnueabihf \
	-static \
	-Wl,--as-needed \
	-Wl,--no-undefined \
	-Wl,-O1 \
	-Wl,--start-group \
	-lrt \
	-ljson-c \
	-lwebp \
	-lwebpdemux \
	-ljpeg \
	-lexpat \
	-lfreetype \
	-lfontconfig \
	-ltiff \
	-lpng16 \
	-lexif \
	-lz \
	-llzma \
	-lzstd \
	-ldeflate \
	-ljbig \
	-lLerc \
	-lgif \
	-lbrotlicommon \
	-lbrotlidec \
	$(X86_LIBS) \
	-pthread \
	-lstdc++ \
	-Wl,-Bdynamic \
	-lwayland-client \
	-lxkbcommon \
	-lm \
	-Wl,--end-group \
	$(PI_LOADER) \
	-v

build/%-protocol.o build/%-client-protocol.h:
	mkdir -p build
	
	@echo "Scanning for Wayland proto $* (will pick first found)" ;\
	PROTO_DEF=$$(find $(SYSROOT)/usr/share/wayland-protocols/ -type f | grep $* | head -n 1) ;\
	test ! -z $$PROTO_DEF || { echo "Can't find Wayland proto $*"; exit 1; } ;\
	test -f $$PROTO_DEF || { echo "Can't find Wayland proto $*: found non-file $$PROTO_DEF"; exit 1; } ;\
	echo "Found $$PROTO_DEF" ;\
	C_OUT=build/$*-protocol.c ;\
	H_OUT=build/$*-client-protocol.h ;\
	wayland-scanner client-header $$PROTO_DEF $$H_OUT ;\
	wayland-scanner private-code  $$PROTO_DEF $$C_OUT
	
	clang $(CFLAGS) build/$*-protocol.c -c -o $@


build/src/%.o: swayimg/src/%.c
	mkdir -p build/src/formats
	clang $(CFLAGS) $< -c -o $@

build/src/formats/%.o: swayimg/src/formats/%.c
	mkdir -p build/src/formats
	clang $(CFLAGS) $< -c -o $@

hackswayimg:  \
	build/content-type-v1-protocol.o \
	build/cursor-shape-v1-protocol.o \
	build/fractional-scale-v1-protocol.o \
	build/viewporter-protocol.o \
	build/xdg-decoration-unstable-v1-protocol.o \
	build/xdg-shell-protocol.o \
	build/tablet-protocol.o \
	build/src/action.o \
	build/src/application.o \
	build/src/config.o \
	build/src/event.o \
	build/src/exif.o \
	build/src/fetcher.o \
	build/src/font.o \
	build/src/formats/bmp.o \
	build/src/formats/dicom.o \
	build/src/formats/farbfeld.o \
	build/src/formats/gif.o \
	build/src/formats/jpeg.o \
	build/src/formats/png.o \
	build/src/formats/pnm.o \
	build/src/formats/qoi.o \
	build/src/formats/tga.o \
	build/src/formats/tiff.o \
	build/src/formats/webp.o \
	build/src/gallery.o \
	build/src/image.o \
	build/src/imagelist.o \
	build/src/info.o \
	build/src/keybind.o \
	build/src/loader.o \
	build/src/main.o \
	build/src/memdata.o \
	build/src/pixmap.o \
	build/src/pixmap_scale.o \
	build/src/sway.o \
	build/src/thumbnail.o \
	build/src/ui.o \
	build/src/viewer.o \
	build/src/wndbuf.o
	clang $(CFLAGS) $^ $(LDFLAGS) -o $@


.PHONY: xcompile-start xcompile-end xcompile-rebuild-sysrootdeps

xcompile-start:
	./rpiz-xcompile/mount_rpy_root.sh ~/src/xcomp-rpiz-env

xcompile-end:
	./rpiz-xcompile/umount_rpy_root.sh ~/src/xcomp-rpiz-env

install_sysroot_deps:
	./rpiz-xcompile/add_sysroot_pkg.sh ~/src/xcomp-rpiz-env http://raspbian.raspberrypi.com/raspbian/pool/main/w/wayland-protocols/wayland-protocols_1.41-1_all.deb
	./rpiz-xcompile/add_sysroot_pkg.sh ~/src/xcomp-rpiz-env http://raspbian.raspberrypi.com/raspbian/pool/main/g/giflib/libgif-dev_5.2.2-1_armhf.deb
	./rpiz-xcompile/add_sysroot_pkg.sh ~/src/xcomp-rpiz-env http://archive.raspberrypi.com/debian/pool/main/w/wayland/libwayland-dev_1.22.0-2.1~bpo12+rpt1_armhf.deb
	./rpiz-xcompile/add_sysroot_pkg.sh ~/src/xcomp-rpiz-env http://raspbian.raspberrypi.com/raspbian/pool/main/libx/libxkbcommon/libxkbcommon-dev_1.5.0-1_armhf.deb
	./rpiz-xcompile/add_sysroot_pkg.sh ~/src/xcomp-rpiz-env http://raspbian.raspberrypi.com/raspbian/pool/main/libe/libexif/libexif12_0.6.24-1+b2_armhf.deb
	./rpiz-xcompile/add_sysroot_pkg.sh ~/src/xcomp-rpiz-env http://raspbian.raspberrypi.com/raspbian/pool/main/libe/libexif/libexif-dev_0.6.24-1+b2_armhf.deb
	./rpiz-xcompile/add_sysroot_pkg.sh ~/src/xcomp-rpiz-env http://raspbian.raspberrypi.com/raspbian/pool/main/f/fontconfig/fontconfig_2.14.1-4_armhf.deb
	./rpiz-xcompile/add_sysroot_pkg.sh ~/src/xcomp-rpiz-env http://raspbian.raspberrypi.com/raspbian/pool/main/f/fontconfig/libfontconfig-dev_2.14.1-4_armhf.deb
	./rpiz-xcompile/add_sysroot_pkg.sh ~/src/xcomp-rpiz-env http://raspbian.raspberrypi.com/raspbian/pool/main/f/freetype/libfreetype-dev_2.12.1+dfsg-5+deb12u3_armhf.deb
	./rpiz-xcompile/add_sysroot_pkg.sh ~/src/xcomp-rpiz-env http://raspbian.raspberrypi.com/raspbian/pool/main/f/freetype/libfreetype6_2.12.1+dfsg-5+deb12u3_armhf.deb
	./rpiz-xcompile/add_sysroot_pkg.sh ~/src/xcomp-rpiz-env http://raspbian.raspberrypi.com/raspbian/pool/main/libj/libjpeg-turbo/libjpeg-dev_2.1.5-2_armhf.deb
	./rpiz-xcompile/add_sysroot_pkg.sh ~/src/xcomp-rpiz-env http://raspbian.raspberrypi.com/raspbian/pool/main/libj/libjpeg-turbo/libjpeg62-turbo-dev_2.1.5-2_armhf.deb
	./rpiz-xcompile/add_sysroot_pkg.sh ~/src/xcomp-rpiz-env http://raspbian.raspberrypi.com/raspbian/pool/main/libj/libjpeg-turbo/libjpeg62-turbo_2.1.5-2_armhf.deb
	./rpiz-xcompile/add_sysroot_pkg.sh ~/src/xcomp-rpiz-env http://raspbian.raspberrypi.com/raspbian/pool/main/libp/libpng1.6/libpng-dev_1.6.39-2_armhf.deb
	./rpiz-xcompile/add_sysroot_pkg.sh ~/src/xcomp-rpiz-env http://raspbian.raspberrypi.com/raspbian/pool/main/libp/libpng1.6/libpng-tools_1.6.39-2_armhf.deb
	./rpiz-xcompile/add_sysroot_pkg.sh ~/src/xcomp-rpiz-env http://raspbian.raspberrypi.com/raspbian/pool/main/libp/libpng1.6/libpng16-16_1.6.39-2_armhf.deb
	./rpiz-xcompile/add_sysroot_pkg.sh ~/src/xcomp-rpiz-env http://raspbian.raspberrypi.com/raspbian/pool/main/t/tiff/libtiff6_4.5.0-6+deb12u2_armhf.deb
	./rpiz-xcompile/add_sysroot_pkg.sh ~/src/xcomp-rpiz-env http://raspbian.raspberrypi.com/raspbian/pool/main/t/tiff/libtiff-dev_4.5.0-6+deb12u2_armhf.deb
	./rpiz-xcompile/add_sysroot_pkg.sh ~/src/xcomp-rpiz-env http://raspbian.raspberrypi.com/raspbian/pool/main/libw/libwebp/libwebp-dev_1.2.4-0.2+deb12u1_armhf.deb
	./rpiz-xcompile/add_sysroot_pkg.sh ~/src/xcomp-rpiz-env http://raspbian.raspberrypi.com/raspbian/pool/main/j/json-c/libjson-c-dev_0.16-2_armhf.deb
	./rpiz-xcompile/add_sysroot_pkg.sh ~/src/xcomp-rpiz-env http://raspbian.raspberrypi.com/raspbian/pool/main/b/brotli/libbrotli1_1.0.9-2+b3_armhf.deb
	./rpiz-xcompile/add_sysroot_pkg.sh ~/src/xcomp-rpiz-env http://raspbian.raspberrypi.com/raspbian/pool/main/b/brotli/libbrotli-dev_1.0.9-2+b3_armhf.deb
	./rpiz-xcompile/add_sysroot_pkg.sh ~/src/xcomp-rpiz-env http://raspbian.raspberrypi.com/raspbian/pool/main/x/xz-utils/liblzma-dev_5.4.1-0.2_armhf.deb
	./rpiz-xcompile/add_sysroot_pkg.sh ~/src/xcomp-rpiz-env http://raspbian.raspberrypi.com/raspbian/pool/main/libz/libzstd/libzstd-dev_1.5.4+dfsg2-5_armhf.deb
	./rpiz-xcompile/add_sysroot_pkg.sh ~/src/xcomp-rpiz-env http://raspbian.raspberrypi.com/raspbian/pool/main/libd/libdeflate/libdeflate-dev_1.14-1_armhf.deb
	./rpiz-xcompile/add_sysroot_pkg.sh ~/src/xcomp-rpiz-env http://raspbian.raspberrypi.com/raspbian/pool/main/l/lerc/liblerc-dev_4.0.0+ds-2_armhf.deb
	./rpiz-xcompile/add_sysroot_pkg.sh ~/src/xcomp-rpiz-env http://raspbian.raspberrypi.com/raspbian/pool/main/j/jbigkit/libjbig-dev_2.1-6.1_armhf.deb

