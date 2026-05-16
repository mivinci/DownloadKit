/*
 * Copyright 2025 The moo Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * event_private.h - Shared base types for event loop backends
 */

#ifndef XBASE_EVENT_PRIVATE_H
#define XBASE_EVENT_PRIVATE_H

#include <x/base/atomic.h>
#include <x/base/event.h>
#include <x/base/heap.h>
#include <x/base/mpsc.h>
#include <x/base/task.h>

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "thread_private.h"

#ifndef _WIN32
#include <fcntl.h>
#include <unistd.h>
#endif

/* ───────────────────── Signal watch ───────────────────── */

#ifndef MOO_SIGNAL_MAX
#define MOO_SIGNAL_MAX 64
#endif

struct xSignalWatch_ {
  xEventSignalFunc fn;
  void            *arg;
};

/* ───────────────────── Source ───────────────────── */

struct xEventSource_ {
  int        fd;
  xEventMask mask;
  xEventFunc fn;
  void      *arg;
  int        deleted; /* marked for deferred removal */
};

/* ───────────────────── Source list (simple dynamic array) ───────────────── */

struct xEventSourceArray_ {
  struct xEventSource_ **items;
  size_t                 len;
  size_t                 cap;
};

static inline void source_array_init(struct xEventSourceArray_ *s) {
  s->items = NULL;
  s->len   = 0;
  s->cap   = 0;
}

static inline int source_array_remove(struct xEventSourceArray_ *s,
                                      struct xEventSource_      *src) {
  (void)s;
  /* Mark for deferred removal — the source may still be referenced
   * by pending events in the current dispatch batch. */
  src->deleted = 1;
  return 0;
}

/* Non-inline implementations — see event_private.c */
void source_array_free(struct xEventSourceArray_ *s);
struct xEventSource_ *source_array_add(struct xEventSourceArray_ *s, int fd,
                                       xEventMask mask, xEventFunc fn,
                                       void *arg);
void source_array_sweep(struct xEventSourceArray_ *s);

static inline struct xEventSource_ *
source_array_find_fd(struct xEventSourceArray_ *s, int fd) {
  for (size_t i = 0; i < s->len; i++) {
    if (s->items[i]->fd == fd) return s->items[i];
  }
  return NULL;
}

/* ───────────────────── Builtin timer entry ───────────────────── */

#define EVENT_TIMER_INVALID_IDX ((size_t)-1)

struct xEventTimer_ {
  uint64_t             deadline; /* absolute ms, CLOCK_MONOTONIC */
  xEventTimerFunc      fn;
  void                *arg;
  size_t               heap_idx;  /* position in the min-heap     */
  int                  fired;     /* 1 after callback has run     */
  struct xEventTimer_ *next_free; /* freelist link (valid only when pooled) */
};

static inline int event_timer_cmp(const void *a, const void *b) {
  const struct xEventTimer_ *ta = (const struct xEventTimer_ *)a;
  const struct xEventTimer_ *tb = (const struct xEventTimer_ *)b;
  if (ta->deadline < tb->deadline) return -1;
  if (ta->deadline > tb->deadline) return 1;
  return 0;
}

static inline void event_timer_set_idx(void *elem, size_t idx) {
  ((struct xEventTimer_ *)elem)->heap_idx = idx;
}

/* ───────────────────── Offload work item ───────────────────── */

struct xEventWork_ {
  xMpsc     mpsc;    /* intrusive MPSC queue node                */
  xTaskFunc work_fn; /* executed on worker thread                */
  union {
    void (*done_fn)(void *arg, void *result); /* offload completion   */
    xEventPostFunc post_fn;                   /* posted callback      */
  };
  void               *arg;
  void               *result;
  xEventLoop          loop;      /* back-pointer to the owning event loop    */
  xTask               task;      /* handle returned by xTaskSubmit           */
  struct xEventWork_ *next_free; /* freelist link (when on the pool) */
};

/* ───────────────────── Loop base ───────────────────── */

struct xEventLoop_ {
  struct xEventSourceArray_ sources;
#ifdef _WIN32
  HANDLE wake_event; /* manual-reset event for loop wakeup */
#else
  int    wake_rfd;   /* read end of wake pipe  */
  int    wake_wfd;   /* write end of wake pipe */
#endif

  /* Offload done queue (lock-free MPSC) */
  xMpsc *done_head;
  xMpsc *done_tail;
  int    inflight; /* number of in-flight offload workers */

  /* Builtin timer heap */
  xHeap  timer_heap;
  xMutex timer_mu;
  int    stopped;

  /* Timer struct freelist (protected by timer_mu) */
  struct xEventTimer_ *timer_free;  /* singly-linked freelist head */
  size_t               timer_nfree; /* current freelist length     */

#define EVENT_TIMER_POOL_MAX 256 /* max cached timer structs    */

  /* Default task group for offload (may be NULL) */
  xTaskGroup task_group;

  /* Lock-free freelist for xEventWork_ (Treiber stack) */
  struct xEventWork_ *work_freelist;

  /* Wake coalescing: only the first writer performs the syscall */
  int wake_pending;

  /* Signal watches (indexed by signal number) */
  struct xSignalWatch_ signal_watches[MOO_SIGNAL_MAX];
};

static inline int loop_init_wake(struct xEventLoop_ *loop) {
#ifdef _WIN32
  loop->wake_event = CreateEventW(NULL, TRUE, FALSE, NULL);
  return loop->wake_event ? 0 : -1;
#else
  int fds[2];
  if (pipe(fds) != 0) return -1;
  /* Set read end to non-blocking so loop_drain_wake never blocks. */
  if (fcntl(fds[0], F_SETFL, fcntl(fds[0], F_GETFL, 0) | O_NONBLOCK) < 0) {
    close(fds[0]);
    close(fds[1]);
    return -1;
  }
  loop->wake_rfd = fds[0];
  loop->wake_wfd = fds[1];
  return 0;
#endif
}

static inline void loop_close_wake(struct xEventLoop_ *loop) {
#ifdef _WIN32
  if (loop->wake_event) CloseHandle(loop->wake_event);
#else
  if (loop->wake_rfd >= 0) close(loop->wake_rfd);
  if (loop->wake_wfd >= 0) close(loop->wake_wfd);
#endif
}

/* ───────────────────── Timer pool helpers ───────────────────── */

/*
 * Allocate a timer struct, preferring the freelist over malloc.
 * Must be called with timer_mu held.
 */
static inline struct xEventTimer_ *event_timer_alloc(struct xEventLoop_ *loop) {
  struct xEventTimer_ *t = loop->timer_free;
  if (t) {
    loop->timer_free = t->next_free;
    loop->timer_nfree--;
    memset(t, 0, sizeof(*t));
    return t;
  }
  return (struct xEventTimer_ *)calloc(1, sizeof(struct xEventTimer_));
}

/*
 * Return a timer struct to the freelist (or free it if pool is full).
 * Must be called with timer_mu held.
 */
static inline void event_timer_free(struct xEventLoop_  *loop,
                                    struct xEventTimer_ *t) {
  if (loop->timer_nfree < EVENT_TIMER_POOL_MAX) {
    t->next_free     = loop->timer_free;
    loop->timer_free = t;
    loop->timer_nfree++;
  } else {
    free(t);
  }
}

/* Drain the freelist (called during destroy, no lock needed). */
static inline void event_timer_pool_destroy(struct xEventLoop_ *loop) {
  struct xEventTimer_ *t = loop->timer_free;
  while (t) {
    struct xEventTimer_ *next = t->next_free;
    free(t);
    t = next;
  }
  loop->timer_free  = NULL;
  loop->timer_nfree = 0;
}

/* Non-inline implementations — see event_private.c */
int  loop_fire_expired_timers(struct xEventLoop_ *loop);
void loop_dispatch_done(struct xEventLoop_ *loop);
void loop_cleanup_done(struct xEventLoop_ *loop);
void loop_wait_inflight(struct xEventLoop_ *loop);

/* ───────────────────── Wake helpers ───────────────────── */

static inline void loop_drain_wake(struct xEventLoop_ *loop) {
#ifdef _WIN32
  ResetEvent(loop->wake_event);
#else
  char buf[64];
  while (read(loop->wake_rfd, buf, sizeof(buf)) > 0)
    ;
#endif
}

/*
 * Wake coalescing: use an atomic flag so that only the first caller
 * after the loop clears the flag actually performs the wake syscall.
 * Returns 1 if the caller should proceed with the real wake, 0 to skip.
 */
static inline int loop_coalesced_wake(struct xEventLoop_ *loop) {
  return xAtomicXchg(&loop->wake_pending, 1, xAtomicAcqRel) == 0;
}

/*
 * Clear the wake-pending flag.  Must be called on the loop thread
 * *before* draining the done queue to avoid a lost-wake race.
 */
static inline void loop_clear_wake_pending(struct xEventLoop_ *loop) {
  xAtomicStore(&loop->wake_pending, 0, xAtomicRelease);
}

/*
 * Lock-free Treiber stack for xEventWork_ recycling.
 *
 * Both alloc and free may be called from any thread:
 *   - alloc: from the submitting thread (xEventLoopSubmit)
 *   - free:  from the event-loop thread  (loop_dispatch_done)
 */
static inline struct xEventWork_ *
event_work_alloc(struct xEventLoop_ *loop) {
  struct xEventWork_ *w;
  for (;;) {
    w = xAtomicLoad(&loop->work_freelist, xAtomicAcquire);
    if (!w) break; /* empty — fall back to calloc */
    if (xAtomicCasPtrWeak(&loop->work_freelist, &w, w->next_free, xAtomicAcqRel))
      break;
  }
  if (w) {
    memset(w, 0, sizeof(*w));
    return w;
  }
  return (struct xEventWork_ *)calloc(1, sizeof(struct xEventWork_));
}

static inline void event_work_free(struct xEventLoop_ *loop,
                                   struct xEventWork_ *w) {
  struct xEventWork_ *head;
  w->next_free = NULL;
  do {
    head         = xAtomicLoad(&loop->work_freelist, xAtomicRelaxed);
    w->next_free = head;
  } while (
    !xAtomicCasPtrWeak(&loop->work_freelist, &head, w, xAtomicRelease));
}

static inline void event_work_pool_destroy(struct xEventLoop_ *loop) {
  struct xEventWork_ *w = loop->work_freelist;
  while (w) {
    struct xEventWork_ *next = w->next_free;
    free(w);
    w = next;
  }
  loop->work_freelist = NULL;
}

#endif /* XBASE_EVENT_PRIVATE_H */
