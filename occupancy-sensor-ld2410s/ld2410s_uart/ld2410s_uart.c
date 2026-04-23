#include "ld2410s_uart.h"
#include "session.h"
#include "transport.h"

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* --- Command words --- */

#define CMD_READ_FIRMWARE 0x0000
#define CMD_WRITE_SERIAL 0x0010
#define CMD_READ_SERIAL 0x0011
#define CMD_CALIBRATE 0x0009
#define CMD_WRITE_COMMON 0x0070
#define CMD_READ_COMMON 0x0071
#define CMD_WRITE_THRESHOLD 0x0072
#define CMD_READ_THRESHOLD 0x0073
#define CMD_WRITE_SNR 0x0074
#define CMD_READ_SNR 0x0075

/* --- Parameter table --- */

struct param_entry {
  const char *name;
  uint16_t write_cmd;
  uint16_t param_word;
};

static const struct param_entry PARAM_TABLE[] = {
    {"farthest_gate", CMD_WRITE_COMMON, 0x05},
    {"nearest_gate", CMD_WRITE_COMMON, 0x0A},
    {"unmanned_delay", CMD_WRITE_COMMON, 0x06},
    {"status_report_freq", CMD_WRITE_COMMON, 0x02},
    {"dist_report_freq", CMD_WRITE_COMMON, 0x0C},
    {"response_speed", CMD_WRITE_COMMON, 0x0B},
    {"gate_0_trigger", CMD_WRITE_THRESHOLD, 0x00},
    {"gate_0_holding", CMD_WRITE_THRESHOLD, 0x08},
    {"gate_1_trigger", CMD_WRITE_THRESHOLD, 0x01},
    {"gate_1_holding", CMD_WRITE_THRESHOLD, 0x09},
    {"gate_2_trigger", CMD_WRITE_THRESHOLD, 0x02},
    {"gate_2_holding", CMD_WRITE_THRESHOLD, 0x0A},
    {"gate_3_trigger", CMD_WRITE_THRESHOLD, 0x03},
    {"gate_3_holding", CMD_WRITE_THRESHOLD, 0x0B},
    {"gate_4_trigger", CMD_WRITE_THRESHOLD, 0x04},
    {"gate_4_holding", CMD_WRITE_THRESHOLD, 0x0C},
    {"gate_5_trigger", CMD_WRITE_THRESHOLD, 0x05},
    {"gate_5_holding", CMD_WRITE_THRESHOLD, 0x0D},
    {"gate_6_trigger", CMD_WRITE_THRESHOLD, 0x06},
    {"gate_6_holding", CMD_WRITE_THRESHOLD, 0x0E},
    {"gate_7_trigger", CMD_WRITE_THRESHOLD, 0x07},
    {"gate_7_holding", CMD_WRITE_THRESHOLD, 0x0F},
    {"gate_8_trigger_snr", CMD_WRITE_SNR, 0x00},
    {"gate_8_hold_snr", CMD_WRITE_SNR, 0x08},
    {"gate_9_trigger_snr", CMD_WRITE_SNR, 0x01},
    {"gate_9_hold_snr", CMD_WRITE_SNR, 0x09},
    {"gate_10_trigger_snr", CMD_WRITE_SNR, 0x02},
    {"gate_10_hold_snr", CMD_WRITE_SNR, 0x0A},
    {"gate_11_trigger_snr", CMD_WRITE_SNR, 0x03},
    {"gate_11_hold_snr", CMD_WRITE_SNR, 0x0B},
    {"gate_12_trigger_snr", CMD_WRITE_SNR, 0x04},
    {"gate_12_hold_snr", CMD_WRITE_SNR, 0x0C},
    {"gate_13_trigger_snr", CMD_WRITE_SNR, 0x05},
    {"gate_13_hold_snr", CMD_WRITE_SNR, 0x0D},
    {"gate_14_trigger_snr", CMD_WRITE_SNR, 0x06},
    {"gate_14_hold_snr", CMD_WRITE_SNR, 0x0E},
    {"gate_15_trigger_snr", CMD_WRITE_SNR, 0x07},
    {"gate_15_hold_snr", CMD_WRITE_SNR, 0x0F},
    {NULL, 0, 0},
};

/* Common param read order (must match struct LD2410S_common_params field order) */
static const uint16_t COMMON_PARAM_WORDS[] = {0x05, 0x0A, 0x06, 0x02, 0x0C, 0x0B};
#define NUM_COMMON_PARAMS 6

/* --- Internal state --- */

struct LD2410S_uart {
  struct session *session;
  atomic_size_t last_vacancy_count;
  bool debug;
  ld2410s_report_cb report_cb;
  void *report_ctx;
  ld2410s_calibration_cb cal_cb;
  void *cal_ctx;
};

/* --- Helpers --- */

static uint16_t read_u16_le(const uint8_t *p) { return (uint16_t)p[0] | ((uint16_t)p[1] << 8); }

static uint32_t read_u32_le(const uint8_t *p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void write_u16_le(uint8_t *p, uint16_t v) {
  p[0] = v & 0xFF;
  p[1] = (v >> 8) & 0xFF;
}

static void write_u32_le(uint8_t *p, uint32_t v) {
  p[0] = v & 0xFF;
  p[1] = (v >> 8) & 0xFF;
  p[2] = (v >> 16) & 0xFF;
  p[3] = (v >> 24) & 0xFF;
}

/* --- Report/calibration decoders (reader thread) --- */

static void decode_report(void *ctx, const uint8_t *frame, size_t len) {
  struct LD2410S_uart *s = ctx;
  if (len < 5 || !s->report_cb)
    return;

  struct LD2410S_report report;
  uint8_t state = frame[1];
  report.distance = read_u16_le(frame + 2);
  report.occupied = !(state == 0 || state == 1);
  s->report_cb(&report, s->report_ctx);
  if (report.occupied) {
    s->last_vacancy_count = 0;
    atomic_store_explicit(&s->last_vacancy_count, 0, memory_order_relaxed);
  } else {
    atomic_fetch_add_explicit(&s->last_vacancy_count, 1, memory_order_relaxed);
  }
}

static void decode_calibration(void *ctx, const uint8_t *frame, size_t len) {
  struct LD2410S_uart *s = ctx;

  /* frame: [header 4] [payload_len 2] [payload...] [footer 4] */
  if (len < 4 + 2 + 3 + 4 || !s->cal_cb)
    return;

  const uint8_t *payload = frame + 6;
  size_t payload_len = len - 4 - 2 - 4;
  if (payload_len < 3)
    return;

  uint16_t progress = read_u16_le(payload + 1);
  s->cal_cb(progress, s->cal_ctx);
}

/* --- Public API: Lifecycle --- */

struct LD2410S_uart *ld2410s_uart_init(const char *dev_path, bool debug, ld2410s_report_cb report_cb,
                                       void *report_user_data, ld2410s_calibration_cb cal_cb, void *cal_user_data) {
  struct LD2410S_uart *s = calloc(1, sizeof(*s));
  if (!s)
    return NULL;

  atomic_init(&s->last_vacancy_count, 0);
  s->debug = debug;
  s->report_cb = report_cb;
  s->report_ctx = report_user_data;
  s->cal_cb = cal_cb;
  s->cal_ctx = cal_user_data;

  s->session = session_init(dev_path, debug, decode_report, s, decode_calibration, s);
  if (!s->session) {
    free(s);
    return NULL;
  }
  return s;
}

void ld2410s_uart_free(struct LD2410S_uart *s) {
  if (!s)
    return;
  session_free(s->session);
  free(s);
}

int ld2410s_uart_start(struct LD2410S_uart *s) { return session_start(s->session); }

/* --- Public API: Getters --- */

int ld2410s_uart_get_firmware(struct LD2410S_uart *s, uint8_t *out, size_t out_len, size_t *actual_len) {
  if (s->debug)
    printf("Retrieving device firmware...\n");
  return session_cmd(s->session, CMD_READ_FIRMWARE, NULL, 0, out, out_len, actual_len);
}

int ld2410s_uart_get_serial(struct LD2410S_uart *s, char *out, size_t out_len) {
  if (s->debug)
    printf("Retrieving device serial number...\n");

  uint8_t buf[TRANSPORT_MAX_DATA];
  size_t n = 0;
  if (session_cmd(s->session, CMD_READ_SERIAL, NULL, 0, buf, sizeof(buf), &n) < 0)
    return -1;

  if (n < 2) {
    fprintf(stderr, "Err: serial response too short\n");
    return -1;
  }

  uint16_t sn_len = read_u16_le(buf);
  size_t avail = n - 2;
  if (sn_len > avail)
    sn_len = (uint16_t)avail;

  size_t copy = sn_len < out_len - 1 ? sn_len : out_len - 1;
  memcpy(out, buf + 2, copy);
  out[copy] = '\0';

  /* Strip trailing nulls */
  while (copy > 0 && out[copy - 1] == '\0')
    copy--;
  out[copy] = '\0';
  return 0;
}

int ld2410s_uart_get_common_params(struct LD2410S_uart *s, struct LD2410S_common_params *out) {
  if (s->debug)
    printf("Retrieving device common params...\n");

  uint8_t payload[NUM_COMMON_PARAMS * 2];
  for (int i = 0; i < NUM_COMMON_PARAMS; i++)
    write_u16_le(payload + i * 2, COMMON_PARAM_WORDS[i]);

  uint8_t buf[NUM_COMMON_PARAMS * 4];
  size_t n = 0;
  if (session_cmd(s->session, CMD_READ_COMMON, payload, sizeof(payload), buf, sizeof(buf), &n) < 0)
    return -1;

  if (n < sizeof(buf)) {
    fprintf(stderr, "Err: common params response too short (%zu)\n", n);
    return -1;
  }

  out->farthest_gate = read_u32_le(buf + 0 * 4);
  out->nearest_gate = read_u32_le(buf + 1 * 4);
  out->unmanned_delay = read_u32_le(buf + 2 * 4);
  out->status_report_freq = read_u32_le(buf + 3 * 4);
  out->dist_report_freq = read_u32_le(buf + 4 * 4);
  out->response_speed = read_u32_le(buf + 5 * 4);
  return 0;
}

int ld2410s_uart_get_threshold(struct LD2410S_uart *s, struct LD2410S_threshold_params *out) {
  if (s->debug)
    printf("Retrieving device config thresholds...\n");

  uint8_t payload[16 * 2];
  for (int i = 0; i < 16; i++)
    write_u16_le(payload + i * 2, (uint16_t)i);

  uint8_t buf[16 * 4];
  size_t n = 0;
  if (session_cmd(s->session, CMD_READ_THRESHOLD, payload, sizeof(payload), buf, sizeof(buf), &n) < 0)
    return -1;

  if (n < sizeof(buf)) {
    fprintf(stderr, "Err: threshold response too short (%zu)\n", n);
    return -1;
  }

  for (int i = 0; i < 8; i++) {
    out->trigger[i] = read_u32_le(buf + i * 4);
    out->holding[i] = read_u32_le(buf + (i + 8) * 4);
  }
  return 0;
}

int ld2410s_uart_get_snr(struct LD2410S_uart *s, struct LD2410S_snr_params *out) {
  if (s->debug)
    printf("Retrieving device config snr's...\n");

  uint8_t payload[16 * 2];
  for (int i = 0; i < 16; i++)
    write_u16_le(payload + i * 2, (uint16_t)i);

  uint8_t buf[16 * 4];
  size_t n = 0;
  if (session_cmd(s->session, CMD_READ_SNR, payload, sizeof(payload), buf, sizeof(buf), &n) < 0)
    return -1;

  if (n < sizeof(buf)) {
    fprintf(stderr, "Err: SNR response too short (%zu)\n", n);
    return -1;
  }

  for (int i = 0; i < 8; i++) {
    out->trigger[i] = read_u32_le(buf + i * 4);
    out->hold[i] = read_u32_le(buf + (i + 8) * 4);
  }
  return 0;
}

/* --- Public API: Setters --- */

int ld2410s_uart_set_serial(struct LD2410S_uart *s, const char *serial) {
  if (s->debug)
    printf("Set device serial number to '%s'...\n", serial);

  uint8_t payload[2 + 8];
  write_u16_le(payload, 8);
  memset(payload + 2, 0, 8);
  size_t len = strlen(serial);
  if (len > 8)
    len = 8;
  memcpy(payload + 2, serial, len);

  return session_cmd(s->session, CMD_WRITE_SERIAL, payload, sizeof(payload), NULL, 0, NULL);
}

int ld2410s_uart_set_param(struct LD2410S_uart *s, const char *name, uint32_t value) {
  if (s->debug)
    printf("Set device config %s=%d...\n", name, value);

  for (const struct param_entry *p = PARAM_TABLE; p->name; p++) {
    if (strcmp(p->name, name) == 0) {
      uint8_t payload[6];
      write_u16_le(payload, p->param_word);
      write_u32_le(payload + 2, value);
      return session_cmd(s->session, p->write_cmd, payload, sizeof(payload), NULL, 0, NULL);
    }
  }
  fprintf(stderr, "Err: unknown param '%s'\n", name);
  return -1;
}

/* --- Public API: Calibration --- */

int ld2410s_uart_start_calibration(struct LD2410S_uart *s, uint16_t trigger, uint16_t retention,
                                   uint16_t duration_secs) {
  uint8_t cal_data[6];
  write_u16_le(cal_data + 0, trigger);
  write_u16_le(cal_data + 2, retention);
  write_u16_le(cal_data + 4, duration_secs);
  return session_cmd(s->session, CMD_CALIBRATE, cal_data, sizeof(cal_data), NULL, 0, NULL);
}

size_t ld2410s_uart_get_vacant_reports_count(struct LD2410S_uart *s) {
  return atomic_load_explicit(&s->last_vacancy_count, memory_order_relaxed);
}
