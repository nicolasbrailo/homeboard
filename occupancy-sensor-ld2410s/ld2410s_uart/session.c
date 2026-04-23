#include "session.h"
#include "transport.h"

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define CMD_ENABLE_CONFIG 0x00FF
#define CMD_END_CONFIG 0x00FE
#define RESP_OFFSET 0x0100
#define CMD_TIMEOUT_SEC 2

struct session {
  struct transport *transport;
  bool debug;

  /* Serializes callers (one transaction at a time) */
  pthread_mutex_t cmd_mutex;

  /* Reader-thread → caller signaling. Reader deposits every response here;
   * waiter loops and discards ones that don't match its expected resp_cmd. */
  pthread_mutex_t resp_mutex;
  pthread_cond_t resp_cond;
  uint16_t resp_cmd;
  uint8_t resp_data[TRANSPORT_MAX_DATA];
  size_t resp_len;
  uint16_t resp_status;
  bool resp_pending; /* a response is sitting in the slot, not yet consumed */
};

/* --- Transport-layer callbacks (reader thread) --- */

static void on_cmd_response(void *ctx, uint16_t resp_cmd, uint16_t status, const uint8_t *data, size_t data_len) {
  struct session *s = ctx;

  if (status != 0)
    fprintf(stderr, "[CMD] error: resp=0x%04x status=0x%04x\n", resp_cmd, status);

  if (s->debug)
    printf("LD2410S UART session: received response 0x%04X\n", resp_cmd);

  pthread_mutex_lock(&s->resp_mutex);
  s->resp_cmd = resp_cmd;
  s->resp_status = status;
  size_t copy = data_len < sizeof(s->resp_data) ? data_len : sizeof(s->resp_data);
  memcpy(s->resp_data, data, copy);
  s->resp_len = copy;
  s->resp_pending = true;
  pthread_cond_signal(&s->resp_cond);
  pthread_mutex_unlock(&s->resp_mutex);
}

/* --- Core send/wait --- */

/* Caller must hold cmd_mutex: the response slot is single-slot, so only one
 * transaction can be outstanding from enqueue through response.
 * Non-matching responses that arrive during the wait are discarded, mirroring
 * the Python driver's tolerance for stray/late/out-of-order frames. */
static int send_and_wait(struct session *s, uint16_t cmd_word, const void *data, size_t data_len, uint8_t *out_buf,
                         size_t out_cap, size_t *out_len) {
  uint16_t expected = cmd_word + RESP_OFFSET;

  pthread_mutex_lock(&s->resp_mutex);
  s->resp_pending = false;
  pthread_mutex_unlock(&s->resp_mutex);

  if (s->debug)
    printf("LD2410S UART Session: queing command 0x%04X\n", cmd_word);
  if (!transport_enqueue(s->transport, cmd_word, data, data_len)) {
    fprintf(stderr, "Err: command queue full\n");
    return -1;
  }

  struct timespec deadline;
  clock_gettime(CLOCK_REALTIME, &deadline);
  deadline.tv_sec += CMD_TIMEOUT_SEC;

  pthread_mutex_lock(&s->resp_mutex);
  for (;;) {
    if (s->debug)
      printf("LD2410S UART Session: waiting for response to 0x%04X...\n", cmd_word);
    while (!s->resp_pending) {
      if (pthread_cond_timedwait(&s->resp_cond, &s->resp_mutex, &deadline) == ETIMEDOUT) {
        pthread_mutex_unlock(&s->resp_mutex);
        fprintf(stderr, "Err: timeout waiting for response to 0x%04x\n", cmd_word);
        return -1;
      }
    }
    s->resp_pending = false;
    if (s->resp_cmd == expected) {
      if (s->debug)
        printf("LD2410S UART Session: received response to 0x%04X...\n", cmd_word);
    } else {
      fprintf(stderr, "LD2410S UART Session: expected response=0x%04x, received=0x%04X...\n", expected, s->resp_cmd);
    }
    break;
  }
  if (out_buf && out_cap) {
    size_t copy = s->resp_len < out_cap ? s->resp_len : out_cap;
    memcpy(out_buf, s->resp_data, copy);
  }
  if (out_len)
    *out_len = s->resp_len;
  int ret = (s->resp_status == 0) ? 0 : -1;
  pthread_mutex_unlock(&s->resp_mutex);
  return ret;
}

/* --- Public API --- */

struct session *session_init(const char *dev_path, bool debug, session_frame_cb report_cb, void *report_ctx,
                             session_frame_cb cal_cb, void *cal_ctx) {
  struct session *s = calloc(1, sizeof(*s));
  if (!s)
    return NULL;

  s->debug = debug;
  s->transport = transport_init(dev_path, debug, on_cmd_response, s, report_cb, report_ctx, cal_cb, cal_ctx);
  if (!s->transport) {
    free(s);
    return NULL;
  }

  pthread_mutex_init(&s->cmd_mutex, NULL);
  pthread_mutex_init(&s->resp_mutex, NULL);
  pthread_cond_init(&s->resp_cond, NULL);
  if (s->debug)
    printf("Started LD2410S UART session manager\n");
  return s;
}

void session_free(struct session *s) {
  if (!s)
    return;
  transport_free(s->transport);
  pthread_mutex_destroy(&s->cmd_mutex);
  pthread_mutex_destroy(&s->resp_mutex);
  pthread_cond_destroy(&s->resp_cond);
  free(s);
  if (s->debug)
    printf("Shutdown LD2410S UART session manager\n");
}

int session_start(struct session *s) { return transport_start(s->transport); }

int session_cmd(struct session *s, uint16_t cmd_word, const void *in, size_t in_len, uint8_t *out_buf, size_t out_cap,
                size_t *out_len) {
  pthread_mutex_lock(&s->cmd_mutex);

  if (s->debug)
    printf("LD2410S UART Session: config enable\n");
  uint8_t enable_data[2] = {0x01, 0x00};
  int ret = send_and_wait(s, CMD_ENABLE_CONFIG, enable_data, sizeof(enable_data), NULL, 0, NULL);

  if (ret == 0) {
    if (s->debug)
      printf("LD2410S UART Session: sending command 0x%04X\n", cmd_word);
    ret = send_and_wait(s, cmd_word, in, in_len, out_buf, out_cap, out_len);
  }

  /* Always try to close config mode, even on failure. */
  if (s->debug)
    printf("LD2410S UART Session: config disable\n");
  send_and_wait(s, CMD_END_CONFIG, NULL, 0, NULL, 0, NULL);

  pthread_mutex_unlock(&s->cmd_mutex);
  return ret;
}
