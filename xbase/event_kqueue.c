/*
 * Copyright 2025 The xKit Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * event_kqueue.c - kqueue-based event loop (edge-triggered)
 */

#ifdef XK_HAS_KQUEUE

#include "event_base.h"

#include <errno.h>
#include <fcntl.h>
#include <sys/event.h>
#include <sys/types.h>

/* ───────────────────── Helpers ───────────────────── */

static int set_nonblock(int fd) {
  int flags = fcntl(fd, F_GETFL, 0);
  if (flags < 0) return -1;
  return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

/*
 * Register or update filters for a source on the kqueue fd.
 * Uses EV_CLEAR for edge-triggered semantics.
 */
static int kq_apply(int kqfd, struct xEventSource_ *src, xEventMask mask) {
  struct kevent changes[2];
  int n = 0;

  if (mask & xEvent_Read) {
    EV_SET(&changes[n], src->fd, EVFILT_READ,
           EV_ADD | EV_CLEAR, 0, 0, src);
    n++;
  } else {
    /* Remove read filter if it was previously set */
    EV_SET(&changes[n], src->fd, EVFILT_READ, EV_DELETE, 0, 0, NULL);
    n++;
  }

  if (mask & xEvent_Write) {
    EV_SET(&changes[n], src->fd, EVFILT_WRITE,
           EV_ADD | EV_CLEAR, 0, 0, src);
    n++;
  } else {
    EV_SET(&changes[n], src->fd, EVFILT_WRITE, EV_DELETE, 0, 0, NULL);
    n++;
  }

  /* Ignore ENOENT errors from deleting filters that don't exist */
  for (int i = 0; i < n; i++) {
    if (kevent(kqfd, &changes[i], 1, NULL, 0, NULL) < 0 &&
        errno != ENOENT)
      return -1;
  }
  return 0;
}

/* ───────────────────── Kqueue-specific loop data ───────────────────── */

struct xEventLoopKqueue_ {
  struct xEventLoop_ base;
  int                kqfd;
};

/* ───────────────────── Public API ───────────────────── */

xEventLoop xEventLoopCreate(void) {
  struct xEventLoopKqueue_ *loop =
      (struct xEventLoopKqueue_ *)calloc(1, sizeof(*loop));
  if (!loop) return NULL;

  loop->kqfd = -1;
  loop->base.wake_rfd = -1;
  loop->base.wake_wfd = -1;
  loop->base.stopped  = 0;
  loop->base.timer_heap = NULL;
  sources_init(&loop->base.sources);
  loop->base.done_head = NULL;
  loop->base.done_tail = NULL;
  atomic_init(&loop->base.inflight, 0);

  loop->base.timer_heap = xHeapCreate(event_timer_cmp, event_timer_set_idx, 0);
  if (!loop->base.timer_heap) goto fail;
  if (pthread_mutex_init(&loop->base.timer_mu, NULL) != 0) goto fail;

  loop->kqfd = kqueue();
  if (loop->kqfd < 0) goto fail;

  if (loop_init_wake(&loop->base) != 0) goto fail;
  if (set_nonblock(loop->base.wake_rfd) != 0) goto fail;
  if (set_nonblock(loop->base.wake_wfd) != 0) goto fail;

  /* Register wake pipe read end with kqueue (edge-triggered) */
  struct kevent ev;
  EV_SET(&ev, loop->base.wake_rfd, EVFILT_READ,
         EV_ADD | EV_CLEAR, 0, 0, NULL);
  if (kevent(loop->kqfd, &ev, 1, NULL, 0, NULL) < 0) goto fail;

  return (xEventLoop)loop;

fail:
  if (loop->kqfd >= 0) close(loop->kqfd);
  loop_close_wake(&loop->base);
  sources_free(&loop->base.sources);
  if (loop->base.timer_heap) {
    xHeapDestroy(loop->base.timer_heap);
    pthread_mutex_destroy(&loop->base.timer_mu);
  }
  free(loop);
  return NULL;
}

void xEventLoopDestroy(xEventLoop loop_) {
  struct xEventLoopKqueue_ *loop = (struct xEventLoopKqueue_ *)loop_;
  if (!loop) return;

  /* Discard all pending timers without firing */
  pthread_mutex_lock(&loop->base.timer_mu);
  while (xHeapSize(loop->base.timer_heap) > 0) {
    struct xEventTimer_ *t = (struct xEventTimer_ *)xHeapPop(loop->base.timer_heap);
    free(t);
  }
  pthread_mutex_unlock(&loop->base.timer_mu);
  xHeapDestroy(loop->base.timer_heap);
  pthread_mutex_destroy(&loop->base.timer_mu);

  loop_wait_inflight(&loop->base);
  loop_cleanup_done(&loop->base);

  close(loop->kqfd);
  loop_close_wake(&loop->base);
  sources_free(&loop->base.sources);
  free(loop);
}

xEventSource xEventAdd(xEventLoop loop_, int fd, xEventMask mask,
                        xEventFunc fn, void *arg) {
  struct xEventLoopKqueue_ *loop = (struct xEventLoopKqueue_ *)loop_;
  if (!loop || !fn) return NULL;

  struct xEventSource_ *src =
      sources_add(&loop->base.sources, fd, mask, fn, arg);
  if (!src) return NULL;

  if (set_nonblock(fd) != 0 || kq_apply(loop->kqfd, src, mask) != 0) {
    sources_remove(&loop->base.sources, src);
    return NULL;
  }

  return (xEventSource)src;
}

xErrno xEventMod(xEventLoop loop_, xEventSource src_, xEventMask mask) {
  struct xEventLoopKqueue_ *loop = (struct xEventLoopKqueue_ *)loop_;
  struct xEventSource_ *src      = (struct xEventSource_ *)src_;
  if (!loop || !src) return xErrno_Unknown;

  if (kq_apply(loop->kqfd, src, mask) != 0)
    return xErrno_Unknown;

  src->mask = mask;
  return xErrno_Ok;
}

xErrno xEventDel(xEventLoop loop_, xEventSource src_) {
  struct xEventLoopKqueue_ *loop = (struct xEventLoopKqueue_ *)loop_;
  struct xEventSource_ *src      = (struct xEventSource_ *)src_;
  if (!loop || !src) return xErrno_Unknown;

  /* Remove all filters */
  struct kevent changes[2];
  EV_SET(&changes[0], src->fd, EVFILT_READ, EV_DELETE, 0, 0, NULL);
  EV_SET(&changes[1], src->fd, EVFILT_WRITE, EV_DELETE, 0, 0, NULL);
  /* Ignore ENOENT */
  kevent(loop->kqfd, &changes[0], 1, NULL, 0, NULL);
  kevent(loop->kqfd, &changes[1], 1, NULL, 0, NULL);

  sources_remove(&loop->base.sources, src);
  return xErrno_Ok;
}

int xEventWait(xEventLoop loop_, int timeout_ms) {
  struct xEventLoopKqueue_ *loop = (struct xEventLoopKqueue_ *)loop_;
  if (!loop) return -1;

  /* Adjust timeout based on timer heap */
  int effective_timeout = timeout_ms;
  pthread_mutex_lock(&loop->base.timer_mu);
  struct xEventTimer_ *top = (struct xEventTimer_ *)xHeapPeek(loop->base.timer_heap);
  if (top) {
    uint64_t now = xEventLoopNowMs();
    int64_t wait = (int64_t)(top->deadline - now);
    int timer_timeout = (wait <= 0) ? 0 : (int)wait;
    if (effective_timeout < 0 || timer_timeout < effective_timeout)
      effective_timeout = timer_timeout;
  }
  pthread_mutex_unlock(&loop->base.timer_mu);

  struct kevent events[64];
  struct timespec ts;
  struct timespec *tsp = NULL;

  if (effective_timeout >= 0) {
    ts.tv_sec  = effective_timeout / 1000;
    ts.tv_nsec = (effective_timeout % 1000) * 1000000L;
    tsp = &ts;
  }

  int n = kevent(loop->kqfd, NULL, 0, events, 64, tsp);
  if (n < 0) n = 0; /* treat EINTR as no events */

  int dispatched = 0;
  for (int i = 0; i < n; i++) {
    /* Skip wake pipe */
    if ((int)events[i].ident == loop->base.wake_rfd) {
      loop_drain_wake(&loop->base);
      loop_dispatch_done(&loop->base);
      continue;
    }

    struct xEventSource_ *src = (struct xEventSource_ *)events[i].udata;
    if (!src) continue;

    xEventMask ready = 0;
    if (events[i].filter == EVFILT_READ)  ready |= xEvent_Read;
    if (events[i].filter == EVFILT_WRITE) ready |= xEvent_Write;

    src->fn(src->fd, ready, src->arg);
    dispatched++;
  }

  /* Fire expired timers */
  pthread_mutex_lock(&loop->base.timer_mu);
  uint64_t now = xEventLoopNowMs();
  while (xHeapSize(loop->base.timer_heap) > 0) {
    struct xEventTimer_ *t = (struct xEventTimer_ *)xHeapPeek(loop->base.timer_heap);
    if (t->deadline > now) break;
    xHeapPop(loop->base.timer_heap);
    t->fired = 1;
    pthread_mutex_unlock(&loop->base.timer_mu);
    t->fn(t->arg);
    free(t);
    pthread_mutex_lock(&loop->base.timer_mu);
  }
  pthread_mutex_unlock(&loop->base.timer_mu);

  return dispatched;
}

xErrno xEventWake(xEventLoop loop_) {
  struct xEventLoopKqueue_ *loop = (struct xEventLoopKqueue_ *)loop_;
  if (!loop) return xErrno_Unknown;

  char c = 1;
  ssize_t r;
  do {
    r = write(loop->base.wake_wfd, &c, 1);
  } while (r < 0 && errno == EINTR);

  return (r == 1 || (r < 0 && errno == EAGAIN)) ? xErrno_Ok : xErrno_Unknown;
}

#endif /* XK_HAS_KQUEUE */
