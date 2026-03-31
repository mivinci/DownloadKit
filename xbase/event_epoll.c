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
  sources_init(&loop->base.sources);

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
  free(loop);
  return NULL;
}

void xEventLoopDestroy(xEventLoop loop_) {
  struct xEventLoopEpoll_ *loop = (struct xEventLoopEpoll_ *)loop_;
  if (!loop) return;

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

  struct epoll_event events[64];
  int n = epoll_wait(loop->epfd, events, 64, timeout_ms);
  if (n < 0) return -1;

  int dispatched = 0;
  for (int i = 0; i < n; i++) {
    struct xEventSource_ *src = (struct xEventSource_ *)events[i].data.ptr;

    /* Wake pipe sentinel */
    if (!src) {
      loop_drain_wake(&loop->base);
      continue;
    }

    xEventMask ready = 0;
    if (events[i].events & EPOLLIN)  ready |= xEvent_Read;
    if (events[i].events & EPOLLOUT) ready |= xEvent_Write;

    src->fn(src->fd, ready, src->arg);
    dispatched++;
  }

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
