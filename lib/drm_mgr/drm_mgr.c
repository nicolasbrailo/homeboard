#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <systemd/sd-bus.h>
#include <unistd.h>

#include "drm_mgr.h"

#define DBUS_SERVICE "io.homeboard.Display"
#define DBUS_PATH "/io/homeboard/Display"
#define DBUS_INTERFACE "io.homeboard.Display1"

struct DRM_Mgr {
  sd_bus *bus;
  void *fb;
  int dmabuf_fd;
  uint32_t fb_size;
  bool holds_drm;
};

struct DRM_Mgr *drm_mgr_init() {
  struct DRM_Mgr *self = calloc(1, sizeof(*self));
  if (!self)
    return NULL;
  self->dmabuf_fd = -1;
  self->holds_drm = false;

  int r = sd_bus_open_system(&self->bus);
  if (r < 0) {
    fprintf(stderr, "sd_bus_open_system: %s\n", strerror(-r));
    free(self);
    return NULL;
  }
  return self;
}

struct DRM_Mgr *drm_mgr_init_acquire_fb(uint32_t** fb, struct fb_info *info) {
  struct DRM_Mgr *drm_mgr = drm_mgr_init();
  if (!drm_mgr) {
    fprintf(stderr, "drm_mgr_init failed\n");
    *fb = NULL;
    memset(info, 0, sizeof(*info));
    return NULL;
  }

  *fb = drm_mgr_acquire_fb(drm_mgr, info);
  if (!*fb) {
    fprintf(stderr, "drm_mgr_acquire_fb failed\n");
    drm_mgr_free(drm_mgr);
    memset(info, 0, sizeof(*info));
    return NULL;
  }
  return drm_mgr;
}

void drm_mgr_free(struct DRM_Mgr *self) {
  if (!self)
    return;

  if (self->holds_drm)
    drm_mgr_release_fb(self);

  if (self->bus)
    sd_bus_flush_close_unref(self->bus);
  free(self);
}

uint32_t *drm_mgr_acquire_fb(struct DRM_Mgr *self, struct fb_info *info) {
  if (!self)
    return NULL;

  sd_bus_error err = SD_BUS_ERROR_NULL;
  sd_bus_message *reply = NULL;

  int r = sd_bus_call_method(self->bus, DBUS_SERVICE, DBUS_PATH, DBUS_INTERFACE,
                             "AcquireLease", &err, &reply, "");
  if (r < 0) {
    fprintf(stderr, "AcquireLease: %s\n",
            err.message ? err.message : strerror(-r));
    sd_bus_error_free(&err);
    return NULL;
  }

  int fd = -1;
  r = sd_bus_message_read(reply, "huuuuu", &fd, &info->width, &info->height,
                          &info->stride, &info->format, &info->size);
  if (r < 0) {
    fprintf(stderr, "AcquireLease: bad reply: %s\n", strerror(-r));
    sd_bus_message_unref(reply);
    sd_bus_error_free(&err);
    return NULL;
  }

  // sd-bus owns the fd in the message; dup so it survives message_unref.
  int owned_fd = fcntl(fd, F_DUPFD_CLOEXEC, 0);
  sd_bus_message_unref(reply);
  sd_bus_error_free(&err);
  if (owned_fd < 0) {
    perror("fcntl(F_DUPFD_CLOEXEC)");
    return NULL;
  }

  void *fb =
      mmap(NULL, info->size, PROT_READ | PROT_WRITE, MAP_SHARED, owned_fd, 0);
  if (fb == MAP_FAILED) {
    perror("mmap");
    close(owned_fd);
    return NULL;
  }

  self->fb = fb;
  self->dmabuf_fd = owned_fd;
  self->fb_size = info->size;
  self->holds_drm = true;
  return fb;
}

void drm_mgr_release_fb(struct DRM_Mgr *self) {
  if (!self->holds_drm) {
    fprintf(stderr, "Error: attempting to release DRM, but not holding it\n");
    return;
  }

  if (self->fb) {
    munmap(self->fb, self->fb_size);
    self->fb = NULL;
  }
  if (self->dmabuf_fd >= 0) {
    close(self->dmabuf_fd);
    self->dmabuf_fd = -1;
  }

  sd_bus_error err = SD_BUS_ERROR_NULL;
  int r = sd_bus_call_method(self->bus, DBUS_SERVICE, DBUS_PATH, DBUS_INTERFACE,
                             "ReleaseLease", &err, NULL, "");
  if (r < 0)
    fprintf(stderr, "ReleaseLease: %s\n",
            err.message ? err.message : strerror(-r));
  sd_bus_error_free(&err);
  self->holds_drm = false;
}
