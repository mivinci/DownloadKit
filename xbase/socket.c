/*
 * Copyright 2025 The xKit Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * socket.c - Async socket abstraction over xEventLoop
 */

#include <xbase/socket.h>

#include <fcntl.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <unistd.h>

/* ───────────────────── Internal structure ───────────────────── */

struct xSocket_ {
  int              fd;
  xEventLoop       loop;
  xEventSource     source;
  xEventMask       mask;
  xSocketFunc      callback;
  void            *userp;
  xEventTimer      read_timer;
  xEventTimer      write_timer;
  int              read_timeout_ms;
  int              write_timeout_ms;
};

/* ───────────────────── Forward declarations ───────────────────── */

static void trampoline(int fd, xEventMask mask, void *arg);
static void read_timeout_cb(void *arg);
static void write_timeout_cb(void *arg);
static void reset_read_timer(struct xSocket_ *s);
static void reset_write_timer(struct xSocket_ *s);
static void cancel_read_timer(struct xSocket_ *s);
static void cancel_write_timer(struct xSocket_ *s);

/* ───────────────────── Trampoline ───────────────────── */

static void trampoline(int fd, xEventMask mask, void *arg) {
  struct xSocket_ *s = (struct xSocket_ *)arg;
  (void)fd;

  /* Reset idle timers on normal I/O events */
  if (mask & xEvent_Read)
    reset_read_timer(s);
  if (mask & xEvent_Write)
    reset_write_timer(s);

  s->callback((xSocket)s, mask, s->userp);
}

/* ───────────────────── Timeout callbacks ───────────────────── */

static void read_timeout_cb(void *arg) {
  struct xSocket_ *s = (struct xSocket_ *)arg;
  s->read_timer = NULL;
  /* Or xEvent_Read so user knows which direction timed out */
  s->callback((xSocket)s, xEvent_Timeout | xEvent_Read, s->userp);
}

static void write_timeout_cb(void *arg) {
  struct xSocket_ *s = (struct xSocket_ *)arg;
  s->write_timer = NULL;
  /* Or xEvent_Write so user knows which direction timed out */
  s->callback((xSocket)s, xEvent_Timeout | xEvent_Write, s->userp);
}

/* ───────────────────── Timer helpers ───────────────────── */

static void cancel_read_timer(struct xSocket_ *s) {
  if (s->read_timer) {
    xEventLoopTimerCancel(s->loop, s->read_timer);
    s->read_timer = NULL;
  }
}

static void cancel_write_timer(struct xSocket_ *s) {
  if (s->write_timer) {
    xEventLoopTimerCancel(s->loop, s->write_timer);
    s->write_timer = NULL;
  }
}

static void reset_read_timer(struct xSocket_ *s) {
  if (s->read_timeout_ms <= 0) return;
  cancel_read_timer(s);
  s->read_timer = xEventLoopTimerAfter(s->loop, read_timeout_cb, s,
                                        (uint64_t)s->read_timeout_ms);
}

static void reset_write_timer(struct xSocket_ *s) {
  if (s->write_timeout_ms <= 0) return;
  cancel_write_timer(s);
  s->write_timer = xEventLoopTimerAfter(s->loop, write_timeout_cb, s,
                                         (uint64_t)s->write_timeout_ms);
}

/* ───────────────────── Lifecycle ───────────────────── */

xSocket xSocketCreate(xEventLoop loop,
                       int family, int type, int protocol,
                       xEventMask mask,
                       xSocketFunc callback, void *userp) {
  if (!loop || !callback) return NULL;

  struct xSocket_ *s =
      (struct xSocket_ *)calloc(1, sizeof(struct xSocket_));
  if (!s) return NULL;

  int fd = -1;

#ifdef SOCK_CLOEXEC
  /* Linux/BSD: use socket() with SOCK_CLOEXEC | SOCK_NONBLOCK if available */
  int sock_type = type;
#ifdef SOCK_NONBLOCK
  sock_type |= SOCK_NONBLOCK;
#endif
  sock_type |= SOCK_CLOEXEC;
  fd = socket(family, sock_type, protocol);
  if (fd < 0) {
    free(s);
    return NULL;
  }
#ifndef SOCK_NONBLOCK
  /* SOCK_NONBLOCK not available, need fcntl */
  int flags = fcntl(fd, F_GETFL, 0);
  if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
    close(fd);
    free(s);
    return NULL;
  }
#endif
#else
  /* Fallback: separate socket() + fcntl() calls */
  fd = socket(family, type, protocol);
  if (fd < 0) {
    free(s);
    return NULL;
  }

  /* Set non-blocking */
  int flags = fcntl(fd, F_GETFL, 0);
  if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
    goto fail;
  }

  /* Set close-on-exec */
  int fdflags = fcntl(fd, F_GETFD, 0);
  if (fdflags < 0 || fcntl(fd, F_SETFD, fdflags | FD_CLOEXEC) < 0) {
    goto fail;
  }
#endif

  s->fd               = fd;
  s->loop             = loop;
  s->mask             = mask;
  s->callback         = callback;
  s->userp            = userp;
  s->read_timer       = NULL;
  s->write_timer      = NULL;
  s->read_timeout_ms  = 0;
  s->write_timeout_ms = 0;

  xEventSource src = xEventAdd(loop, fd, mask, trampoline, s);
  if (!src) {
#ifdef SOCK_CLOEXEC
    close(fd);
    free(s);
    return NULL;
#else
    goto fail;
#endif
  }
  s->source = src;

  return (xSocket)s;

#ifndef SOCK_CLOEXEC
fail:
  close(fd);
  free(s);
  return NULL;
#endif
}

void xSocketDestroy(xEventLoop loop, xSocket sock) {
  if (!sock) return;
  struct xSocket_ *s = (struct xSocket_ *)sock;

  cancel_read_timer(s);
  cancel_write_timer(s);

  xEventDel(loop, s->source);
  close(s->fd);
  free(s);
}

/* ───────────────────── Event mask ───────────────────── */

xErrno xSocketSetMask(xEventLoop loop, xSocket sock, xEventMask mask) {
  if (!sock) return xErrno_InvalidArg;
  struct xSocket_ *s = (struct xSocket_ *)sock;

  xErrno err = xEventMod(loop, s->source, mask);
  if (err == xErrno_Ok)
    s->mask = mask;
  return err;
}

/* ───────────────────── Timeout ───────────────────── */

xErrno xSocketSetTimeout(xSocket sock,
                          int read_timeout_ms, int write_timeout_ms) {
  if (!sock) return xErrno_InvalidArg;
  struct xSocket_ *s = (struct xSocket_ *)sock;

  /* Read timeout */
  s->read_timeout_ms = read_timeout_ms;
  if (read_timeout_ms > 0) {
    cancel_read_timer(s);
    s->read_timer = xEventLoopTimerAfter(s->loop, read_timeout_cb, s,
                                          (uint64_t)read_timeout_ms);
  } else {
    cancel_read_timer(s);
  }

  /* Write timeout */
  s->write_timeout_ms = write_timeout_ms;
  if (write_timeout_ms > 0) {
    cancel_write_timer(s);
    s->write_timer = xEventLoopTimerAfter(s->loop, write_timeout_cb, s,
                                           (uint64_t)write_timeout_ms);
  } else {
    cancel_write_timer(s);
  }

  return xErrno_Ok;
}

/* ───────────────────── Query ───────────────────── */

int xSocketFd(xSocket sock) {
  if (!sock) return -1;
  return ((struct xSocket_ *)sock)->fd;
}

xEventMask xSocketMask(xSocket sock) {
  if (!sock) return 0;
  return ((struct xSocket_ *)sock)->mask;
}
