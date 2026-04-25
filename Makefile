SUBDIRS := $(filter-out rpiz-xcompile stockimgs lib bin, $(patsubst %/,%,$(wildcard */)))
SERVICES := ambience dbus-mqtt-bridge display-mgr occupancy-sensor-ld2410s photo-provider presence-service

# Keep these in sync with common.mk
DEPLOY_TGT_HOST=batman@10.0.0.78
DEPLOY_TGT_DIR=/home/batman/homeboard

.PHONY: all clean format $(SUBDIRS)

all:
	@for dir in $(SUBDIRS); do $(MAKE) -C $$dir ; done

clean:
	@for dir in $(SUBDIRS); do $(MAKE) -C $$dir clean; done

deploy-all:
	ssh "$(DEPLOY_TGT_HOST)" mkdir -p $(DEPLOY_TGT_DIR)/bin/
	ssh "$(DEPLOY_TGT_HOST)" mkdir -p $(DEPLOY_TGT_DIR)/etc/
	@for dir in $(SUBDIRS); do \
		$(MAKE) -C $$dir deploy-bin; \
		$(MAKE) -C $$dir deploy-config; \
		$(MAKE) -C $$dir deploy-dbus-policy; \
	done
	$(MAKE) -C stockimgs deploy-imgs

deploy-scripts:
	scp scripts/* $(DEPLOY_TGT_HOST):$(DEPLOY_TGT_DIR)/bin/

install-systemd:
	@for dir in $(SERVICES); do \
		$(MAKE) -C $$dir install-systemd; \
	done

format:
	find lib -name '*.c' -o -name '*.h' | xargs clang-format -i
	@for dir in $(SUBDIRS); do $(MAKE) -C $$dir format; done

patch-target-config:
	@echo "Will patch a bunch of settings and permissions that are needed for the project to run. Check each specific project for details on what they do"
	ssh "$(DEPLOY_TGT_HOST)" sudo raspi-config nonint do_spi 0
	ssh "$(DEPLOY_TGT_HOST)" sudo dtparam spi=on
	ssh "$(DEPLOY_TGT_HOST)" sudo usermod -aG gpio,spi '$$USER'
	ssh "$(DEPLOY_TGT_HOST)" sudo usermod -aG video '$$USER'
	ssh "$(DEPLOY_TGT_HOST)" sudo usermod -aG systemd-journal '$$USER'
	ssh "$(DEPLOY_TGT_HOST)" sudo setcap cap_sys_admin+ep display-mgr

