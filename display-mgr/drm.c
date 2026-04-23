#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <libdrm/drm_fourcc.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

#include "drm.h"

#define MAX_OUTS 4

struct DRM_output {
  uint32_t conn_id;
  uint32_t crtc_id;
  drmModeCrtc *saved_crtc;
};

struct DRM_state {
  int fd;
  uint32_t fb_handle;
  uint32_t fb_id;
  int dmabuf_fd;
  struct fb_info fb_info;
  size_t num_outputs;
  struct DRM_output outputs[MAX_OUTS];
};

void drm_free(struct DRM_state *s) {
  if (!s)
    return;

  for (size_t i = 0; i < s->num_outputs; i++) {
    if (s->outputs[i].saved_crtc)
      drmModeFreeCrtc(s->outputs[i].saved_crtc);
  }

  if (s->fb_id)
    drmModeRmFB(s->fd, s->fb_id);
  if (s->dmabuf_fd >= 0)
    close(s->dmabuf_fd);
  if (s->fb_handle) {
    struct drm_mode_destroy_dumb destroy = {.handle = s->fb_handle};
    drmIoctl(s->fd, DRM_IOCTL_MODE_DESTROY_DUMB, &destroy);
  }

  if (s->fd >= 0) {
    drmDropMaster(s->fd);
    close(s->fd);
  }

  free(s);
}

struct DRM_state *drm_init(void) {
  struct DRM_state *s = calloc(1, sizeof(struct DRM_state));
  if (!s)
    return NULL;

  s->fd = -1;
  s->dmabuf_fd = -1;

  s->fd = open("/dev/dri/card0", O_RDWR);
  if (s->fd < 0) {
    s->fd = open("/dev/dri/card1", O_RDWR);
    if (s->fd < 0) {
      perror("open /dev/dri/card*");
      drm_free(s);
      return NULL;
    }
  }

  if (drmSetMaster(s->fd) < 0) {
    perror("drmSetMaster");
    drm_free(s);
    return NULL;
  }

  drmModeRes *res = drmModeGetResources(s->fd);
  if (!res) {
    perror("drmModeGetResources");
    drm_free(s);
    return NULL;
  }

  s->num_outputs = 0;
  for (int i = 0; i < res->count_connectors; i++) {
    if (s->num_outputs >= MAX_OUTS) {
      fprintf(stderr, "More than max (%d) supported display outputs detected.\n", MAX_OUTS);
      break;
    }

    drmModeConnector *conn = drmModeGetConnector(s->fd, res->connectors[i]);
    if (!conn)
      continue;
    if (conn->connection != DRM_MODE_CONNECTED) {
      drmModeFreeConnector(conn);
      continue;
    }

    drmModeEncoder *enc = drmModeGetEncoder(s->fd, conn->encoder_id);
    if (!enc) {
      drmModeFreeConnector(conn);
      continue;
    }

    struct DRM_output *out = &s->outputs[s->num_outputs];
    out->conn_id = conn->connector_id;
    out->crtc_id = enc->crtc_id;
    out->saved_crtc = drmModeGetCrtc(s->fd, enc->crtc_id);
    s->num_outputs++;

    drmModeFreeEncoder(enc);
    drmModeFreeConnector(conn);
  }

  drmModeFreeResources(res);

  if (s->num_outputs == 0) {
    fprintf(stderr, "no connected outputs found\n");
    drm_free(s);
    return NULL;
  }

  return s;
}

void drm_display_off(struct DRM_state *s) {
  for (size_t i = 0; i < s->num_outputs; i++) {
    if (drmModeSetCrtc(s->fd, s->outputs[i].crtc_id, 0, 0, 0, NULL, 0, NULL) < 0)
      perror("Failed to turn display off: drmModeSetCrtc (disable)");
  }
}

void drm_display_on(struct DRM_state *s) {
  for (size_t i = 0; i < s->num_outputs; i++) {
    drmModeCrtc *c = s->outputs[i].saved_crtc;
    if (!c || !c->mode_valid)
      continue;
    if (drmModeSetCrtc(s->fd, s->outputs[i].crtc_id, s->fb_id, 0, 0, &s->outputs[i].conn_id, 1, &c->mode) < 0)
      perror("Failed to turn display on: drmModeSetCrtc (enable)");
  }
}

bool drm_is_display_on(const struct DRM_state *s) {
  for (size_t i = 0; i < s->num_outputs; i++) {
    drmModeCrtc *crtc = drmModeGetCrtc(s->fd, s->outputs[i].crtc_id);
    if (!crtc)
      continue;
    bool active = (crtc->buffer_id != 0);
    drmModeFreeCrtc(crtc);
    if (active)
      return true;
  }
  return false;
}

int drm_create_framebuffer(struct DRM_state *s) {
  drmModeCrtc *crtc = s->outputs[0].saved_crtc;
  if (!crtc || !crtc->mode_valid) {
    fprintf(stderr, "no valid mode on primary output\n");
    return -1;
  }

  uint32_t width = crtc->mode.hdisplay;
  uint32_t height = crtc->mode.vdisplay;

  struct drm_mode_create_dumb create = {
      .width = width,
      .height = height,
      .bpp = 32,
  };
  if (drmIoctl(s->fd, DRM_IOCTL_MODE_CREATE_DUMB, &create) < 0) {
    perror("DRM_IOCTL_MODE_CREATE_DUMB");
    return -1;
  }

  s->fb_handle = create.handle;

  if (drmModeAddFB(s->fd, width, height, 24, 32, create.pitch, s->fb_handle, &s->fb_id) < 0) {
    perror("drmModeAddFB");
    return -1;
  }

  int dmabuf_fd = -1;
  if (drmPrimeHandleToFD(s->fd, s->fb_handle, DRM_CLOEXEC | DRM_RDWR, &dmabuf_fd) < 0) {
    perror("drmPrimeHandleToFD");
    return -1;
  }
  s->dmabuf_fd = dmabuf_fd;

  s->fb_info.width = width;
  s->fb_info.height = height;
  s->fb_info.stride = create.pitch;
  s->fb_info.format = DRM_FORMAT_XRGB8888;
  s->fb_info.size = (uint32_t)create.size;

  fprintf(stderr, "framebuffer: %ux%u stride=%u size=%u\n", width, height, s->fb_info.stride, s->fb_info.size);

  // Set this framebuffer on all CRTCs
  for (size_t i = 0; i < s->num_outputs; i++) {
    drmModeCrtc *c = s->outputs[i].saved_crtc;
    if (!c || !c->mode_valid)
      continue;
    if (drmModeSetCrtc(s->fd, s->outputs[i].crtc_id, s->fb_id, 0, 0, &s->outputs[i].conn_id, 1, &c->mode) < 0)
      perror("drmModeSetCrtc (set fb)");
  }

  return 0;
}

const struct fb_info *drm_get_fb_info(const struct DRM_state *s) { return &s->fb_info; }

int drm_get_dmabuf_fd(const struct DRM_state *s) { return s->dmabuf_fd; }

size_t drm_get_num_outputs(const struct DRM_state *s) { return s->num_outputs; }
