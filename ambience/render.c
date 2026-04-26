#include "render.h"
#include "photo_client.h"
#include "eink_meta.h"

#include "jpeg_render/img_render.h"
#include "jpeg_render/jpeg_loader.h"

#include <semaphore.h>
#include <errno.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stdio.h>
#include <pthread.h>
#include <stdatomic.h>

struct RenderCtx {
  pthread_t render_thread;
  atomic_bool thread_running;

  // DRM
  pthread_mutex_t drm_mutex;
  uint32_t* fb;
  struct fb_info fbi;
  uint32_t* scratch_fb;
  // Atomic so wait_or_stop can probe readiness without taking drm_mutex. The
  // mutex still owns coherence across fb/scratch_fb/fbi/sz for actual rendering;
  // anyone planning to *use* the fb pointers must still take it.
  atomic_size_t scratch_fb_sz;

  pthread_mutex_t img_cfg_mutex;
  struct img_render_cfg img_cfg;

  atomic_uint transition_time_s;
  atomic_int skip_count;
  atomic_bool slideshow_active;
  sem_t wake_sem;
  char *fallback_img_path;

  render_pre_commit_cb_t render_pre_commit_cb;
  void *render_pre_commit_cb_ud;

  // Downstream deps
  struct PhotoClient* photo_client;
  struct EinkMeta *eink;
};

// Call holding drm lock
static bool render_fd(struct RenderCtx *s, int fd) {
  const struct img_render_cfg cfg = s->img_cfg;
  const struct fb_info fbi = s->fbi;

  struct jpeg_image *img = jpeg_load_fd(fd, fbi.width, fbi.height);
  if (!img) {
    fprintf(stderr, "jpeg decode failed\n");
    return false;
  }
  img_render(s->scratch_fb, fbi.width, fbi.height, fbi.stride,
             img->pixels, img->width, img->height, &cfg);
  jpeg_free(img);
  return true;
}

static bool render_fallback(struct RenderCtx *s) {
  pthread_mutex_lock(&s->img_cfg_mutex);
  const struct img_render_cfg cfg = s->img_cfg;
  const struct fb_info fbi = s->fbi;
  pthread_mutex_unlock(&s->img_cfg_mutex);

  if (!s->fallback_img_path)
    return false;
  struct jpeg_image *img = jpeg_load(s->fallback_img_path, fbi.width, fbi.height);
  if (!img) {
    fprintf(stderr, "fallback image decode failed: %s\n", s->fallback_img_path);
    return false;
  }
  img_render(s->scratch_fb, fbi.width, fbi.height, fbi.stride,
             img->pixels, img->width, img->height,
             &cfg);
  jpeg_free(img);
  memcpy(s->fb, s->scratch_fb,
         atomic_load_explicit(&s->scratch_fb_sz, memory_order_relaxed));
  printf("Rendered fallback image: %s\n", s->fallback_img_path);
  return true;
}

// Blocks until the slideshow is ready to advance (active + fb attached) AND
// either the transition timer fires or a press/wake arrives. Returns true if
// the thread was asked to stop. On false return, the caller can fetch + render
// the next photo (the in-mutex active check still guards the fb write in case
// pause races with the fetch).
//
// Behaviour:
//  - Not ready (paused or no fb): wait indefinitely for any state change.
//  - Just transitioned not-ready → ready: return immediately, so a resume or
//    fb arrival doesn't make the user wait a full transition for the first
//    photo.
//  - Ready: wait transition_time_s, return on timeout or sem post (re-checking
//    that state is still ready, otherwise loop back to the not-ready branch).
static bool wait_or_stop(struct RenderCtx *s) {
  bool was_ready = true;

  for (;;) {
    if (!atomic_load_explicit(&s->thread_running, memory_order_relaxed))
      return true;

    bool can_render =
        atomic_load_explicit(&s->slideshow_active, memory_order_relaxed) &&
        atomic_load_explicit(&s->scratch_fb_sz, memory_order_relaxed) > 0;

    if (!can_render) {
      was_ready = false;
      // We're not ready to render, wait until someone wakes us up
      if (sem_wait(&s->wake_sem) < 0 && errno != EINTR) {
        perror("sem_wait");
        return true;
      }
      continue;
    }

    if (!was_ready) {
      // This is the second loop after we were not ready to render:
      // 1st loop -> Not ready, sem_wait then reenter loop
      // 2nd loop -> can_render is now true, was_ready = false
      // This means something recently changed to enable rendering; in this case, shortcircuit
      // the normal wait (we don't wait for deadline, we try to render something ASAP)
      return false;
    }

    struct timespec deadline;
    clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_sec += atomic_load_explicit(&s->transition_time_s, memory_order_relaxed);
    int r = sem_timedwait(&s->wake_sem, &deadline);
    if (r == 0) {
      // Wakeup while ready. If state is still ready it was a press → fetch.
      // If pause raced with the wait, fall back to the !ready branch.
      if (atomic_load_explicit(&s->slideshow_active, memory_order_relaxed) &&
          atomic_load_explicit(&s->scratch_fb_sz, memory_order_relaxed) > 0)
        return false;
      continue;
    }
    if (errno == ETIMEDOUT)
      return false;
    if (errno == EINTR)
      continue;
    perror("sem_timedwait");
    return true;
  }
}

static void *render_thread_fn(void *arg) {
  struct RenderCtx *s = arg;
  while (true) {
    if (wait_or_stop(s))
      break;

    // Collapse any queued next/prev presses into a single step. Drain the sem
    // FIRST so that every post we observe (including the one that woke us) has
    // already committed its skip_count update, then read the counter.
    while (sem_trywait(&s->wake_sem) == 0) {
    }
    int skip = atomic_exchange(&s->skip_count, 0);
    const char *method = (skip < 0) ? "GetPrevPhoto" : "GetPhoto";

    int fd;
    char *meta = NULL;
    photo_client_fetch_one(s->photo_client, method, &fd, &meta);

    // Re-check active under the mutex: if pause raced with our fetch, the pause
    // path has already painted the fallback and we must not stomp on it.
    bool rendered = false;
    pthread_mutex_lock(&s->drm_mutex);
    if (s->scratch_fb && atomic_load_explicit(&s->slideshow_active, memory_order_relaxed)) {
      render_fd(s, fd);
      s->render_pre_commit_cb(s->render_pre_commit_cb_ud, s->scratch_fb, &s->fbi);
      memcpy(s->fb, s->scratch_fb,
             atomic_load_explicit(&s->scratch_fb_sz, memory_order_relaxed));
      rendered = true;
    }
    pthread_mutex_unlock(&s->drm_mutex);
    close(fd);

    if (rendered && s->eink) {
      eink_meta_render(s->eink, meta);
    } else {
      printf("Discarding fetched picture, slideshow became inactive.\n");
    }

    free(meta);
  }

  return NULL;
}

struct RenderCtx* render_init(render_pre_commit_cb_t cb, void* render_pre_commit_cb_ud,
      const char* fallback_img_path, uint32_t transition_time_s,
      bool use_eink,
      const struct img_render_cfg *img_cfg) {
  struct RenderCtx* s = calloc(1, sizeof(*s));
  atomic_init(&s->thread_running, true);
  pthread_mutex_init(&s->drm_mutex, NULL);
  pthread_mutex_init(&s->img_cfg_mutex, NULL);
  s->fb = NULL;
  memset(&s->fbi, 0, sizeof(s->fbi));
  s->scratch_fb = NULL;
  atomic_init(&s->scratch_fb_sz, 0);
  atomic_init(&s->transition_time_s, 30);
  slideshow_set_transition_time_s(s, transition_time_s);
  s->img_cfg = *img_cfg;
  atomic_init(&s->skip_count, 0);
  atomic_init(&s->slideshow_active, false);
  s->fallback_img_path = fallback_img_path? strdup(fallback_img_path) : NULL;
  s->render_pre_commit_cb = cb;
  s->render_pre_commit_cb_ud = render_pre_commit_cb_ud;
  render_fallback(s);

  if (sem_init(&s->wake_sem, 0, 0) != 0) {
    perror("sem_init");
    render_free(s);
    return NULL;
  }

  s->photo_client = photo_client_init();
  if (!s->photo_client) {
    render_free(s);
    return NULL;
  }

  s->eink = use_eink? eink_meta_init() : NULL;
  if (use_eink && !s->eink) {
    render_free(s);
    return NULL;
  }

  if (pthread_create(&s->render_thread, NULL, render_thread_fn, s) != 0) {
    perror("pthread_create");
    return NULL;
  }

  return s;
}

void render_set_fb(struct RenderCtx* s, uint32_t* fb, const struct fb_info* fbi) {
  void *scratch = fb? malloc(fbi->height * fbi->stride) : NULL;
  pthread_mutex_lock(&s->drm_mutex);
  s->fb = fb;
  s->fbi = *fbi;
  atomic_store_explicit(&s->scratch_fb_sz,
                        (size_t)s->fbi.height * s->fbi.stride,
                        memory_order_relaxed);
  free(s->scratch_fb);
  s->scratch_fb = scratch;
  if (!atomic_load_explicit(&s->slideshow_active, memory_order_relaxed))
    render_fallback(s);
  pthread_mutex_unlock(&s->drm_mutex);
  sem_post(&s->wake_sem);
}

void render_free(struct RenderCtx* s) {
  if (!s)
    return;

  if (s->thread_running) {
    atomic_store_explicit(&s->thread_running, false, memory_order_relaxed);
    sem_post(&s->wake_sem);
    pthread_join(s->render_thread, NULL);
  }

  photo_client_free(s->photo_client);
  if (s->eink) {
    eink_meta_clear(s->eink);
    eink_meta_free(s->eink);
  }
  sem_destroy(&s->wake_sem);

  // Make sure we don't race with set_fb even if it was called after the destructor started
  pthread_mutex_lock(&s->drm_mutex);
  if (!render_fallback(s)) {
    // If fallback image failed, clear fb to black
    memset(s->fb, 0,
           atomic_load_explicit(&s->scratch_fb_sz, memory_order_relaxed));
  }
  pthread_mutex_unlock(&s->drm_mutex);

  free(s->fallback_img_path);
  free(s->scratch_fb);
  pthread_mutex_destroy(&s->drm_mutex);
  pthread_mutex_destroy(&s->img_cfg_mutex);
  free(s);
}

void render_slideshow_next(struct RenderCtx *s) {
  if (!s->thread_running)
    return;
  printf("User requested to advance to the next picture\n");
  atomic_fetch_add(&s->skip_count, 1);
  sem_post(&s->wake_sem);
}

void render_slideshow_prev(struct RenderCtx *s) {
  if (!s->thread_running)
    return;
  printf("User requested to step back to the previous picture\n");
  atomic_fetch_sub(&s->skip_count, 1);
  sem_post(&s->wake_sem);
}

void render_slideshow_set_active(struct RenderCtx *s, bool active) {
  bool prev = atomic_exchange(&s->slideshow_active, active);
  if (prev == active)
    return;
  printf("Slideshow %s\n", active ? "active" : "paused");
  if (!active) {
    pthread_mutex_lock(&s->drm_mutex);
    if (s->fb)
      render_fallback(s);
    pthread_mutex_unlock(&s->drm_mutex);
  } else {
    pthread_mutex_lock(&s->drm_mutex);
    const uint32_t w = s->fbi.width;
    const uint32_t h = s->fbi.height;
    pthread_mutex_unlock(&s->drm_mutex);
    if (push_initial_config(s->photo_client, w, h, false) != 0) {
      fprintf(stderr, "Failed to setup photo-provider config, will use defaults\n");
    }
  }
  // Wake the render thread so it picks up the new state immediately:
  // on pause it stops fetching; on resume it fetches a fresh photo.
  sem_post(&s->wake_sem);
}

bool slideshow_set_transition_time_s(struct RenderCtx *s, uint32_t seconds) {
  if (seconds < 3 || seconds > 300) {
    fprintf(stderr, "slideshow_set_transition_time_s: invalid transition time %u, must be [3, 300]\n", seconds);
    return false;
  }
  atomic_store(&s->transition_time_s, seconds);
  printf("Transition time updated to %u seconds\n", seconds);
  return true;
}

void render_set_img_render_config(struct RenderCtx *s, const struct img_render_cfg* cfg) {
  pthread_mutex_lock(&s->img_cfg_mutex);
  s->img_cfg = *cfg;
  pthread_mutex_unlock(&s->img_cfg_mutex);
  // Simulate a call to next to implement the new render cfg
  render_slideshow_next(s);
}

