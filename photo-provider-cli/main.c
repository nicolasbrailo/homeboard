#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <systemd/sd-bus.h>
#include <unistd.h>

#define DBUS_SERVICE "io.homeboard.PhotoProvider"
#define DBUS_PATH "/io/homeboard/PhotoProvider"
#define DBUS_INTERFACE "io.homeboard.PhotoProvider1"

static void usage(const char *argv0) {
  fprintf(stderr,
          "Usage:\n"
          "  %s <out.jpg>           fetch next photo, save to <out.jpg>, print metadata\n"
          "  %s -prev <out.jpg>     fetch previous photo from history, save to <out.jpg>\n"
          "  %s -qr <0|1>           set embed-QR flag\n"
          "  %s -size <WxH>         set target size (eg. 1024x768)\n",
          argv0, argv0, argv0, argv0);
}

static int call_simple(sd_bus *bus, const char *method, const char *sig, ...) {
  sd_bus_error err = SD_BUS_ERROR_NULL;
  sd_bus_message *reply = NULL;
  va_list ap;
  va_start(ap, sig);
  sd_bus_message *call = NULL;
  int r = sd_bus_message_new_method_call(bus, &call, DBUS_SERVICE, DBUS_PATH, DBUS_INTERFACE, method);
  if (r < 0)
    goto out;
  r = sd_bus_message_appendv(call, sig, ap);
  if (r < 0)
    goto out;
  r = sd_bus_call(bus, call, 0, &err, &reply);
out:
  va_end(ap);
  if (r < 0)
    fprintf(stderr, "%s failed: %s\n", method, err.message ? err.message : strerror(-r));
  sd_bus_error_free(&err);
  sd_bus_message_unref(call);
  sd_bus_message_unref(reply);
  return r < 0 ? -1 : 0;
}

static int cmd_get_photo(sd_bus *bus, const char *method, const char *out_path) {
  sd_bus_error err = SD_BUS_ERROR_NULL;
  sd_bus_message *reply = NULL;
  int r = sd_bus_call_method(bus, DBUS_SERVICE, DBUS_PATH, DBUS_INTERFACE, method, &err, &reply, "");
  if (r < 0) {
    fprintf(stderr, "%s failed: %s\n", method, err.message ? err.message : strerror(-r));
    sd_bus_error_free(&err);
    return -1;
  }

  int fd = -1;
  const char *meta = NULL;
  r = sd_bus_message_read(reply, "hs", &fd, &meta);
  if (r < 0) {
    fprintf(stderr, "bad reply: %s\n", strerror(-r));
    sd_bus_message_unref(reply);
    return -1;
  }

  printf("%s\n", meta);

  int out = open(out_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (out < 0) {
    perror(out_path);
    sd_bus_message_unref(reply);
    return -1;
  }

  off_t pos = 0;
  if (lseek(fd, 0, SEEK_SET) < 0) {
    perror("lseek");
    close(out);
    sd_bus_message_unref(reply);
    return -1;
  }
  for (;;) {
    char buf[64 * 1024];
    ssize_t n = read(fd, buf, sizeof(buf));
    if (n == 0)
      break;
    if (n < 0) {
      if (errno == EINTR)
        continue;
      perror("read");
      close(out);
      sd_bus_message_unref(reply);
      return -1;
    }
    ssize_t done = 0;
    while (done < n) {
      ssize_t w = write(out, buf + done, (size_t)(n - done));
      if (w < 0) {
        if (errno == EINTR)
          continue;
        perror("write");
        close(out);
        sd_bus_message_unref(reply);
        return -1;
      }
      done += w;
      pos += w;
    }
  }

  close(out);
  sd_bus_message_unref(reply);
  fprintf(stderr, "wrote %lld bytes to %s\n", (long long)pos, out_path);
  return 0;
}

int main(int argc, char *argv[]) {
  if (argc < 2) {
    usage(argv[0]);
    return 1;
  }

  sd_bus *bus = NULL;
  int r = sd_bus_open_system(&bus);
  if (r < 0) {
    fprintf(stderr, "sd_bus_open_system: %s\n", strerror(-r));
    return 1;
  }

  int rc = 1;
  if (strcmp(argv[1], "-qr") == 0) {
    if (argc != 3) {
      usage(argv[0]);
      goto out;
    }
    int v = atoi(argv[2]);
    rc = call_simple(bus, "SetEmbedQr", "b", v) < 0 ? 1 : 0;
  } else if (strcmp(argv[1], "-size") == 0) {
    if (argc != 3) {
      usage(argv[0]);
      goto out;
    }
    unsigned w = 0, h = 0;
    if (sscanf(argv[2], "%ux%u", &w, &h) != 2) {
      fprintf(stderr, "bad size '%s' (expected WxH)\n", argv[2]);
      goto out;
    }
    rc = call_simple(bus, "SetTargetSize", "uu", (uint32_t)w, (uint32_t)h) < 0 ? 1 : 0;
  } else if (strcmp(argv[1], "-prev") == 0) {
    if (argc != 3) {
      usage(argv[0]);
      goto out;
    }
    rc = cmd_get_photo(bus, "GetPrevPhoto", argv[2]) < 0 ? 1 : 0;
  } else if (argv[1][0] == '-') {
    usage(argv[0]);
  } else {
    rc = cmd_get_photo(bus, "GetPhoto", argv[1]) < 0 ? 1 : 0;
  }

out:
  sd_bus_flush_close_unref(bus);
  return rc;
}
