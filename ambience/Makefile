BIN_NAME = ambience
DBUS_POLICY = io.homeboard.Ambience.conf
SRCS = main.c config.c dbus.c dbus_helpers.c display.c slideshow.c eink_meta.c \
       ../lib/drm_mgr/drm_mgr.c \
       ../lib/jpeg_render/jpeg_loader.c \
       ../lib/jpeg_render/img_render.c \
       ../lib/rpigpio/rpigpio.c \
       ../lib/eink/eink.c \
       ../lib/eink/cairo_helpers.c
CFLAGS += -I../lib/
LDFLAGS += -Wl,-Bstatic -ljson-c  -ljpeg -lpthread \
           -Wl,-Bstatic -lcairo -lfontconfig -lfreetype -lpng -lz -lexpat -lbz2 -lbrotlidec -lbrotlicommon -lm \
           -Wl,-Bdynamic -lpixman-1 -lpthread -lsystemd

all: $(BIN_NAME)

include ../common.mk

SSH = ssh $(DEPLOY_TGT_HOST)
tgt-next:
	$(SSH) 'busctl --system call io.homeboard.Ambience /io/homeboard/Ambience io.homeboard.Ambience1 Next'
tgt-prev:
	$(SSH) 'busctl --system call io.homeboard.Ambience /io/homeboard/Ambience io.homeboard.Ambience1 Prev'
tgt-force-on:
	$(SSH) 'busctl --system call io.homeboard.Ambience /io/homeboard/Ambience io.homeboard.Ambience1 ForceSlideshowOn'
tgt-force-off:
	$(SSH) 'busctl --system call io.homeboard.Ambience /io/homeboard/Ambience io.homeboard.Ambience1 ForceSlideshowOff'
tgt-set-transition-time:
	$(SSH) 'busctl --system call io.homeboard.Ambience /io/homeboard/Ambience io.homeboard.Ambience1 SetTransitionTimeSecs u 5'
announce:
	$(SSH) 'busctl --system call io.homeboard.Ambience /io/homeboard/Ambience io.homeboard.Ambience1 Announce us 10 "HOLA MUNDO"'

install_sysroot_deps:
	@# Jpeg render deps
	../rpiz-xcompile/add_sysroot_pkg.sh ~/src/xcomp-rpiz-env http://raspbian.raspberrypi.com/raspbian/pool/main/libj/libjpeg-turbo/libjpeg62-turbo-dev_2.1.5-4_armhf.deb
	../rpiz-xcompile/add_sysroot_pkg.sh ~/src/xcomp-rpiz-env http://raspbian.raspberrypi.com/raspbian/pool/main/libj/libjpeg-turbo/libjpeg62-turbo_2.1.5-4_armhf.deb
	@# Eink deps
	../rpiz-xcompile/add_sysroot_pkg.sh ~/src/xcomp-rpiz-env http://archive.raspberrypi.com/debian/pool/main/c/cairo/libcairo2-dev_1.16.0-7+rpt1_armhf.deb
	../rpiz-xcompile/add_sysroot_pkg.sh ~/src/xcomp-rpiz-env http://raspbian.raspberrypi.com/raspbian/pool/main/p/pixman/libpixman-1-dev_0.46.4-1_armhf.deb
	../rpiz-xcompile/add_sysroot_pkg.sh ~/src/xcomp-rpiz-env http://raspbian.raspberrypi.com/raspbian/pool/main/f/freetype/libfreetype-dev_2.14.3+dfsg-1_armhf.deb
	../rpiz-xcompile/add_sysroot_pkg.sh ~/src/xcomp-rpiz-env http://raspbian.raspberrypi.com/raspbian/pool/main/f/fontconfig/libfontconfig-dev_2.17.1-5_armhf.deb
	../rpiz-xcompile/add_sysroot_pkg.sh ~/src/xcomp-rpiz-env http://raspbian.raspberrypi.com/raspbian/pool/main/libp/libpng1.6/libpng-dev_1.6.55-1_armhf.deb
	../rpiz-xcompile/add_sysroot_pkg.sh ~/src/xcomp-rpiz-env http://raspbian.raspberrypi.com/raspbian/pool/main/e/expat/libexpat1-dev_2.7.5-1_armhf.deb
	../rpiz-xcompile/add_sysroot_pkg.sh ~/src/xcomp-rpiz-env http://raspbian.raspberrypi.com/raspbian/pool/main/b/bzip2/libbz2-dev_1.0.8-6_armhf.deb
	../rpiz-xcompile/add_sysroot_pkg.sh ~/src/xcomp-rpiz-env http://raspbian.raspberrypi.com/raspbian/pool/main/b/brotli/libbrotli-dev_1.2.0-3_armhf.deb
