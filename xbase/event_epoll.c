/*
 * Copyright 2025 The xKit Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * event_epoll.c - epoll-based event loop (edge-triggered)
 */

#ifdef XK_HAS_EPOLL

#include "event_base.h"

#include <errno.h>
#include <fcntl.h>
#include <sys/epoll.h>

/* ───────────────────── Helpers ───────────────────── */

static int set_nonblock(int fd) {
  int flags = fcntl(fd, F_GETFL, 0);
  if (flags < 0) return -1;
  return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

static uint32_t mask_to_epoll(xEventMask mask) {
  uint32_t ev = EPOLLET; /* always edge-triggered */
  if (mask & xEvent_Read)  ev |= EPOLLIN;
  if (mask & xEvent_Write) ev |= EPOLLOUT;
  return ev;
}

/* ───────────────────── Epoll-specific loop data ───────────────────── */

struct xEventLoopEpoll_ {
  struct xEventLoop_ base;
  int                epfd;
};

/* ───────────────────── Public API ───────────────────── */

xEventLoop xEventLoopCreate(void) {
  struct xEventLoopEpoll_ *loop =
      (struct xEventLoopEpoll_ *)calloc(1, sizeof(*loop));
  if (!loop) return NULL;

  loop->epfd = -1;
  loop->base.wake_rfd = -1;
  loop->base.wake_wfd = -1;
  loop->base.stopped  = 0;
  loop->base.timer_heap = NULL;
  sources_init(&loop->base.sources);
  loop->base.done_head = NULL;
  loop->base.done_tail = NULL;
  xAtomicStore(&loop->base.inflight, 0, xAtomicRelaxed);

  loop->base.timer_heap = xHeapCreate(event_timer_cmp, event_timer_set_idx, 0);
  if (!loop->base.timer_heap) goto fail;
  if (pthread_mutex_init(&loop->base.timer_mu, NULL) != 0) goto fail;

  loop->epfd = epoll_create1(EPOLL_CLOEXEC);
  if (loop->epfd < 0) goto fail;

  if (loop_init_wake(&loop->base) != 0) goto fail;
  if (set_nonblock(loop->base.wake_rfd) != 0) goto fail;
  if (set_nonblock(loop->base.wake_wfd) != 0) goto fail;

  /* Register wake pipe read end */
  struct epoll_event ev;
  ev.events  = EPOLLIN | EPOLLET;
  ev.data.ptr = NULL; /* sentinel: wake pipe */
  if (epoll_ctl(loop->epfd, EPOLL_CTL_ADD, loop->base.wake_rfd, &ev) != 0)
    goto fail;

  return (xEventLoop)loop;

fail:
  if (loop->epfd >= 0) close(loop->epfd);
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
  struct xEventLoopEpoll_ *loop = (struct xEventLoopEpoll_ *)loop_;
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

  close(loop->epfd);
  loop_close_wake(&loop->base);
  sources_free(&loop->base.sources);
  free(loop);
}

xEventSource xEventAdd(xEventLoop loop_, int fd, xEventMask mask,
                        xEventFunc fn, void *arg) {
  struct xEventLoopEpoll_ *loop = (struct xEventLoopEpoll_ *)loop_;
  if (!loop || !fn) return NULL;

  struct xEventSource_ *src =
      sources_add(&loop->base.sources, fd, mask, fn, arg);
  if (!src) return NULL;

  if (set_nonblock(fd) != 0) {
    sources_remove(&loop->base.sources, src);
    return NULL;
  }

  struct epoll_event ev;
  ev.events   = mask_to_epoll(mask);
  ev.data.ptr = src;
  if (epoll_ctl(loop->epfd, EPOLL_CTL_ADD, fd, &ev) != 0) {
    sources_remove(&loop->base.sources, src);
    return NULL;
  }

  return (xEventSource)src;
}

xErrno xEventMod(xEventLoop loop_, xEventSource src_, xEventMask mask) {
  struct xEventLoopEpoll_ *loop = (struct xEventLoopEpoll_ *)loop_;
  struct xEventSource_ *src     = (struct xEventSource_ *)src_;
  if (!loop || !src) return xErrno_Unknown;

  struct epoll_event ev;
  ev.events   = mask_to_epoll(mask);
  ev.data.ptr = src;
  if (epoll_ctl(loop->epfd, EPOLL_CTL_MOD, src->fd, &ev) != 0)
    return xErrno_Unknown;

  src->mask = mask;
  return xErrno_Ok;
}

xErrno xEventDel(xEventLoop loop_, xEventSource src_) {
  struct xEventLoopEpoll_ *loop = (struct xEventLoopEpoll_ *)loop_;
  struct xEventSource_ *src     = (struct xEventSource_ *)src_;
  if (!loop || !src) return xErrno_Unknown;

  epoll_ctl(loop->epfd, EPOLL_CTL_DEL, src->fd, NULL);
  sources_remove(&loop->base.sources, src);
  return xErrno_Ok;
}

int xEventWait(xEventLoop loop_, int timeout_ms) {
  struct xEventLoopEpoll_ *loop = (struct xEventLoopEpoll_ *)loop_;
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

  struct epoll_event events[64];
  int n = epoll_wait(loop->epfd, events, 64, effective_timeout);
  if (n < 0) n = 0;

  int dispatched = 0;
  for (int i = 0; i < n; i++) {
    struct xEventSource_ *src = (struct xEventSource_ *)events[i].data.ptr;

    /* Wake pipe sentinel */
    if (!src) {
      loop_drain_wake(&loop->base);
      loop_dispatch_done(&loop->base);
      continue;
    }

    xEventMask ready = 0;
    if (events[i].events & EPOLLIN)  ready |= xEvent_Read;
    if (events[i].events & EPOLLOUT) ready |= xEvent_Write;

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
  struct xEventLoopEpoll_ *loop = (struct xEventLoopEpoll_ *)loop_;
  if (!loop) return xErrno_Unknown;

  char c = 1;
  ssize_t r;
  do {
    r = write(loop->base.wake_wfd, &c, 1);
  } while (r < 0 && errno == EINTR);

  return (r == 1 || (r < 0 && errno == EAGAIN)) ? xErrno_Ok : xErrno_Unknown;
}

#endif /* XK_HAS_EPOLL */
