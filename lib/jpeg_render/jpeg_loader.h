#pragma once

#include <stdint.h>

struct jpeg_image {
  uint32_t width;
  uint32_t height;
  uint8_t *pixels; // RGB, 3 bytes per pixel
};

// Load a JPEG file. If target_w/target_h are non-zero, downscales during
// decoding to the smallest size still >= the target on both axes.
// Returns a malloc'd image, or NULL on failure.
struct jpeg_image *jpeg_load(const char *path, uint32_t target_w, uint32_t target_h);

// Same, but reads from an already-open fd (eg. a memfd). Takes ownership of
// fd: fd is closed before returning, whether or not decoding succeeds.
struct jpeg_image *jpeg_load_fd(int fd, uint32_t target_w, uint32_t target_h);

// Free a loaded image.
void jpeg_free(struct jpeg_image *img);
