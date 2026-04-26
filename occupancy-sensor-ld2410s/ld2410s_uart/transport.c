#include "transport.h"

#include <fcntl.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#define TRANSPORT_BUF_SIZE 256
#define TRANSPORT_QUEUE_SIZE 8

struct transport_cmd {
  uint16_t cmd_word;
  uint8_t data[TRANSPORT_MAX_DATA];
  size_t data_len;
};

struct transport {
  int fd;
  pthread_t reader_thread;
  atomic_bool thread_running;

  /* Lockless SPSC command queue.
   * Producer: caller thread (via transport_enqueue).
   * Consumer: reader thread (dequeue + send + ack tracking). */
  struct transport_cmd queue[TRANSPORT_QUEUE_SIZE];
  atomic_size_t queue_head;
  atomic_size_t queue_tail;

  transport_cmd_response_cb on_cmd_response;
  void *cmd_response_ctx;
  transport_report_cb on_report;
  void *report_ctx;
  transport_calibration_cb on_calibration;
  void *calibration_ctx;

  bool debug;
};

/* --- Protocol frame constants --- */

static const uint8_t CMD_HEADER[] = {0xFD, 0xFC, 0xFB, 0xFA};
static const uint8_t CMD_FOOTER[] = {0x04, 0x03, 0x02, 0x01};
static const uint8_t CAL_HEADER[] = {0xF4, 0xF3, 0xF2, 0xF1};
static const uint8_t CAL_FOOTER[] = {0xF8, 0xF7, 0xF6, 0xF5};

#define RESP_OFFSET 0x0100
#define CMD_TIMEOUT_SEC 2.0

/* --- Helpers --- */

static uint16_t read_u16_le(const uint8_t *p) {
  return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static void write_u16_le(uint8_t *p, uint16_t v) {
  p[0] = v & 0xFF;
  p[1] = (v >> 8) & 0xFF;
}

static double monotonic_now(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return ts.tv_sec + ts.tv_nsec * 1e-9;
}

/* Scan backward through buf[0..buf_len) for a 4-byte pattern. */
static const uint8_t *memrfind4(const uint8_t *buf, size_t buf_len,
                                const uint8_t pattern[4]) {
  if (buf_len < 4)
    return NULL;
  for (size_t i = buf_len - 4;; i--) {
    if (memcmp(buf + i, pattern, 4) == 0)
      return buf + i;
    if (i == 0)
      break;
  }
  return NULL;
}

static void debug_hex(const struct transport *f, const char *prefix,
                      const uint8_t *data, size_t len) {
  if (!f->debug || !data || (len == 0))
    return;
  printf("%s", prefix);
  for (size_t i = 0; i < len; i++)
    printf(" %02x", data[i]);
  printf("\n");
}

/* --- SPSC queue (lockless, single-producer single-consumer) --- */

static bool spsc_enqueue(struct transport *f, const struct transport_cmd *cmd) {
  size_t head = atomic_load_explicit(&f->queue_head, memory_order_relaxed);
  size_t tail = atomic_load_explicit(&f->queue_tail, memory_order_acquire);
  if (head - tail >= TRANSPORT_QUEUE_SIZE)
    return false;
  f->queue[head % TRANSPORT_QUEUE_SIZE] = *cmd;
  atomic_store_explicit(&f->queue_head, head + 1, memory_order_release);
  return true;
}

static bool spsc_dequeue(struct transport *f, struct transport_cmd *out) {
  size_t tail = atomic_load_explicit(&f->queue_tail, memory_order_relaxed);
  size_t head = atomic_load_explicit(&f->queue_head, memory_order_acquire);
  if (tail == head)
    return false;
  *out = f->queue[tail % TRANSPORT_QUEUE_SIZE];
  atomic_store_explicit(&f->queue_tail, tail + 1, memory_order_release);
  return true;
}

/* --- Serial port --- */

static int open_serial(const char *dev_path) {
  int fd = open(dev_path, O_RDWR | O_NOCTTY);
  if (fd < 0) {
    perror("open serial");
    return -1;
  }

  struct termios tty;
  if (tcgetattr(fd, &tty) < 0) {
    perror("tcgetattr");
    close(fd);
    return -1;
  }

  cfmakeraw(&tty);
  cfsetispeed(&tty, B115200);
  cfsetospeed(&tty, B115200);
  tty.c_cc[VMIN] = 0;
  tty.c_cc[VTIME] = 1; /* 100ms read timeout */

  if (tcsetattr(fd, TCSANOW, &tty) < 0) {
    perror("tcsetattr");
    close(fd);
    return -1;
  }

  /* Drop any stale bytes left over from a prior run. */
  tcflush(fd, TCIOFLUSH);

  return fd;
}

/* --- Frame building --- */

static int send_cmd(struct transport *f, uint16_t cmd_word, const void *data,
                    size_t data_len) {
  /* [header 4] [payload_len 2] [cmd_word 2] [data ...] [footer 4] */
  uint8_t frame[4 + 2 + 2 + TRANSPORT_MAX_DATA + 4];
  size_t payload_len = 2 + data_len;

  if (payload_len > TRANSPORT_MAX_DATA) {
    fprintf(stderr, "Err: payload too large (%zu, max is %zu\n", payload_len,
            TRANSPORT_MAX_DATA);
    return -1;
  }

  size_t pos = 0;
  memcpy(frame + pos, CMD_HEADER, 4);
  pos += 4;
  write_u16_le(frame + pos, (uint16_t)payload_len);
  pos += 2;
  write_u16_le(frame + pos, cmd_word);
  pos += 2;
  if (data_len > 0) {
    memcpy(frame + pos, data, data_len);
    pos += data_len;
  }
  memcpy(frame + pos, CMD_FOOTER, 4);
  pos += 4;

  debug_hex(f, "[TX] cmd", frame, pos);
  if (write(f->fd, frame, pos) < 0) {
    perror("write serial");
    return -1;
  }
  return 0;
}

/* --- Frame parsing --- */

static void handle_report_frame(struct transport *f, const uint8_t *frame,
                                size_t len) {
  if (f->on_report)
    f->on_report(f->report_ctx, frame, len);
}

static void handle_calibration_frame(struct transport *f, const uint8_t *frame,
                                     size_t len) {
  if (f->on_calibration)
    f->on_calibration(f->calibration_ctx, frame, len);
}

static void handle_cmd_response(struct transport *f, const uint8_t *frame,
                                size_t len) {
  /* frame: [header 4] [payload_len 2] [resp_cmd 2] [status 2] [data...] [footer
   * 4] */
  if (len < 4 + 2 + 2 + 2 + 4) {
    fprintf(stderr, "Err: cmd response too short (%zu)\n", len);
    return;
  }

  size_t payload_len = read_u16_le(frame + 4);
  const uint8_t *payload = frame + 6;
  size_t actual_payload = len - 4 - 2 - 4;

  if (actual_payload != payload_len)
    fprintf(stderr, "Err: bad cmd response len=%zu expected=%zu\n",
            actual_payload, payload_len);

  if (actual_payload < 4) {
    fprintf(stderr, "Err: cmd response short payload\n");
    return;
  }

  uint16_t resp_cmd = read_u16_le(payload);
  uint16_t status = read_u16_le(payload + 2);
  const uint8_t *data = payload + 4;
  size_t data_len = actual_payload - 4;

  if (f->on_cmd_response) {
    if (f->debug)
      printf("LD2410S UART transport: invoke on_cmd_response\n");
    f->on_cmd_response(f->cmd_response_ctx, resp_cmd, status, data, data_len);
  }
}

enum parse_result {
  PARSE_NONE,
  PARSE_REPORT,
  PARSE_CMD_RESPONSE,
  PARSE_CALIBRATION,
};

/* Try to parse a complete frame from buf. */
static enum parse_result parse_buffer(struct transport *f, uint8_t *buf,
                                      size_t n) {
  /* Short report frame: 6E [state] [dist_lo] [dist_hi] 62 */
  if (n >= 5 && buf[n - 5] == 0x6E && buf[n - 1] == 0x62) {
    if (n > 5)
      debug_hex(f, "[RX] ERR, desync:", buf, n - 5);
    debug_hex(f, "[RX] report", buf + n - 5, 5);
    handle_report_frame(f, buf + n - 5, 5);
    return PARSE_REPORT;
  }

  /* Command response frame */
  if (n >= 4 && memcmp(buf + n - 4, CMD_FOOTER, 4) == 0) {
    const uint8_t *hdr = memrfind4(buf, n - 4, CMD_HEADER);
    if (!hdr) {
      debug_hex(f, "[RX] desync:", buf, n);
    } else {
      size_t frame_len = (size_t)(buf + n - hdr);
      size_t prefix = (size_t)(hdr - buf);
      debug_hex(f, "[RX] ERR, desync:", buf, prefix);
      debug_hex(f, "[RX] cmd_response", hdr, frame_len);
      handle_cmd_response(f, hdr, frame_len);
    }
    return PARSE_CMD_RESPONSE;
  }

  /* Calibration report frame */
  if (n >= 4 && memcmp(buf + n - 4, CAL_FOOTER, 4) == 0) {
    const uint8_t *hdr = memrfind4(buf, n - 4, CAL_HEADER);
    if (!hdr) {
      debug_hex(f, "[RX] desync:", buf, n);
    } else {
      size_t frame_len = (size_t)(buf + n - hdr);
      debug_hex(f, "[RX] calibration", hdr, frame_len);
      handle_calibration_frame(f, hdr, frame_len);
    }
    return PARSE_CALIBRATION;
  }

  return PARSE_NONE;
}

/* --- Reader thread --- */

static void *reader_thread_fn(void *arg) {
  struct transport *f = arg;
  uint8_t buf[TRANSPORT_BUF_SIZE];
  size_t buf_len = 0;

  bool waiting_for_response = false;
  double response_timeout = 0;

  while (f->thread_running) {
    /* Try to send next queued command */
    if (waiting_for_response) {
      if (monotonic_now() >= response_timeout) {
        fprintf(stderr, "Timeout waiting for command response\n");
        waiting_for_response = false;
      }
    }

    if (!waiting_for_response) {
      struct transport_cmd cmd;
      if (spsc_dequeue(f, &cmd)) {
        if (f->debug)
          printf("LD2410S UART transport: dequed command 0x%04X\n",
                 cmd.cmd_word);
        send_cmd(f, cmd.cmd_word, cmd.data, cmd.data_len);
        waiting_for_response = true;
        response_timeout = monotonic_now() + CMD_TIMEOUT_SEC;
      }
    }

    /* Read a byte from serial */
    uint8_t byte;
    ssize_t n = read(f->fd, &byte, 1);
    if (n <= 0)
      continue;

    buf[buf_len++] = byte;

    enum parse_result r = parse_buffer(f, buf, buf_len);
    /* A command response clears the ack wait; reports and
     * calibration frames can arrive interleaved with the wait, so we ignore
     * them. In theory, this may ACK a different command that the user expects,
     * but in practice the device will only work on a single command at a time,
     * if we send multiple commands without waiting for the first to ACK, the
     * client is broken */
    if (r == PARSE_CMD_RESPONSE) {
      waiting_for_response = false;
    }

    if (r != PARSE_NONE) {
      if (f->debug)
        printf("LD2410S UART transport: discovered response %d\n", r);
      buf_len = 0;
    }

    if (buf_len >= TRANSPORT_BUF_SIZE) {
      fprintf(stderr, "Err: buffer overflow, clearing\n");
      buf_len = 0;
    }
  }
  return NULL;
}

/* --- Public API --- */

struct transport *transport_init(
    const char *dev_path, bool debug, transport_cmd_response_cb on_cmd_response,
    void *cmd_response_ctx, transport_report_cb on_report, void *report_ctx,
    transport_calibration_cb on_calibration, void *calibration_ctx) {
  struct transport *f = calloc(1, sizeof(*f));
  if (!f)
    return NULL;

  f->fd = open_serial(dev_path);
  if (f->fd < 0) {
    free(f);
    return NULL;
  }
  if (debug)
    printf("Connected to UART port\n");

  atomic_init(&f->queue_head, 0);
  atomic_init(&f->queue_tail, 0);

  f->on_cmd_response = on_cmd_response;
  f->cmd_response_ctx = cmd_response_ctx;
  f->on_report = on_report;
  f->report_ctx = report_ctx;
  f->on_calibration = on_calibration;
  f->calibration_ctx = calibration_ctx;
  f->debug = debug;
  return f;
}

void transport_free(struct transport *f) {
  if (!f)
    return;

  if (f->thread_running) {
    f->thread_running = false;
    pthread_join(f->reader_thread, NULL);
  }

  if (f->fd >= 0)
    close(f->fd);

  free(f);
}

int transport_start(struct transport *f) {
  f->thread_running = true;
  if (pthread_create(&f->reader_thread, NULL, reader_thread_fn, f) != 0) {
    perror("pthread_create");
    f->thread_running = false;
    return -1;
  }
  return 0;
}

bool transport_enqueue(struct transport *f, uint16_t cmd_word, const void *data,
                       size_t data_len) {
  if (data_len > TRANSPORT_MAX_DATA) {
    fprintf(stderr, "Err: enqueue data too large (%zu)\n", data_len);
    return false;
  }

  struct transport_cmd cmd;
  cmd.cmd_word = cmd_word;
  cmd.data_len = data_len;
  if (data_len > 0)
    memcpy(cmd.data, data, data_len);

  return spsc_enqueue(f, &cmd);
}
