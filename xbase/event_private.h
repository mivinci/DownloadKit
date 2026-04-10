/*
 * Copyright 2025 The xKit Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * event_private.h - Shared base types for event loop backends
 */

#ifndef XBASE_EVENT_PRIVATE_H
#define XBASE_EVENT_PRIVATE_H

#include <xbase/atomic.h>
#include <xbase/event.h>
#include <xbase/heap.h>
#include <xbase/mpsc.h>
#include <xbase/task.h>

#include <fcntl.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* ───────────────────── Signal watch ───────────────────── */

#ifndef XK_SIGNAL_MAX
#define XK_SIGNAL_MAX 64
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

static inline void source_array_free(struct xEventSourceArray_ *s) {
  for (size_t i = 0; i < s->len; i++)
    free(s->items[i]);
  free(s->items);
  s->items = NULL;
  s->len   = 0;
  s->cap   = 0;
}

static inline struct xEventSource_ *
source_array_add(struct xEventSourceArray_ *s, int fd, xEventMask mask,
                 xEventFunc fn, void *arg) {
  if (s->len == s->cap) {
    size_t                 newcap = s->cap ? s->cap * 2 : 16;
    struct xEventSource_ **tmp =
      (struct xEventSource_ **)realloc(s->items, newcap * sizeof(*s->items));
    if (!tmp) return NULL;
    s->items = tmp;
    s->cap   = newcap;
  }
  struct xEventSource_ *src =
    (struct xEventSource_ *)calloc(1, sizeof(struct xEventSource_));
  if (!src) return NULL;
  src->fd            = fd;
  src->mask          = mask;
  src->fn            = fn;
  src->arg           = arg;
  s->items[s->len++] = src;
  return src;
}

static inline int source_array_remove(struct xEventSourceArray_ *s,
                                      struct xEventSource_      *src) {
  (void)s;
  /* Mark for deferred removal — the source may still be referenced
   * by pending events in the current dispatch batch. */
  src->deleted = 1;
  return 0;
}

/**
 * @brief Sweep deleted sources after a dispatch batch completes.
 *
 * Must be called at the end of each xEventWait() to actually free
 * sources that were removed during callback dispatch.
 */
static inline void source_array_sweep(struct xEventSourceArray_ *s) {
  size_t i = 0;
  while (i < s->len) {
    if (s->items[i]->deleted) {
      free(s->items[i]);
      s->items[i] = s->items[--s->len];
    } else {
      i++;
    }
  }
}

static inline struct xEventSource_ *
source_array_find_fd(struct xEventSourceArray_ *s, int fd) {
  for (size_t i = 0; i < s->len; i++) {
    if (s->items[i]->fd == fd) return s->items[i];
  }
  return NULL;
}

/* ───────────────────── Builtin timer entry ───────────────────── */

#define EVENT_TIMER_INVALID_IDX ((size_t) - 1)

struct xEventTimer_ {
  uint64_t        deadline; /* absolute ms, CLOCK_MONOTONIC */
  xEventTimerFunc fn;
  void           *arg;
  size_t          heap_idx; /* position in the min-heap     */
  int             fired;    /* 1 after callback has run     */
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
  void (*done_fn)(void *arg, void *result); /* executed on loop thread */
  void      *arg;
  void      *result;
  xEventLoop loop;      /* back-pointer to the owning event loop    */
  xTask      task;      /* handle returned by xTaskSubmit           */
  struct xEventWork_ *next_free; /* freelist link (when on the pool) */
};

/* ───────────────────── Loop base ───────────────────── */

struct xEventLoop_ {
  struct xEventSourceArray_ sources;
  int                       wake_rfd; /* read end of wake pipe  */
  int                       wake_wfd; /* write end of wake pipe */

  /* Offload done queue (lock-free MPSC) */
  xMpsc *done_head;
  xMpsc *done_tail;
  int    inflight; /* number of in-flight offload workers */

  /* Builtin timer heap */
  xHeap           timer_heap;
  pthread_mutex_t timer_mu;
  int             stopped;

  /* Timer struct freelist (protected by timer_mu) */
  struct xEventTimer_ *timer_free;  /* singly-linked freelist head */
  size_t               timer_nfree; /* current freelist length     */

#define EVENT_TIMER_POOL_MAX 256 /* max cached timer structs    */

  /* Default task group for offload (may be NULL) */
  xTaskGroup task_group;

  /* Lock-free freelist for xEventWork_ (Treiber stack) */
  struct xEventWork_ *work_freelist;

  /* Signal watches (indexed by signal number) */
  struct xSignalWatch_ signal_watches[XK_SIGNAL_MAX];
};

static inline int loop_init_wake(struct xEventLoop_ *loop) {
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
}

static inline void loop_close_wake(struct xEventLoop_ *loop) {
  if (loop->wake_rfd >= 0) close(loop->wake_rfd);
  if (loop->wake_wfd >= 0) close(loop->wake_wfd);
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
static inline void event_timer_free(struct xEventLoop_ *loop,
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

/*
 * Pop all expired timers under a single lock acquisition, then fire
 * them and return each struct to the pool.  Replaces the per-pop
 * lock/unlock pattern in all three backends.
 */
static inline int loop_fire_expired_timers(struct xEventLoop_ *loop) {
  /* Scratch buffer on stack; fall back to heap for huge batches. */
  struct xEventTimer_ *stack_buf[128];
  struct xEventTimer_ **batch = stack_buf;
  size_t batch_cap = 128;
  size_t batch_len = 0;

  pthread_mutex_lock(&loop->timer_mu);
  uint64_t now = xMonoMs();
  while (xHeapSize(loop->timer_heap) > 0) {
    struct xEventTimer_ *t =
      (struct xEventTimer_ *)xHeapPeek(loop->timer_heap);
    if (t->deadline > now) break;
    xHeapPop(loop->timer_heap);
    t->fired = 1;
    /* Grow batch if needed */
    if (batch_len == batch_cap) {
      size_t newcap = batch_cap * 2;
      struct xEventTimer_ **heap_buf;
      if (batch == stack_buf) {
        heap_buf = (struct xEventTimer_ **)malloc(
          newcap * sizeof(struct xEventTimer_ *));
        if (heap_buf) memcpy(heap_buf, stack_buf, batch_len * sizeof(*batch));
      } else {
        heap_buf = (struct xEventTimer_ **)realloc(
          batch, newcap * sizeof(struct xEventTimer_ *));
      }
      if (!heap_buf) {
        /* Out of memory — fire what we have so far, recycle, retry later */
        break;
      }
      batch     = heap_buf;
      batch_cap = newcap;
    }
    batch[batch_len++] = t;
  }
  pthread_mutex_unlock(&loop->timer_mu);

  /* Fire callbacks outside the lock */
  for (size_t i = 0; i < batch_len; i++) {
    batch[i]->fn(batch[i]->arg);
  }

  /* Return structs to the pool (re-acquire lock once) */
  if (batch_len > 0) {
    pthread_mutex_lock(&loop->timer_mu);
    for (size_t i = 0; i < batch_len; i++) {
      event_timer_free(loop, batch[i]);
    }
    pthread_mutex_unlock(&loop->timer_mu);
  }

  if (batch != stack_buf) free(batch);
  return (int)batch_len;
}

/* ───────────────────── Wake helpers ───────────────────── */

static inline void loop_drain_wake(struct xEventLoop_ *loop) {
  char buf[64];
  while (read(loop->wake_rfd, buf, sizeof(buf)) > 0)
    ;
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
    if (xAtomicCasWeak(&loop->work_freelist, &w, w->next_free, xAtomicAcqRel))
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
    !xAtomicCasWeak(&loop->work_freelist, &head, w, xAtomicRelease));
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

/* Dispatch all completed offload work items (call done_fn, then recycle). */
static inline void loop_dispatch_done(struct xEventLoop_ *loop) {
  xMpsc *node;
  while ((node = xMpscPop(&loop->done_head, &loop->done_tail)) != NULL) {
    struct xEventWork_ *w = xContainerOf(node, struct xEventWork_, mpsc);
    /* Release the xTask handle allocated by xTaskSubmit. */
    xTaskWait(w->task, NULL);
    if (w->done_fn) w->done_fn(w->arg, w->result);
    xAtomicFetchSub(&loop->inflight, 1, xAtomicRelaxed);
    event_work_free(loop, w);
  }
}

/* Drain remaining offload work items without executing done_fn (for destroy).
 */
static inline void loop_cleanup_done(struct xEventLoop_ *loop) {
  xMpsc *node;
  while ((node = xMpscPop(&loop->done_head, &loop->done_tail)) != NULL) {
    struct xEventWork_ *w = xContainerOf(node, struct xEventWork_, mpsc);
    xTaskWait(w->task, NULL);
    free(w); /* truly free — loop is being destroyed */
  }
}

/*
 * Spin-wait until all in-flight offload workers have finished and
 * pushed their results into the done queue.  Must be called before
 * loop_cleanup_done() during destroy to avoid use-after-free.
 */
static inline void loop_wait_inflight(struct xEventLoop_ *loop) {
  while (xAtomicLoad(&loop->inflight, xAtomicAcquire) > 0) {
    /* Brief yield to let worker threads finish. */
    usleep(100);
  }
}

#endif /* XBASE_EVENT_PRIVATE_H */
