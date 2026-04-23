# show-img

Displays a JPEG image on the framebuffer via `display-mgr`. Scales to fit (preserving aspect ratio), centers on screen with black letterboxing.

```
show-img [options] <image.jpg>
  -r <0|90|180|270>      rotation (default: 0)
  -i <nearest|bilinear>  interpolation (default: bilinear)
```

Runs until interrupted (SIGTERM/SIGINT). Large images are downscaled during JPEG decoding for efficiency.

## Dependencies

Requires `libjpeg-turbo` in the sysroot:

```
sudo make install_sysroot_deps
```

## Source files

| File | Purpose |
|------|---------|
| `main.c` | Entry point, argument parsing |
| `jpeg_loader.c` | JPEG decoding via libjpeg, with optional decode-time downscaling |
| `img_render.c` | Scaling, rotation, and framebuffer blit (nearest-neighbor or bilinear) |

Uses `lib/drm_mgr` to communicate with `display-mgr`.
