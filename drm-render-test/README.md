# drm-render-test

Test client for `display-mgr`. Acquires the framebuffer lease and cycles solid colors at 1 FPS until interrupted (SIGTERM/SIGINT).

Uses `lib/drm_mgr` to communicate with the daemon.
