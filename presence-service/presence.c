#include "presence.h"

#include <stdio.h>
#include <stdlib.h>
#include <time.h>

struct Presence {
  unsigned sensor_stabilization_delay_secs;
  unsigned hyst_occupied;
  unsigned hyst_vacant;

  unsigned silent_hyst_occupied;
  unsigned silent_hyst_vacant;
  int silent_start_min; /* minutes since local midnight, or <0 if disabled */
  int silent_end_min;

  unsigned ticks; /* raw reports received since the sensor came up */

  bool stable_known;      /* first stable value committed since sensor up */
  bool stable_state;      /* post-hysteresis occupancy */
  bool pending_state;     /* last raw value seen */
  unsigned pending_count; /* consecutive raw reports of pending_state */

  bool force_off_latched;

  int announced; /* -1 = not yet announced, 0/1 = last announced value */

  presence_change_cb cb;
  void *ud;
};

// True if the current local wall-clock time falls in the silent window.
static bool in_silent_period(const struct Presence *p) {
  if (p->silent_start_min < 0 || p->silent_end_min < 0)
    return false;
  if (p->silent_start_min == p->silent_end_min)
    return false; /* empty window */

  time_t now = time(NULL);
  struct tm tm;
  localtime_r(&now, &tm);
  const int m = tm.tm_hour * 60 + tm.tm_min;

  if (p->silent_start_min < p->silent_end_min)
    return m >= p->silent_start_min && m < p->silent_end_min;
  /* window wraps midnight */
  return m >= p->silent_start_min || m < p->silent_end_min;
}

static void evaluate(struct Presence *p) {
  if (!p->stable_known)
    return;
  const bool effective = p->force_off_latched ? false : p->stable_state;
  const int v = effective ? 1 : 0;
  if (p->announced == v)
    return;
  p->announced = v;
  p->cb(p->ud, effective);
}

struct Presence *presence_init(unsigned sensor_stabilization_delay_secs,
                               unsigned hyst_occupied, unsigned hyst_vacant,
                               unsigned silent_hyst_occupied,
                               unsigned silent_hyst_vacant,
                               int silent_start_min, int silent_end_min,
                               presence_change_cb cb, void *ud) {
  if (!cb)
    return NULL;
  struct Presence *p = calloc(1, sizeof(*p));
  if (!p)
    return NULL;
  p->sensor_stabilization_delay_secs = sensor_stabilization_delay_secs;
  p->hyst_occupied = hyst_occupied ? hyst_occupied : 1;
  p->hyst_vacant = hyst_vacant ? hyst_vacant : 1;
  p->silent_hyst_occupied = silent_hyst_occupied ? silent_hyst_occupied : 1;
  p->silent_hyst_vacant = silent_hyst_vacant ? silent_hyst_vacant : 1;
  p->silent_start_min = silent_start_min;
  p->silent_end_min = silent_end_min;
  p->announced = -1;
  p->cb = cb;
  p->ud = ud;
  return p;
}

void presence_free(struct Presence *p) {
  if (!p)
    return;
  free(p);
}

void presence_on_report(struct Presence *p, bool occupied) {
  p->ticks++;

  if (p->pending_count == 0 || p->pending_state != occupied) {
    p->pending_state = occupied;
    p->pending_count = 1;
  } else {
    p->pending_count++;
  }

  if (!p->stable_known) {
    if (p->ticks >= p->sensor_stabilization_delay_secs) {
      p->stable_known = true;
      p->stable_state = occupied;
    }
  } else if (occupied != p->stable_state) {
    const bool silent = in_silent_period(p);
    unsigned thr;
    if (occupied)
      thr = silent ? p->silent_hyst_occupied : p->hyst_occupied;
    else
      thr = silent ? p->silent_hyst_vacant : p->hyst_vacant;
    if (p->pending_count >= thr) {
      const bool was = p->stable_state;
      p->stable_state = occupied;
      // Latch clears only on a genuine occupied → vacant commit.
      if (was && !occupied)
        p->force_off_latched = false;
    }
  }

  evaluate(p);
}

void presence_force_on(struct Presence *p) {
  printf("ForceOn: clearing latch, presence=true\n");
  p->force_off_latched = false;
  p->stable_known = true;
  p->stable_state = true;
  p->pending_state = true;
  p->pending_count = 1;
  evaluate(p);
}

void presence_force_off(struct Presence *p) {
  printf("ForceOff: latching, presence=false until next vacancy\n");
  p->force_off_latched = true;
  p->stable_known = true;
  p->stable_state = false;
  p->pending_state = false;
  p->pending_count = 1;
  evaluate(p);
}

void presence_on_sensor_up(struct Presence *p) {
  printf("Sensor up: applying %us stabilization delay before reporting\n",
         p->sensor_stabilization_delay_secs);
  // Re-arm the stabilization delay: announced/latched state are external
  // concerns and stay as they are; only the hysteresis pipeline restarts
  // so flicker during the sensor's own boot doesn't escape.
  p->ticks = 0;
  p->stable_known = false;
  p->pending_state = false;
  p->pending_count = 0;
}

void presence_on_sensor_down(struct Presence *p) {
  fprintf(stderr, "WARNING: occupancy sensor is down, assuming no presence\n");
  p->force_off_latched = false;
  p->stable_known = true;
  p->stable_state = false;
  p->pending_state = false;
  p->pending_count = 1;
  evaluate(p);
}
