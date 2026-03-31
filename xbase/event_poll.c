/*
 * Copyright 2025 The xKit Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * event_poll.c - poll(2)-based event loop (POSIX fallback)
 *
 * Used when neither kqueue nor epoll is available.
 * poll(2) is level-triggered by nature; we emulate edge-triggered
 * semantics by disabling events after each notification and requiring
 * the caller to re-arm via xEventMod().
 */

#if !defined(XK_HAS_KQUEUE) && !defined(XK_HAS_EPOLL)

#include "event_base.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>

/* ───────────────────── Helpers ───────────────────── */

static int set_nonblock(int fd) {
  int flags = fcntl(fd, F_GETFL, 0);
  if (flags < 0) return -1;
  return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

static short mask_to_poll(xEventMask mask) {
  short ev = 0;
  if (mask & xEvent_Read)  ev |= POLLIN;
  if (mask & xEvent_Write) ev |= POLLOUT;
  return ev;
}

/* ───────────────────── Poll-specific loop data ───────────────────── */

struct xEventLoopPoll_ {
  struct xEventLoop_ base;

  /* Parallel arrays: pollfds[i] corresponds to sources.items[i],
   * except pollfds[0] is always the wake pipe. */
  struct pollfd *pollfds;
  size_t         pfd_len;
  size_t         pfd_cap;
};

static int pfd_grow(struct xEventLoopPoll_ *loop) {
  size_t newcap = loop->pfd_cap ? loop->pfd_cap * 2 : 16;
  struct pollfd *tmp = (struct pollfd *)realloc(
      loop->pollfds, newcap * sizeof(struct pollfd));
  if (!tmp) return -1;
  loop->pollfds = tmp;
  loop->pfd_cap = newcap;
  return 0;
}

/* Rebuild pollfds array from sources list + wake pipe */
static void pfd_rebuild(struct xEventLoopPoll_ *loop) {
  size_t needed = 1 + loop->base.sources.len; /* wake + sources */
  while (loop->pfd_cap < needed)
    pfd_grow(loop);

  /* Slot 0: wake pipe */
  loop->pollfds[0].fd      = loop->base.wake_rfd;
  loop->pollfds[0].events  = POLLIN;
  loop->pollfds[0].revents = 0;

  for (size_t i = 0; i < loop->base.sources.len; i++) {
    struct xEventSource_ *src = loop->base.sources.items[i];
    loop->pollfds[1 + i].fd      = src->fd;
    loop->pollfds[1 + i].events  = mask_to_poll(src->mask);
    loop->pollfds[1 + i].revents = 0;
  }
  loop->pfd_len = needed;
}

/* ───────────────────── Public API ───────────────────── */

xEventLoop xEventLoopCreate(void) {
  struct xEventLoopPoll_ *loop =
      (struct xEventLoopPoll_ *)calloc(1, sizeof(*loop));
  if (!loop) return NULL;

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

  if (loop_init_wake(&loop->base) != 0) goto fail;
  if (set_nonblock(loop->base.wake_rfd) != 0) goto fail;
  if (set_nonblock(loop->base.wake_wfd) != 0) goto fail;

  return (xEventLoop)loop;

fail:
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
  struct xEventLoopPoll_ *loop = (struct xEventLoopPoll_ *)loop_;
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

  loop_close_wake(&loop->base);
  sources_free(&loop->base.sources);
  free(loop->pollfds);
  free(loop);
}

xEventSource xEventAdd(xEventLoop loop_, int fd, xEventMask mask,
                        xEventFunc fn, void *arg) {
  struct xEventLoopPoll_ *loop = (struct xEventLoopPoll_ *)loop_;
  if (!loop || !fn) return NULL;

  struct xEventSource_ *src =
      sources_add(&loop->base.sources, fd, mask, fn, arg);
  if (!src) return NULL;

  if (set_nonblock(fd) != 0) {
    sources_remove(&loop->base.sources, src);
    return NULL;
  }

  return (xEventSource)src;
}

xErrno xEventMod(xEventLoop loop_, xEventSource src_, xEventMask mask) {
  struct xEventLoopPoll_ *loop = (struct xEventLoopPoll_ *)loop_;
  struct xEventSource_ *src    = (struct xEventSource_ *)src_;
  if (!loop || !src) return xErrno_InvalidArg;

  src->mask = mask;
  return xErrno_Ok;
}

xErrno xEventDel(xEventLoop loop_, xEventSource src_) {
  struct xEventLoopPoll_ *loop = (struct xEventLoopPoll_ *)loop_;
  struct xEventSource_ *src    = (struct xEventSource_ *)src_;
  if (!loop || !src) return xErrno_InvalidArg;

  sources_remove(&loop->base.sources, src);
  return xErrno_Ok;
}

int xEventWait(xEventLoop loop_, int timeout_ms) {
  struct xEventLoopPoll_ *loop = (struct xEventLoopPoll_ *)loop_;
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

  pfd_rebuild(loop);

  int n = poll(loop->pollfds, (nfds_t)loop->pfd_len, effective_timeout);
  if (n < 0) n = 0;

  int dispatched = 0;

  /* Check wake pipe (slot 0) */
  if (loop->pollfds[0].revents & POLLIN) {
    loop_drain_wake(&loop->base);
    loop_dispatch_done(&loop->base);
  }

  /* Check sources (slots 1..pfd_len-1) */
  for (size_t i = 0; i < loop->base.sources.len; i++) {
    struct pollfd *pfd = &loop->pollfds[1 + i];
    if (pfd->revents == 0) continue;

    struct xEventSource_ *src = loop->base.sources.items[i];
    xEventMask ready = 0;
    if (pfd->revents & POLLIN)  ready |= xEvent_Read;
    if (pfd->revents & POLLOUT) ready |= xEvent_Write;

    if (ready) {
      src->mask = 0;
      src->fn(src->fd, ready, src->arg);
      dispatched++;
    }
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
  struct xEventLoopPoll_ *loop = (struct xEventLoopPoll_ *)loop_;
  if (!loop) return xErrno_InvalidArg;

  char c = 1;
  ssize_t r;
  do {
    r = write(loop->base.wake_wfd, &c, 1);
  } while (r < 0 && errno == EINTR);

  return (r == 1 || (r < 0 && errno == EAGAIN)) ? xErrno_Ok : xErrno_SysError;
}

#endif /* !XK_HAS_KQUEUE && !XK_HAS_EPOLL */
