# display-ctrl

CLI client for `display-mgr`. Calls the `io.homeboard.Display1` interface on the system D-Bus and prints the result.

```
display-ctrl on|off|status
```

Requires `display-mgr` to be running and the `io.homeboard.Display.conf` D-Bus policy to be installed (see `../display-mgr/README.md`).

For ad-hoc calls, `busctl` works too:

```
busctl --system call io.homeboard.Display /io/homeboard/Display io.homeboard.Display1 Status
```
