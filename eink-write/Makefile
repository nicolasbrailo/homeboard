BIN_NAME = eink-write
CFLAGS += -I../lib/
LDFLAGS += -Wl,--gc-sections \
	-Wl,-Bstatic -lcairo -lfontconfig -lfreetype -lpng -lz -lexpat -lbz2 -lbrotlidec -lbrotlicommon \
	-Wl,-Bdynamic -lpixman-1 -lm -lpthread

SRCS = \
		../lib/rpigpio/rpigpio.c \
		../lib/eink/eink.c \
		../lib/eink/cairo_helpers.c \
		main.c

all: $(BIN_NAME)

include ../common.mk

install_sysroot_deps:
	../rpiz-xcompile/add_sysroot_pkg.sh ~/src/xcomp-rpiz-env http://archive.raspberrypi.com/debian/pool/main/c/cairo/libcairo2-dev_1.16.0-7+rpt1_armhf.deb
	../rpiz-xcompile/add_sysroot_pkg.sh ~/src/xcomp-rpiz-env http://raspbian.raspberrypi.com/raspbian/pool/main/p/pixman/libpixman-1-dev_0.46.4-1_armhf.deb
	../rpiz-xcompile/add_sysroot_pkg.sh ~/src/xcomp-rpiz-env http://raspbian.raspberrypi.com/raspbian/pool/main/f/freetype/libfreetype-dev_2.14.3+dfsg-1_armhf.deb
	../rpiz-xcompile/add_sysroot_pkg.sh ~/src/xcomp-rpiz-env http://raspbian.raspberrypi.com/raspbian/pool/main/f/fontconfig/libfontconfig-dev_2.17.1-5_armhf.deb
	../rpiz-xcompile/add_sysroot_pkg.sh ~/src/xcomp-rpiz-env http://raspbian.raspberrypi.com/raspbian/pool/main/libp/libpng1.6/libpng-dev_1.6.55-1_armhf.deb
	../rpiz-xcompile/add_sysroot_pkg.sh ~/src/xcomp-rpiz-env http://raspbian.raspberrypi.com/raspbian/pool/main/e/expat/libexpat1-dev_2.7.5-1_armhf.deb
	../rpiz-xcompile/add_sysroot_pkg.sh ~/src/xcomp-rpiz-env http://raspbian.raspberrypi.com/raspbian/pool/main/b/bzip2/libbz2-dev_1.0.8-6_armhf.deb
	../rpiz-xcompile/add_sysroot_pkg.sh ~/src/xcomp-rpiz-env http://raspbian.raspberrypi.com/raspbian/pool/main/b/brotli/libbrotli-dev_1.2.0-3_armhf.deb

enable-spi:
	ssh $(DEPLOY_TGT_HOST) sudo raspi-config nonint do_spi 0
	ssh $(DEPLOY_TGT_HOST) sudo dtparam spi=on

enable-user-gpio:
	ssh $(DEPLOY_TGT_HOST) sudo usermod -aG gpio,spi '$$USER'


