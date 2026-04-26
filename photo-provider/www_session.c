#define _GNU_SOURCE
#include "www_session.h"

#include <curl/curl.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#define IMG_MIN_SZ 128
#define IMG_MAX_SZ 3840
#define MAX_CLIENT_ID 128
#define CLIENT_STALE_S (5 * 60)

struct pp_www_session {
  char url_base[256];
  long connect_timeout_s;
  long request_timeout_s;

  // Separate CURL handles so fetch (worker thread) and ctrl (dbus thread)
  // never share curl state.
  CURL *curl_fetch;
  CURL *curl_ctrl;

  atomic_uint target_w;
  atomic_uint target_h;
  atomic_bool embed_qr;

  // client_id is mutated by re-register (ctrl path) and read by the fetch
  // path. Guarded by id_mu. Readers copy under the lock and release before
  // doing IO.
  pthread_mutex_t id_mu;
  char client_id[MAX_CLIENT_ID];

  // Monotonic seconds of the last successful registration or fetch. The
  // server evicts idle clients, so when a fetch sees this stale we
  // re-register before talking to the server.
  atomic_long last_activity_s;

  pp_ws_invalidate_fn on_invalidate;
  void *on_invalidate_ud;
};

struct mem_buf {
  char *data;
  size_t len;
  size_t cap;
};

static long now_monotonic_s(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (long)ts.tv_sec;
}

static size_t write_to_mem(void *ptr, size_t size, size_t nmemb, void *ud) {
  struct mem_buf *b = ud;
  size_t n = size * nmemb;
  if (b->len + n + 1 > b->cap) {
    size_t nc = b->cap ? b->cap * 2 : 4096;
    while (nc < b->len + n + 1)
      nc *= 2;
    char *p = realloc(b->data, nc);
    if (!p)
      return 0;
    b->data = p;
    b->cap = nc;
  }
  memcpy(b->data + b->len, ptr, n);
  b->len += n;
  b->data[b->len] = '\0';
  return n;
}

static size_t write_to_fd(void *ptr, size_t size, size_t nmemb, void *ud) {
  int fd = *(int *)ud;
  size_t n = size * nmemb;
  size_t done = 0;
  while (done < n) {
    ssize_t w = write(fd, (char *)ptr + done, n - done);
    if (w <= 0)
      return 0;
    done += (size_t)w;
  }
  return n;
}

static int do_get_mem(struct pp_www_session *s, CURL *curl, const char *url, struct mem_buf *out) {
  curl_easy_reset(curl);
  curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, s->connect_timeout_s);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT, s->request_timeout_s);
  curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
  curl_easy_setopt(curl, CURLOPT_URL, url);
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_to_mem);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, out);
  CURLcode rc = curl_easy_perform(curl);
  if (rc != CURLE_OK) {
    fprintf(stderr, "curl %s: %s\n", url, curl_easy_strerror(rc));
    return -1;
  }
  long code = 0;
  curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
  if (code < 200 || code >= 300) {
    fprintf(stderr, "curl %s: HTTP %ld\n", url, code);
    return -1;
  }
  return 0;
}

// Performs /client_register and returns the id in `out`. Uses curl_ctrl.
static int http_register(struct pp_www_session *s, char *out, size_t out_sz) {
  char url[512];
  snprintf(url, sizeof(url), "%s/client_register", s->url_base);
  struct mem_buf b = {0};
  int r = do_get_mem(s, s->curl_ctrl, url, &b);
  if (r < 0) {
    free(b.data);
    return -1;
  }
  size_t start = 0, end = b.len;
  while (start < end &&
         (b.data[start] == ' ' || b.data[start] == '\n' || b.data[start] == '\r' || b.data[start] == '\t'))
    start++;
  while (end > start &&
         (b.data[end - 1] == ' ' || b.data[end - 1] == '\n' || b.data[end - 1] == '\r' || b.data[end - 1] == '\t'))
    end--;
  size_t n = end - start;
  if (n == 0 || n >= out_sz) {
    free(b.data);
    return -1;
  }
  memcpy(out, b.data + start, n);
  out[n] = '\0';
  free(b.data);
  return 0;
}

static int http_push_embed_qr(struct pp_www_session *s, const char *id, bool v) {
  char url[512];
  snprintf(url, sizeof(url), "%s/client_cfg/%s/embed_info_qr_code/%s", s->url_base, id, v ? "true" : "false");
  struct mem_buf b = {0};
  int r = do_get_mem(s, s->curl_ctrl, url, &b);
  free(b.data);
  return r;
}

static int http_push_target_size(struct pp_www_session *s, const char *id, uint32_t w, uint32_t h) {
  char url[512];
  snprintf(url, sizeof(url), "%s/client_cfg/%s/target_size/%ux%u", s->url_base, id, w, h);
  struct mem_buf b = {0};
  int r = do_get_mem(s, s->curl_ctrl, url, &b);
  free(b.data);
  return r;
}

// Registers with the server and pushes the current config. Called on
// init and whenever a setter flips a value. Fires on_invalidate after the
// new client_id is installed.
static int reregister(struct pp_www_session *s) {
  char new_id[MAX_CLIENT_ID];
  if (http_register(s, new_id, sizeof(new_id)) < 0) {
    fprintf(stderr, "re-register failed\n");
    return -1;
  }
  uint32_t tw = atomic_load(&s->target_w);
  uint32_t th = atomic_load(&s->target_h);
  bool qr = atomic_load(&s->embed_qr);
  http_push_embed_qr(s, new_id, qr);
  http_push_target_size(s, new_id, tw, th);

  pthread_mutex_lock(&s->id_mu);
  strncpy(s->client_id, new_id, sizeof(s->client_id) - 1);
  s->client_id[sizeof(s->client_id) - 1] = '\0';
  pthread_mutex_unlock(&s->id_mu);
  atomic_store(&s->last_activity_s, now_monotonic_s());
  printf("Registered with server, client_id=%s\n", new_id);

  if (s->on_invalidate)
    s->on_invalidate(s->on_invalidate_ud);
  return 0;
}

struct pp_www_session *pp_www_session_init(const char *server_url, uint32_t target_w, uint32_t target_h, bool embed_qr,
                                           uint32_t connect_timeout_s, uint32_t request_timeout_s) {
  if (!server_url || server_url[0] == '\0') {
    fprintf(stderr, "pp_www_session_init: empty server_url\n");
    return NULL;
  }
  if (target_w < IMG_MIN_SZ || target_h < IMG_MIN_SZ) {
    fprintf(stderr, "pp_www_session_init: Requested target size too small\n");
    return NULL;
  }
  if (target_w > IMG_MAX_SZ || target_h > IMG_MAX_SZ) {
    fprintf(stderr, "pp_www_session_init: Requested target size too big\n");
    return NULL;
  }
  if (connect_timeout_s == 0 || request_timeout_s == 0) {
    fprintf(stderr, "pp_www_session_init: timeouts must be non-zero\n");
    return NULL;
  }

  struct pp_www_session *s = calloc(1, sizeof(*s));
  if (!s)
    return NULL;

  // Copy url_base, strip trailing '/' if present
  strncpy(s->url_base, server_url, sizeof(s->url_base) - 1);
  const size_t bl = strlen(s->url_base);
  if (s->url_base[bl - 1] == '/')
    s->url_base[bl - 1] = '\0';

  s->connect_timeout_s = (long)connect_timeout_s;
  s->request_timeout_s = (long)request_timeout_s;

  atomic_init(&s->target_w, target_w);
  atomic_init(&s->target_h, target_h);
  atomic_init(&s->embed_qr, embed_qr);
  atomic_init(&s->last_activity_s, 0);
  pthread_mutex_init(&s->id_mu, NULL);

  s->curl_fetch = curl_easy_init();
  s->curl_ctrl = curl_easy_init();
  if (!s->curl_fetch || !s->curl_ctrl) {
    fprintf(stderr, "pp_www_session_init: failed to setup curl\n");
    pp_www_session_free(s);
    return NULL;
  }
  return s;
}

int pp_www_session_start(struct pp_www_session *s, pp_ws_invalidate_fn on_invalidate, void *ud) {
  s->on_invalidate = on_invalidate;
  s->on_invalidate_ud = ud;
  return reregister(s);
}

void pp_www_session_free(struct pp_www_session *s) {
  if (!s)
    return;
  if (s->curl_fetch)
    curl_easy_cleanup(s->curl_fetch);
  if (s->curl_ctrl)
    curl_easy_cleanup(s->curl_ctrl);
  pthread_mutex_destroy(&s->id_mu);
  free(s);
}

int pp_www_session_set_target_size(struct pp_www_session *s, uint32_t w, uint32_t h) {
  if (w < IMG_MIN_SZ || h < IMG_MIN_SZ) {
    fprintf(stderr, "pp_www_session_set_target_size: Requested target size too small\n");
    return -1;
  }
  if (w > IMG_MAX_SZ || h > IMG_MAX_SZ) {
    fprintf(stderr, "pp_www_session_set_target_size: Requested target size too big\n");
    return -1;
  }
  uint32_t ow = atomic_exchange(&s->target_w, w);
  uint32_t oh = atomic_exchange(&s->target_h, h);
  if (ow == w && oh == h)
    return 0;
  return reregister(s);
}

int pp_www_session_set_embed_qr(struct pp_www_session *s, bool v) {
  bool ov = atomic_exchange(&s->embed_qr, v);
  if (ov == v)
    return 0;
  return reregister(s);
}

static int fetch_img_into_fd(struct pp_www_session *s, const char *id) {
  int fd = memfd_create("photo", MFD_CLOEXEC);
  if (fd < 0) {
    perror("memfd_create");
    return -1;
  }
  char url[512];
  snprintf(url, sizeof(url), "%s/get_next_img/%s", s->url_base, id);

  curl_easy_reset(s->curl_fetch);
  curl_easy_setopt(s->curl_fetch, CURLOPT_CONNECTTIMEOUT, s->connect_timeout_s);
  curl_easy_setopt(s->curl_fetch, CURLOPT_TIMEOUT, s->request_timeout_s);
  curl_easy_setopt(s->curl_fetch, CURLOPT_FOLLOWLOCATION, 1L);
  curl_easy_setopt(s->curl_fetch, CURLOPT_URL, url);
  curl_easy_setopt(s->curl_fetch, CURLOPT_WRITEFUNCTION, write_to_fd);
  curl_easy_setopt(s->curl_fetch, CURLOPT_WRITEDATA, &fd);
  CURLcode rc = curl_easy_perform(s->curl_fetch);
  if (rc != CURLE_OK) {
    fprintf(stderr, "curl %s: %s\n", url, curl_easy_strerror(rc));
    close(fd);
    return -1;
  }
  long code = 0;
  curl_easy_getinfo(s->curl_fetch, CURLINFO_RESPONSE_CODE, &code);
  if (code < 200 || code >= 300) {
    fprintf(stderr, "curl %s: HTTP %ld\n", url, code);
    close(fd);
    return -1;
  }
  if (lseek(fd, 0, SEEK_SET) < 0) {
    perror("lseek");
    close(fd);
    return -1;
  }
  return fd;
}

static char *fetch_meta_str(struct pp_www_session *s, const char *id) {
  char url[512];
  snprintf(url, sizeof(url), "%s/get_current_img_meta/%s", s->url_base, id);
  struct mem_buf b = {0};
  if (do_get_mem(s, s->curl_fetch, url, &b) < 0) {
    free(b.data);
    return NULL;
  }
  if (!b.data)
    return strdup("");
  return b.data;
}

int pp_www_session_fetch_next(struct pp_www_session *s, int *fd_out, char **meta_out) {
  char id[MAX_CLIENT_ID];

  // Server evicts idle clients, so refresh the registration if we've been
  // quiet too long. last_activity_s == 0 means start() hasn't registered
  // yet — leave that to start(); this path must not race against it on
  // the shared ctrl curl handle.
  long last = atomic_load(&s->last_activity_s);
  if (last != 0 && now_monotonic_s() - last > CLIENT_STALE_S) {
    if (reregister(s) < 0)
      fprintf(stderr, "stale-client re-register failed; attempting fetch with old id\n");
  }

  // Snapshots the current client_id into `out`. Returns -1 if no id is set
  // (shouldn't happen after a successful init, but guards against the brief
  // window when a re-register is between failing and retrying).
  pthread_mutex_lock(&s->id_mu);
  if (s->client_id[0] == '\0') {
    fprintf(stderr, "Failed to fetch new image: client not registered or invalid id received\n");
    pthread_mutex_unlock(&s->id_mu);
    return -1;
  }
  strncpy(id, s->client_id, sizeof(id) - 1);
  id[sizeof(id) - 1] = '\0';
  pthread_mutex_unlock(&s->id_mu);

  int fd = fetch_img_into_fd(s, id);
  if (fd < 0)
    return -1;

  char *meta = fetch_meta_str(s, id);
  if (!meta) {
    fprintf(stderr, "meta fetch failed; serving image with empty metadata\n");
    meta = strdup("{}");
  }

  atomic_store(&s->last_activity_s, now_monotonic_s());
  *fd_out = fd;
  *meta_out = meta;
  return 0;
}
