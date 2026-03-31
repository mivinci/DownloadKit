/*
 * Copyright 2025 The xKit Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * event_base.h - Shared base types for event loop backends
 */

#ifndef XBASE_EVENT_BASE_H
#define XBASE_EVENT_BASE_H

#include <xbase/event.h>
#include <xbase/heap.h>
#include <xbase/mpsc.h>
#include <xbase/task.h>

#include <fcntl.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* ───────────────────── Source ───────────────────── */

struct xEventSource_ {
  int         fd;
  xEventMask  mask;
  xEventFunc  fn;
  void       *arg;
};

/* ───────────────────── Source list (simple dynamic array) ───────────────── */

struct xEventSources_ {
  struct xEventSource_ **items;
  size_t                 len;
  size_t                 cap;
};

static inline void sources_init(struct xEventSources_ *s) {
  s->items = NULL;
  s->len   = 0;
  s->cap   = 0;
}

static inline void sources_free(struct xEventSources_ *s) {
  for (size_t i = 0; i < s->len; i++)
    free(s->items[i]);
  free(s->items);
  s->items = NULL;
  s->len   = 0;
  s->cap   = 0;
}

static inline struct xEventSource_ *sources_add(struct xEventSources_ *s,
                                                 int fd, xEventMask mask,
                                                 xEventFunc fn, void *arg) {
  if (s->len == s->cap) {
    size_t newcap = s->cap ? s->cap * 2 : 16;
    struct xEventSource_ **tmp = (struct xEventSource_ **)realloc(
        s->items, newcap * sizeof(*s->items));
    if (!tmp) return NULL;
    s->items = tmp;
    s->cap   = newcap;
  }
  struct xEventSource_ *src =
      (struct xEventSource_ *)calloc(1, sizeof(struct xEventSource_));
  if (!src) return NULL;
  src->fd   = fd;
  src->mask = mask;
  src->fn   = fn;
  src->arg  = arg;
  s->items[s->len++] = src;
  return src;
}

static inline int sources_remove(struct xEventSources_ *s,
                                 struct xEventSource_ *src) {
  for (size_t i = 0; i < s->len; i++) {
    if (s->items[i] == src) {
      free(s->items[i]);
      s->items[i] = s->items[--s->len];
      return 0;
    }
  }
  return -1;
}

static inline struct xEventSource_ *sources_find_fd(struct xEventSources_ *s,
                                                     int fd) {
  for (size_t i = 0; i < s->len; i++) {
    if (s->items[i]->fd == fd)
      return s->items[i];
  }
  return NULL;
}

/* ───────────────────── Builtin timer entry ───────────────────── */

#define EVENT_TIMER_INVALID_IDX ((size_t)-1)

struct xEventTimer_ {
  uint64_t         deadline;  /* absolute ms, CLOCK_MONOTONIC */
  xEventTimerFunc  fn;
  void            *arg;
  size_t           heap_idx;  /* position in the min-heap     */
  int              fired;     /* 1 after callback has run     */
};

static inline int event_timer_cmp(const void *a, const void *b) {
  const struct xEventTimer_ *ta = (const struct xEventTimer_ *)a;
  const struct xEventTimer_ *tb = (const struct xEventTimer_ *)b;
  if (ta->deadline < tb->deadline) return -1;
  if (ta->deadline > tb->deadline) return  1;
  return 0;
}

static inline void event_timer_set_idx(void *elem, size_t idx) {
  ((struct xEventTimer_ *)elem)->heap_idx = idx;
}

/* ───────────────────── Offload work item ───────────────────── */

struct xEventWork_ {
  xMpsc             mpsc;     /* intrusive MPSC queue node                */
  xTaskFunc         work_fn;  /* executed on worker thread                */
  void            (*done_fn)(void *arg, void *result); /* executed on loop thread */
  void             *arg;
  void             *result;
  xEventLoop        loop;     /* back-pointer to the owning event loop    */
  xTask             task;     /* handle returned by xTaskSubmit           */
};

/* ───────────────────── Loop base ───────────────────── */

struct xEventLoop_ {
  struct xEventSources_ sources;
  int                   wake_rfd; /* read end of wake pipe  */
  int                   wake_wfd; /* write end of wake pipe */

  /* Offload done queue (lock-free MPSC) */
  xMpsc                *done_head;
  xMpsc                *done_tail;
  atomic_int            inflight; /* number of in-flight offload workers */

  /* Builtin timer heap */
  xHeap                 timer_heap;
  pthread_mutex_t       timer_mu;
  int                   stopped;
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

static inline void loop_drain_wake(struct xEventLoop_ *loop) {
  char buf[64];
  while (read(loop->wake_rfd, buf, sizeof(buf)) > 0)
    ;
}

/* Dispatch all completed offload work items (call done_fn, then free). */
static inline void loop_dispatch_done(struct xEventLoop_ *loop) {
  xMpsc *node;
  while ((node = xMpscPop(&loop->done_head, &loop->done_tail)) != NULL) {
    struct xEventWork_ *w = xContainerOf(node, struct xEventWork_, mpsc);
    /* Release the xTask handle allocated by xTaskSubmit. */
    xTaskWait(w->task, NULL);
    if (w->done_fn)
      w->done_fn(w->arg, w->result);
    atomic_fetch_sub(&loop->inflight, 1);
    free(w);
  }
}

/* Drain remaining offload work items without executing done_fn (for destroy). */
static inline void loop_cleanup_done(struct xEventLoop_ *loop) {
  xMpsc *node;
  while ((node = xMpscPop(&loop->done_head, &loop->done_tail)) != NULL) {
    struct xEventWork_ *w = xContainerOf(node, struct xEventWork_, mpsc);
    xTaskWait(w->task, NULL);
    free(w);
  }
}

/*
 * Spin-wait until all in-flight offload workers have finished and
 * pushed their results into the done queue.  Must be called before
 * loop_cleanup_done() during destroy to avoid use-after-free.
 */
static inline void loop_wait_inflight(struct xEventLoop_ *loop) {
  while (atomic_load(&loop->inflight) > 0) {
    /* Brief yield to let worker threads finish. */
    usleep(100);
  }
}

#endif /* XBASE_EVENT_BASE_H */
