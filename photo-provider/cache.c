#define _GNU_SOURCE
#include "cache.h"

#include "www_session.h"

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

struct entry {
  int fd;
  char *meta;
};

struct pp_cache {
  struct pp_www_session *ws; // borrowed

  bool dump_to_disk;
  char dump_dir[256];
  int dump_counter;

  pthread_mutex_t mu;
  pthread_cond_t cv;

  // Bumped on every invalidate. Worker captures this before a fetch and
  // discards the result if the generation moved.
  uint32_t generation;

  // Single ring indexed by logical position mod buf_cap. Positions split
  // into three ranges:
  //   [head, cursor)   — history, replayable via pop_prev
  //   cursor           — currently delivered photo (-1 if none delivered yet)
  //   (cursor, tail)   — prefetched, not yet delivered
  // Capacity = history_depth + 1 + prefetch_depth.
  struct entry *buf;
  uint32_t buf_cap;
  uint32_t prefetch_depth;
  int64_t head;
  int64_t cursor;
  int64_t tail;

  bool shutdown;
  pthread_t worker;
};

static void entry_close(struct entry *e) {
  if (e->fd >= 0)
    close(e->fd);
  free(e->meta);
  e->fd = -1;
  e->meta = NULL;
}

static struct entry *slot(struct pp_cache *c, int64_t pos) { return &c->buf[(uint64_t)pos % c->buf_cap]; }

static void flush_locked(struct pp_cache *c) {
  for (int64_t i = c->head; i < c->tail; i++)
    entry_close(slot(c, i));
  c->head = 0;
  c->tail = 0;
  c->cursor = -1;
}

void pp_cache_invalidate(void *ud) {
  struct pp_cache *c = ud;
  pthread_mutex_lock(&c->mu);
  flush_locked(c);
  c->generation++;
  pthread_cond_broadcast(&c->cv);
  pthread_mutex_unlock(&c->mu);
}

static void dump_to_disk(struct pp_cache *c, int fd, const char *meta) {
  if (!c->dump_to_disk)
    return;
  mkdir(c->dump_dir, 0755);
  int counter = ++c->dump_counter;
  char path[512];
  snprintf(path, sizeof(path), "%s/%d.jpeg", c->dump_dir, counter);
  FILE *f = fopen(path, "wb");
  if (!f) {
    perror(path);
    return;
  }
  off_t save = lseek(fd, 0, SEEK_CUR);
  lseek(fd, 0, SEEK_SET);
  char buf[8192];
  ssize_t n;
  while ((n = read(fd, buf, sizeof(buf))) > 0)
    fwrite(buf, 1, (size_t)n, f);
  fclose(f);
  lseek(fd, save, SEEK_SET);

  snprintf(path, sizeof(path), "%s/%d.json", c->dump_dir, counter);
  f = fopen(path, "w");
  if (f) {
    fputs(meta ? meta : "", f);
    fclose(f);
  }
  printf("Dumped photo #%d to %s\n", counter, c->dump_dir);
}

// True when prefetch is short of target AND the buffer has (or can make)
// room. Room exists either directly (occupied < buf_cap) or by evicting
// head — but evicting head is only safe when head < cursor, since head ==
// cursor would evict the photo the consumer is currently looking at.
static bool should_fetch_locked(const struct pp_cache *c) {
  int64_t ahead = c->tail - c->cursor - 1;
  if (ahead >= (int64_t)c->prefetch_depth)
    return false;
  int64_t occupied = c->tail - c->head;
  if (occupied < (int64_t)c->buf_cap)
    return true;
  return c->head < c->cursor;
}

static void *worker_main(void *arg) {
  struct pp_cache *c = arg;

  pthread_mutex_lock(&c->mu);
  while (!c->shutdown) {
    while (!c->shutdown && !should_fetch_locked(c))
      pthread_cond_wait(&c->cv, &c->mu);
    if (c->shutdown)
      break;

    uint32_t gen = c->generation;
    pthread_mutex_unlock(&c->mu);

    int fd = -1;
    char *meta = NULL;
    int rc = pp_www_session_fetch_next(c->ws, &fd, &meta);

    pthread_mutex_lock(&c->mu);
    if (rc < 0) {
      pthread_mutex_unlock(&c->mu);
      struct timespec ts = {.tv_sec = 2, .tv_nsec = 0};
      nanosleep(&ts, NULL);
      pthread_mutex_lock(&c->mu);
      continue;
    }
    if (gen != c->generation || c->shutdown) {
      close(fd);
      free(meta);
      continue;
    }

    // Evict head to make room. Stop before evicting the current photo.
    while ((c->tail - c->head) >= (int64_t)c->buf_cap && c->head < c->cursor) {
      entry_close(slot(c, c->head));
      c->head++;
    }
    if ((c->tail - c->head) >= (int64_t)c->buf_cap) {
      // Cursor moved back during the fetch — can't place without evicting
      // something we'd need for pop_prev. Drop this fetch; we'll refetch
      // on the next iteration once room opens up.
      close(fd);
      free(meta);
      continue;
    }

    dump_to_disk(c, fd, meta);
    struct entry *e = slot(c, c->tail);
    e->fd = fd;
    e->meta = meta;
    c->tail++;
    pthread_cond_broadcast(&c->cv);
  }
  flush_locked(c);
  pthread_mutex_unlock(&c->mu);
  return NULL;
}

struct pp_cache *pp_cache_init(const struct pp_cache_params *p) {
  struct pp_cache *c = calloc(1, sizeof(*c));
  if (!c)
    return NULL;
  c->ws = p->ws;
  c->dump_to_disk = p->dump_to_disk;
  strncpy(c->dump_dir, p->dump_dir ? p->dump_dir : "", sizeof(c->dump_dir) - 1);
  c->prefetch_depth = p->cache_depth ? p->cache_depth : 1;
  c->buf_cap = p->history_depth + 1 + c->prefetch_depth;
  c->buf = calloc(c->buf_cap, sizeof(*c->buf));
  if (!c->buf) {
    free(c);
    return NULL;
  }
  for (uint32_t i = 0; i < c->buf_cap; i++)
    c->buf[i].fd = -1;
  c->head = 0;
  c->cursor = -1;
  c->tail = 0;

  pthread_mutex_init(&c->mu, NULL);
  pthread_cond_init(&c->cv, NULL);

  if (pthread_create(&c->worker, NULL, worker_main, c) != 0) {
    pp_cache_free(c);
    return NULL;
  }
  return c;
}

void pp_cache_free(struct pp_cache *c) {
  if (!c)
    return;
  if (c->worker) {
    pthread_mutex_lock(&c->mu);
    c->shutdown = true;
    pthread_cond_broadcast(&c->cv);
    pthread_mutex_unlock(&c->mu);
    pthread_join(c->worker, NULL);
  }
  free(c->buf);
  pthread_mutex_destroy(&c->mu);
  pthread_cond_destroy(&c->cv);
  free(c);
}

int pp_cache_pop(struct pp_cache *c, int *fd_out, char **meta_out, int timeout_ms) {
  struct timespec deadline;
  clock_gettime(CLOCK_REALTIME, &deadline);
  deadline.tv_sec += timeout_ms / 1000;
  deadline.tv_nsec += (timeout_ms % 1000) * 1000000L;
  if (deadline.tv_nsec >= 1000000000L) {
    deadline.tv_sec++;
    deadline.tv_nsec -= 1000000000L;
  }

  pthread_mutex_lock(&c->mu);
  while (c->cursor + 1 >= c->tail) {
    int r = pthread_cond_timedwait(&c->cv, &c->mu, &deadline);
    if (r == ETIMEDOUT) {
      pthread_mutex_unlock(&c->mu);
      return -1;
    }
  }
  c->cursor++;
  struct entry *e = slot(c, c->cursor);
  int caller_fd = dup(e->fd);
  char *caller_meta = e->meta ? strdup(e->meta) : NULL;
  pthread_cond_broadcast(&c->cv);
  pthread_mutex_unlock(&c->mu);

  if (caller_fd < 0) {
    free(caller_meta);
    return -1;
  }
  *fd_out = caller_fd;
  *meta_out = caller_meta;
  return 0;
}

int pp_cache_pop_prev(struct pp_cache *c, int *fd_out, char **meta_out) {
  pthread_mutex_lock(&c->mu);
  if (c->cursor <= c->head) {
    pthread_mutex_unlock(&c->mu);
    return -1;
  }
  c->cursor--;
  struct entry *e = slot(c, c->cursor);
  int caller_fd = dup(e->fd);
  char *caller_meta = e->meta ? strdup(e->meta) : NULL;
  pthread_mutex_unlock(&c->mu);

  if (caller_fd < 0) {
    free(caller_meta);
    return -1;
  }
  *fd_out = caller_fd;
  *meta_out = caller_meta;
  return 0;
}
