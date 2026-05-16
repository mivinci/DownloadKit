/*
 * Copyright 2025 The libx Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * event_wsapoll.c - WSAPoll-based event loop (Windows)
 *
 * Uses WSAPoll() for I/O multiplexing on Windows. API semantics are
 * identical to the POSIX poll() backend: edge-triggered emulation via
 * disabling events after each notification, requiring the caller to
 * re-arm via xEventMod().
 *
 * Wake mechanism: loopback socket pair (connect-to-localhost) replaces
 * the POSIX pipe(). Only sockets can be polled with WSAPoll; the
 * CreateEvent HANDLE in event_private.h is left unused by this backend.
 */

#ifdef _WIN32

#include "event_private.h"

#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>

#include <mswsock.h>

/* ───────────────────── WSAStartup ───────────────────── */

static INIT_ONCE wsa_init_once = INIT_ONCE_STATIC_INIT;

static BOOL CALLBACK wsa_init_fn(PINIT_ONCE once, PVOID param, PVOID *ctx) {
  (void)once;
  (void)param;
  (void)ctx;
  WSADATA d;
  WSAStartup(MAKEWORD(2, 2), &d);
  return TRUE;
}

static void wsa_ensure_init(void) {
  InitOnceExecuteOnce(&wsa_init_once, wsa_init_fn, NULL, NULL);
}

/* ───────────────────── Helpers ───────────────────── */

static int set_nonblock(SOCKET fd) {
  u_long mode = 1;
  return ioctlsocket(fd, FIONBIO, &mode);
}

static short mask_to_poll(xEventMask mask) {
  short ev = 0;
  if (mask & xEvent_Read) ev |= POLLIN;
  if (mask & xEvent_Write) ev |= POLLOUT;
  return ev;
}

/*
 * Create a loopback socket pair for the wake pipe.
 *
 * WSAPoll can only poll sockets, so we create a TCP connection to
 * localhost and use the two ends as read/write fds — same as a pipe().
 */
static int wake_socket_pair(SOCKET *rfd, SOCKET *wfd) {
  SOCKET listener = INVALID_SOCKET;
  SOCKET conn     = INVALID_SOCKET;
  SOCKET acceptor = INVALID_SOCKET;

  listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (listener == INVALID_SOCKET) goto fail;

  /* Bind to loopback on an ephemeral port */
  struct sockaddr_in addr;
  memset(&addr, 0, sizeof(addr));
  addr.sin_family      = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port        = 0;
  if (bind(listener, (struct sockaddr *)&addr, sizeof(addr)) != 0) goto fail;

  if (listen(listener, 1) != 0) goto fail;

  /* Get the assigned port */
  int addrlen = sizeof(addr);
  if (getsockname(listener, (struct sockaddr *)&addr, &addrlen) != 0) goto fail;

  /* Connect to self */
  conn = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (conn == INVALID_SOCKET) goto fail;

  if (connect(conn, (struct sockaddr *)&addr, sizeof(addr)) != 0) goto fail;

  acceptor = accept(listener, NULL, NULL);
  if (acceptor == INVALID_SOCKET) goto fail;

  /* Set both ends non-blocking */
  if (set_nonblock(acceptor) != 0 || set_nonblock(conn) != 0) goto fail;

  closesocket(listener);

  *rfd = acceptor; /* read end */
  *wfd = conn;     /* write end */
  return 0;

fail:
  if (listener != INVALID_SOCKET) closesocket(listener);
  if (conn != INVALID_SOCKET) closesocket(conn);
  if (acceptor != INVALID_SOCKET) closesocket(acceptor);
  return -1;
}

/* ───────────────────── WSAPoll-specific loop data ───────────────────── */

struct xEventLoopWSAPoll_ {
  struct xEventLoop_ base;

  /* Loopback socket pair for wake (replaces pipe on POSIX) */
  SOCKET wake_rfd;
  SOCKET wake_wfd;

  /* Parallel arrays: pollfds[i] corresponds to sources.items[i],
   * except pollfds[0] is always the wake socket. */
  WSAPOLLFD *pollfds;
  size_t     pfd_len;
  size_t     pfd_cap;
};

static int pfd_grow(struct xEventLoopWSAPoll_ *loop) {
  size_t      newcap = loop->pfd_cap ? loop->pfd_cap * 2 : 16;
  WSAPOLLFD  *tmp =
    (WSAPOLLFD *)realloc(loop->pollfds, newcap * sizeof(WSAPOLLFD));
  if (!tmp) return -1;
  loop->pollfds = tmp;
  loop->pfd_cap = newcap;
  return 0;
}

/* Rebuild pollfds array from sources list + wake socket */
static void pfd_rebuild(struct xEventLoopWSAPoll_ *loop) {
  size_t needed = 1 + loop->base.sources.len; /* wake + sources */
  while (loop->pfd_cap < needed)
    pfd_grow(loop);

  /* Slot 0: wake socket */
  loop->pollfds[0].fd      = (SOCKET)loop->wake_rfd;
  loop->pollfds[0].events  = POLLIN;
  loop->pollfds[0].revents = 0;

  for (size_t i = 0; i < loop->base.sources.len; i++) {
    struct xEventSource_ *src    = loop->base.sources.items[i];
    loop->pollfds[1 + i].fd      = (SOCKET)src->fd;
    loop->pollfds[1 + i].events  = mask_to_poll(src->mask);
    loop->pollfds[1 + i].revents = 0;
  }

  loop->pfd_len = needed;
}

/* ───────────────────── Public API ───────────────────── */

xEventLoop xEventLoopCreate(void) {
  return xEventLoopCreateWithGroup(NULL);
}

xEventLoop xEventLoopCreateWithGroup(xTaskGroup group) {
  wsa_ensure_init();

  struct xEventLoopWSAPoll_ *loop =
    (struct xEventLoopWSAPoll_ *)calloc(1, sizeof(*loop));
  if (!loop) return NULL;

  loop->base.wake_event   = NULL; /* not used by WSAPoll backend */
  loop->base.stopped      = 0;
  loop->base.timer_heap   = NULL;
  loop->base.task_group   = group;
  source_array_init(&loop->base.sources);
  loop->base.done_head    = NULL;
  loop->base.done_tail    = NULL;
  loop->base.work_freelist = NULL;
  xAtomicStore(&loop->base.inflight, 0, xAtomicRelaxed);
  xAtomicStore(&loop->base.wake_pending, 0, xAtomicRelaxed);

  loop->base.timer_heap = xHeapCreate(event_timer_cmp, event_timer_set_idx, 0);
  if (!loop->base.timer_heap) goto fail;
  xMutexInit(&loop->base.timer_mu);

  if (wake_socket_pair(&loop->wake_rfd, &loop->wake_wfd) != 0) goto fail;

  return (xEventLoop)loop;

fail:
  if (loop->wake_rfd != INVALID_SOCKET) closesocket(loop->wake_rfd);
  if (loop->wake_wfd != INVALID_SOCKET) closesocket(loop->wake_wfd);
  source_array_free(&loop->base.sources);
  if (loop->base.timer_heap) {
    xHeapDestroy(loop->base.timer_heap);
    xMutexDestroy(&loop->base.timer_mu);
  }
  free(loop);
  return NULL;
}

void xEventLoopDestroy(xEventLoop loop_) {
  struct xEventLoopWSAPoll_ *loop = (struct xEventLoopWSAPoll_ *)loop_;
  if (!loop) return;

  /* Discard all pending timers without firing */
  xMutexLock(&loop->base.timer_mu);
  while (xHeapSize(loop->base.timer_heap) > 0) {
    struct xEventTimer_ *t =
      (struct xEventTimer_ *)xHeapPop(loop->base.timer_heap);
    event_timer_free(&loop->base, t);
  }
  xMutexUnlock(&loop->base.timer_mu);
  event_timer_pool_destroy(&loop->base);
  xHeapDestroy(loop->base.timer_heap);
  xMutexDestroy(&loop->base.timer_mu);

  loop_wait_inflight(&loop->base);
  loop_cleanup_done(&loop->base);
  event_work_pool_destroy(&loop->base);

  closesocket(loop->wake_rfd);
  closesocket(loop->wake_wfd);

  source_array_free(&loop->base.sources);
  free(loop->pollfds);
  free(loop);
}

xEventSource xEventAdd(xEventLoop loop_, int fd, xEventMask mask, xEventFunc fn,
                       void *arg) {
  struct xEventLoopWSAPoll_ *loop = (struct xEventLoopWSAPoll_ *)loop_;
  if (!loop || !fn) return NULL;

  struct xEventSource_ *src =
    source_array_add(&loop->base.sources, fd, mask, fn, arg);
  if (!src) return NULL;

  if (set_nonblock((SOCKET)fd) != 0) {
    source_array_remove(&loop->base.sources, src);
    return NULL;
  }

  return (xEventSource)src;
}

xErrno xEventMod(xEventLoop loop_, xEventSource src_, xEventMask mask) {
  struct xEventLoopWSAPoll_ *loop = (struct xEventLoopWSAPoll_ *)loop_;
  struct xEventSource_      *src  = (struct xEventSource_ *)src_;
  if (!loop || !src) return xErrno_InvalidArg;

  src->mask = mask;
  return xErrno_Ok;
}

xErrno xEventDel(xEventLoop loop_, xEventSource src_) {
  struct xEventLoopWSAPoll_ *loop = (struct xEventLoopWSAPoll_ *)loop_;
  struct xEventSource_      *src  = (struct xEventSource_ *)src_;
  if (!loop || !src) return xErrno_InvalidArg;

  source_array_remove(&loop->base.sources, src);
  return xErrno_Ok;
}

int xEventWait(xEventLoop loop_, int timeout_ms) {
  struct xEventLoopWSAPoll_ *loop = (struct xEventLoopWSAPoll_ *)loop_;
  if (!loop) return -1;

  /* Adjust timeout based on timer heap */
  int effective_timeout = timeout_ms;
  xMutexLock(&loop->base.timer_mu);
  struct xEventTimer_ *top =
    (struct xEventTimer_ *)xHeapPeek(loop->base.timer_heap);
  if (top) {
    uint64_t now           = xMonoMs();
    int64_t  wait          = (int64_t)(top->deadline - now);
    int      timer_timeout = (wait <= 0) ? 0 : (int)wait;
    if (effective_timeout < 0 || timer_timeout < effective_timeout)
      effective_timeout = timer_timeout;
  }
  xMutexUnlock(&loop->base.timer_mu);

  pfd_rebuild(loop);

  int n = WSAPoll(loop->pollfds, (ULONG)loop->pfd_len, effective_timeout);
  if (n < 0) n = 0;

  int dispatched = 0;

  /* Check wake socket (slot 0) */
  if (loop->pollfds[0].revents & POLLIN) {
    /* Drain the wake socket */
    char buf[64];
    while (recv(loop->wake_rfd, buf, sizeof(buf), 0) > 0)
      ;
    loop_clear_wake_pending(&loop->base);
    loop_dispatch_done(&loop->base);
  }

  /* Check sources (slots 1..pfd_len-1) */
  for (size_t i = 0; i < loop->base.sources.len; i++) {
    WSAPOLLFD *pfd = &loop->pollfds[1 + i];
    if (pfd->revents == 0) continue;

    struct xEventSource_ *src = loop->base.sources.items[i];
    if (src->deleted) continue;
    xEventMask ready = 0;
    if (pfd->revents & POLLIN) ready |= xEvent_Read;
    if (pfd->revents & POLLOUT) ready |= xEvent_Write;
    /* POLLERR/POLLHUP map to read so the user gets notified */
    if (pfd->revents & (POLLERR | POLLHUP)) ready |= xEvent_Read;

    if (ready) {
      xEventMask orig_mask = src->mask;
      src->mask = 0; /* edge-triggered: disable to prevent level-triggered re-fire */
      src->fn(src->fd, ready, src->arg);

      /* Re-arm the source if the fd was fully drained.
       * This emulates EPOLLET's "empty → non-empty" edge semantics:
       * after the callback drains the buffer, restoring the mask
       * allows the next data arrival to trigger a new edge. */
      if (!src->deleted && src->mask == 0) {
        xEventMask restore = 0;
        if (orig_mask & xEvent_Read) {
          /* Peek to check if the read buffer is empty */
          char buf;
          int r = recv((SOCKET)src->fd, &buf, 1, MSG_PEEK);
          if (r <= 0) {
            /* Buffer is empty (EWOULDBLOCK) or connection closed — re-arm */
            restore |= (orig_mask & xEvent_Read);
          }
          /* r > 0: data still available, keep read disabled (no new edge) */
        }
        if (orig_mask & xEvent_Write) {
          /* Re-arm write: if the write buffer was full (blocking send),
           * the fire indicated it became writable again. Restoring allows
           * detecting the next "full → writable" transition. */
          restore |= (orig_mask & xEvent_Write);
        }
        src->mask = restore;
      }
      dispatched++;
    }
  }

  /* Fire expired timers (batch pop, single lock acquisition) */
  loop_fire_expired_timers(&loop->base);

  /* Sweep sources marked for deletion during this dispatch batch. */
  source_array_sweep(&loop->base.sources);

  return dispatched;
}

xErrno xEventWake(xEventLoop loop_) {
  struct xEventLoopWSAPoll_ *loop = (struct xEventLoopWSAPoll_ *)loop_;
  if (!loop) return xErrno_InvalidArg;

  /* Coalesce: skip the syscall if another thread already set the flag. */
  if (!loop_coalesced_wake(&loop->base)) return xErrno_Ok;

  char c = 1;
  send(loop->wake_wfd, &c, 1, 0);

  return xErrno_Ok;
}

xErrno xEventLoopSignalWatch(xEventLoop loop_, int signo, xEventSignalFunc fn,
                             void *arg) {
  (void)loop_;
  (void)signo;
  (void)fn;
  (void)arg;
  /* POSIX signals are not available on Windows. */
  return xErrno_InvalidArg;
}

#endif /* _WIN32 */
