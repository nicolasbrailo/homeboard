#define _GNU_SOURCE
#include "www_session.h"

#include <curl/curl.h>
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

  // All HTTP happens on the fetch (cache worker) thread, so everything below
  // that isn't atomic is private to it.
  CURL *curl;
  char client_id[MAX_CLIENT_ID];

  // Monotonic seconds of the last successful registration or fetch. The
  // server evicts idle clients, so when a fetch sees this stale we
  // re-register before talking to the server.
  long last_activity_s;

  // Written by setters on the dbus thread, read by the fetch thread.
  atomic_uint target_w;
  atomic_uint target_h;
  atomic_bool embed_qr;

  // Set by setters (and at init) to ask the fetch thread to register with the
  // current config before its next fetch. Stays set until a registration
  // succeeds.
  atomic_bool needs_register;
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

static int do_get_mem(struct pp_www_session *s, const char *url,
                      struct mem_buf *out) {
  CURL *curl = s->curl;
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

// Performs /client_register and returns the id in `out`.
static int http_register(struct pp_www_session *s, char *out, size_t out_sz) {
  char url[512];
  snprintf(url, sizeof(url), "%s/client_register", s->url_base);
  struct mem_buf b = {0};
  int r = do_get_mem(s, url, &b);
  if (r < 0) {
    free(b.data);
    return -1;
  }
  size_t start = 0, end = b.len;
  while (start < end && (b.data[start] == ' ' || b.data[start] == '\n' ||
                         b.data[start] == '\r' || b.data[start] == '\t'))
    start++;
  while (end > start && (b.data[end - 1] == ' ' || b.data[end - 1] == '\n' ||
                         b.data[end - 1] == '\r' || b.data[end - 1] == '\t'))
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

static int http_push_embed_qr(struct pp_www_session *s, const char *id,
                              bool v) {
  char url[512];
  snprintf(url, sizeof(url), "%s/client_cfg/%s/embed_info_qr_code/%s",
           s->url_base, id, v ? "true" : "false");
  struct mem_buf b = {0};
  int r = do_get_mem(s, url, &b);
  free(b.data);
  return r;
}

static int http_push_target_size(struct pp_www_session *s, const char *id,
                                 uint32_t w, uint32_t h) {
  char url[512];
  snprintf(url, sizeof(url), "%s/client_cfg/%s/target_size/%ux%u", s->url_base,
           id, w, h);
  struct mem_buf b = {0};
  int r = do_get_mem(s, url, &b);
  free(b.data);
  return r;
}

// Registers with the server and pushes the current config. Fetch thread only.
// The caller must clear needs_register *before* calling this, so that a setter
// racing with us (after we read the config below) sets it again and triggers
// another registration with its new values.
static int reregister(struct pp_www_session *s) {
  char new_id[MAX_CLIENT_ID];
  if (http_register(s, new_id, sizeof(new_id)) < 0) {
    fprintf(stderr, "re-register failed\n");
    return -1;
  }
  uint32_t tw = atomic_load(&s->target_w);
  uint32_t th = atomic_load(&s->target_h);
  bool qr = atomic_load(&s->embed_qr);
  // A registration without our config would serve wrong-sized photos until
  // the next config change; fail it so it's retried.
  if (http_push_embed_qr(s, new_id, qr) < 0 ||
      http_push_target_size(s, new_id, tw, th) < 0) {
    fprintf(stderr, "re-register failed: can't push config\n");
    return -1;
  }

  strncpy(s->client_id, new_id, sizeof(s->client_id) - 1);
  s->client_id[sizeof(s->client_id) - 1] = '\0';
  s->last_activity_s = now_monotonic_s();
  printf("Registered with server, client_id=%s (%ux%u, embed_qr=%d)\n",
         new_id, tw, th, qr);
  return 0;
}

struct pp_www_session *pp_www_session_init(const char *server_url,
                                           uint32_t target_w, uint32_t target_h,
                                           bool embed_qr,
                                           uint32_t connect_timeout_s,
                                           uint32_t request_timeout_s) {
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
  atomic_init(&s->needs_register, true);

  s->curl = curl_easy_init();
  if (!s->curl) {
    fprintf(stderr, "pp_www_session_init: failed to setup curl\n");
    pp_www_session_free(s);
    return NULL;
  }
  return s;
}

void pp_www_session_free(struct pp_www_session *s) {
  if (!s)
    return;
  if (s->curl)
    curl_easy_cleanup(s->curl);
  free(s);
}

// Setters only record the new value and flag a registration; the fetch thread
// does the HTTP. This keeps the dbus thread from blocking on the network.
int pp_www_session_set_target_size(struct pp_www_session *s, uint32_t w,
                                   uint32_t h) {
  if (w < IMG_MIN_SZ || h < IMG_MIN_SZ) {
    fprintf(
        stderr,
        "pp_www_session_set_target_size: Requested target size too small\n");
    return -1;
  }
  if (w > IMG_MAX_SZ || h > IMG_MAX_SZ) {
    fprintf(stderr,
            "pp_www_session_set_target_size: Requested target size too big\n");
    return -1;
  }
  uint32_t ow = atomic_exchange(&s->target_w, w);
  uint32_t oh = atomic_exchange(&s->target_h, h);
  if (ow == w && oh == h)
    return 0;
  atomic_store(&s->needs_register, true);
  return 1;
}

int pp_www_session_set_embed_qr(struct pp_www_session *s, bool v) {
  bool ov = atomic_exchange(&s->embed_qr, v);
  if (ov == v)
    return 0;
  atomic_store(&s->needs_register, true);
  return 1;
}

static int fetch_img_into_fd(struct pp_www_session *s, const char *id) {
  int fd = memfd_create("photo", MFD_CLOEXEC);
  if (fd < 0) {
    perror("memfd_create");
    return -1;
  }
  char url[512];
  snprintf(url, sizeof(url), "%s/get_next_img/%s", s->url_base, id);

  curl_easy_reset(s->curl);
  curl_easy_setopt(s->curl, CURLOPT_CONNECTTIMEOUT, s->connect_timeout_s);
  curl_easy_setopt(s->curl, CURLOPT_TIMEOUT, s->request_timeout_s);
  curl_easy_setopt(s->curl, CURLOPT_FOLLOWLOCATION, 1L);
  curl_easy_setopt(s->curl, CURLOPT_URL, url);
  curl_easy_setopt(s->curl, CURLOPT_WRITEFUNCTION, write_to_fd);
  curl_easy_setopt(s->curl, CURLOPT_WRITEDATA, &fd);
  CURLcode rc = curl_easy_perform(s->curl);
  if (rc != CURLE_OK) {
    fprintf(stderr, "curl %s: %s\n", url, curl_easy_strerror(rc));
    close(fd);
    return -1;
  }
  long code = 0;
  curl_easy_getinfo(s->curl, CURLINFO_RESPONSE_CODE, &code);
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
  if (do_get_mem(s, url, &b) < 0) {
    free(b.data);
    return NULL;
  }
  if (!b.data)
    return strdup("");
  return b.data;
}

int pp_www_session_fetch_next(struct pp_www_session *s, int *fd_out,
                              char **meta_out) {
  // Server evicts idle clients, so refresh the registration if we've been
  // quiet too long.
  if (now_monotonic_s() - s->last_activity_s > CLIENT_STALE_S)
    atomic_store(&s->needs_register, true);

  // Clear the flag before registering: a setter that changes the config while
  // we register sets it again, so its values get their own registration.
  if (atomic_exchange(&s->needs_register, false)) {
    if (reregister(s) < 0) {
      atomic_store(&s->needs_register, true);
      return -1;
    }
  }

  int fd = fetch_img_into_fd(s, s->client_id);
  if (fd < 0)
    return -1;

  char *meta = fetch_meta_str(s, s->client_id);
  if (!meta) {
    fprintf(stderr, "meta fetch failed; serving image with empty metadata\n");
    meta = strdup("{}");
  }

  s->last_activity_s = now_monotonic_s();
  *fd_out = fd;
  *meta_out = meta;
  return 0;
}
