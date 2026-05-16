/*
 * Copyright 2025 The moo Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * event_epoll.c - epoll-based event loop (edge-triggered)
 */

#ifdef MOO_HAS_EPOLL

#include "event_private.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>

/* ───────────────────── Helpers ───────────────────── */

static int set_nonblock(int fd) {
  int flags = fcntl(fd, F_GETFL, 0);
  if (flags < 0) return -1;
  return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

static uint32_t mask_to_epoll(xEventMask mask) {
  uint32_t ev = EPOLLET; /* always edge-triggered */
  if (mask & xEvent_Read) ev |= EPOLLIN;
  if (mask & xEvent_Write) ev |= EPOLLOUT;
  return ev;
}

/* ───────────────────── Signal self-pipe (async-signal-safe) ─────────── */

/*
 * Why self-pipe instead of signalfd(2)?
 *
 * signalfd requires the target signal to be blocked (via sigprocmask /
 * pthread_sigmask) in *every* thread of the process.  pthread_sigmask only
 * affects the calling thread; any thread that has not blocked the signal
 * will receive it with the default disposition — typically process
 * termination.  In practice this is fragile: third-party libraries, test
 * frameworks (e.g. gtest death-tests), or thread-pools may spawn threads
 * that never call pthread_sigmask, so the signal races to an unblocked
 * thread and kills the process.
 *
 * The self-pipe trick avoids the problem entirely: we install a normal
 * signal handler via sigaction(2) which writes a byte into a pipe.  The
 * handler runs on whichever thread receives the signal (safe — write(2) to
 * a pipe is async-signal-safe), and the event loop picks it up through the
 * pipe's read end registered with epoll.  No thread-wide signal mask
 * manipulation is needed.
 */

/* Global write-end array for the signal handler. */
static volatile int g_signal_pipe_w[MOO_SIGNAL_MAX];

static void signal_handler(int signo) {
  if (signo > 0 && signo < MOO_SIGNAL_MAX) {
    int wfd = g_signal_pipe_w[signo];
    if (wfd >= 0) {
      char c = (char)signo;
      (void)write(wfd, &c, 1); /* async-signal-safe */
    }
  }
}

/* ───────────────────── Epoll-specific loop data ───────────────────── */

struct xEventLoopEpoll_ {
  struct xEventLoop_ base;
  int                epfd;
  /* Self-pipe trick for signal delivery */
  int signal_pipe_r[MOO_SIGNAL_MAX]; /* read end, -1 = unused */
  int signal_pipe_w[MOO_SIGNAL_MAX]; /* write end, -1 = unused */
};

/* Check whether an epoll fd belongs to a signal pipe read end.
 * Returns the signal number, or 0 if not found. */
static int find_signal_by_fd(struct xEventLoopEpoll_ *loop, int fd) {
  for (int i = 1; i < MOO_SIGNAL_MAX; i++) {
    if (loop->signal_pipe_r[i] >= 0 && loop->signal_pipe_r[i] == fd) return i;
  }
  return 0;
}

/* ───────────────────── Public API ───────────────────── */

xEventLoop xEventLoopCreate(void) {
  return xEventLoopCreateWithGroup(NULL);
}

xEventLoop xEventLoopCreateWithGroup(xTaskGroup group) {
  struct xEventLoopEpoll_ *loop =
    (struct xEventLoopEpoll_ *)calloc(1, sizeof(*loop));
  if (!loop) return NULL;

  loop->epfd              = -1;
  loop->base.wake_rfd     = -1;
  loop->base.wake_wfd     = -1;
  loop->base.stopped      = 0;
  loop->base.timer_heap   = NULL;
  loop->base.task_group   = group;
  source_array_init(&loop->base.sources);
  loop->base.done_head = NULL;
  loop->base.done_tail = NULL;
  loop->base.work_freelist = NULL;
  xAtomicStore(&loop->base.inflight, 0, xAtomicRelaxed);
  xAtomicStore(&loop->base.wake_pending, 0, xAtomicRelaxed);

  loop->base.timer_heap = xHeapCreate(event_timer_cmp, event_timer_set_idx, 0);
  if (!loop->base.timer_heap) goto fail;
  if (pthread_mutex_init(&loop->base.timer_mu, NULL) != 0) goto fail;

  for (int i = 0; i < MOO_SIGNAL_MAX; i++) {
    loop->signal_pipe_r[i] = -1;
    loop->signal_pipe_w[i] = -1;
  }

  loop->epfd = epoll_create1(EPOLL_CLOEXEC);
  if (loop->epfd < 0) goto fail;

  /* Use eventfd for lightweight wake (single fd, no pipe). */
  loop->base.wake_rfd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
  if (loop->base.wake_rfd < 0) goto fail;

  /* Register eventfd with epoll (edge-triggered) */
  struct epoll_event ev;
  ev.events  = EPOLLIN | EPOLLET;
  ev.data.fd = loop->base.wake_rfd;
  if (epoll_ctl(loop->epfd, EPOLL_CTL_ADD, loop->base.wake_rfd, &ev) != 0)
    goto fail;

  return (xEventLoop)loop;

fail:
  if (loop->epfd >= 0) close(loop->epfd);
  if (loop->base.wake_rfd >= 0) close(loop->base.wake_rfd);
  source_array_free(&loop->base.sources);
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
    struct xEventTimer_ *t =
      (struct xEventTimer_ *)xHeapPop(loop->base.timer_heap);
    event_timer_free(&loop->base, t);
  }
  pthread_mutex_unlock(&loop->base.timer_mu);
  event_timer_pool_destroy(&loop->base);
  xHeapDestroy(loop->base.timer_heap);
  pthread_mutex_destroy(&loop->base.timer_mu);

  loop_wait_inflight(&loop->base);
  loop_cleanup_done(&loop->base);
  event_work_pool_destroy(&loop->base);

  /* Close any open signal pipes */
  for (int i = 0; i < MOO_SIGNAL_MAX; i++) {
    if (loop->signal_pipe_r[i] >= 0) {
      epoll_ctl(loop->epfd, EPOLL_CTL_DEL, loop->signal_pipe_r[i], NULL);
      close(loop->signal_pipe_r[i]);
    }
    if (loop->signal_pipe_w[i] >= 0) close(loop->signal_pipe_w[i]);
  }

  close(loop->epfd);
  if (loop->base.wake_rfd >= 0) close(loop->base.wake_rfd);
  source_array_free(&loop->base.sources);
  free(loop);
}

xEventSource xEventAdd(xEventLoop loop_, int fd, xEventMask mask, xEventFunc fn,
                       void *arg) {
  struct xEventLoopEpoll_ *loop = (struct xEventLoopEpoll_ *)loop_;
  if (!loop || !fn) return NULL;

  struct xEventSource_ *src =
    source_array_add(&loop->base.sources, fd, mask, fn, arg);
  if (!src) return NULL;

  if (set_nonblock(fd) != 0) {
    source_array_remove(&loop->base.sources, src);
    return NULL;
  }

  struct epoll_event ev;
  ev.events   = mask_to_epoll(mask);
  ev.data.ptr = src;
  if (epoll_ctl(loop->epfd, EPOLL_CTL_ADD, fd, &ev) != 0) {
    source_array_remove(&loop->base.sources, src);
    return NULL;
  }

  return (xEventSource)src;
}

xErrno xEventMod(xEventLoop loop_, xEventSource src_, xEventMask mask) {
  struct xEventLoopEpoll_ *loop = (struct xEventLoopEpoll_ *)loop_;
  struct xEventSource_    *src  = (struct xEventSource_ *)src_;
  if (!loop || !src) return xErrno_InvalidArg;

  struct epoll_event ev;
  ev.events   = mask_to_epoll(mask);
  ev.data.ptr = src;
  if (epoll_ctl(loop->epfd, EPOLL_CTL_MOD, src->fd, &ev) != 0)
    return xErrno_SysError;

  src->mask = mask;
  return xErrno_Ok;
}

xErrno xEventDel(xEventLoop loop_, xEventSource src_) {
  struct xEventLoopEpoll_ *loop = (struct xEventLoopEpoll_ *)loop_;
  struct xEventSource_    *src  = (struct xEventSource_ *)src_;
  if (!loop || !src) return xErrno_InvalidArg;

  epoll_ctl(loop->epfd, EPOLL_CTL_DEL, src->fd, NULL);
  source_array_remove(&loop->base.sources, src);
  return xErrno_Ok;
}

int xEventWait(xEventLoop loop_, int timeout_ms) {
  struct xEventLoopEpoll_ *loop = (struct xEventLoopEpoll_ *)loop_;
  if (!loop) return -1;

  /* Adjust timeout based on timer heap */
  int effective_timeout = timeout_ms;
  pthread_mutex_lock(&loop->base.timer_mu);
  struct xEventTimer_ *top =
    (struct xEventTimer_ *)xHeapPeek(loop->base.timer_heap);
  if (top) {
    uint64_t now           = xMonoMs();
    int64_t  wait          = (int64_t)(top->deadline - now);
    int      timer_timeout = (wait <= 0) ? 0 : (int)wait;
    if (effective_timeout < 0 || timer_timeout < effective_timeout)
      effective_timeout = timer_timeout;
  }
  pthread_mutex_unlock(&loop->base.timer_mu);

  struct epoll_event events[64];
  int                n = epoll_wait(loop->epfd, events, 64, effective_timeout);
  if (n < 0) n = 0;

  int dispatched = 0;
  for (int i = 0; i < n; i++) {
    /*
     * epoll_data is a union — we use data.fd for internal fds
     * (wake pipe, signal pipes) and data.ptr for user event sources.
     * Identify internal fds first by comparing data.fd against
     * known descriptors.
     */
    int efd = events[i].data.fd;

    /* Wake eventfd — registered with data.fd */
    if (efd == loop->base.wake_rfd) {
      /* Drain the eventfd counter */
      uint64_t val;
      (void)read(loop->base.wake_rfd, &val, sizeof(val));
      loop_clear_wake_pending(&loop->base);
      loop_dispatch_done(&loop->base);
      continue;
    }

    /* Check if this is a signal pipe event — registered with data.fd */
    int signo = find_signal_by_fd(loop, efd);
    if (signo > 0) {
      /* Drain the signal pipe */
      char buf[64];
      while (read(efd, buf, sizeof(buf)) > 0)
        ;

      if (loop->base.signal_watches[signo].fn) {
        loop->base.signal_watches[signo].fn(
          signo, loop->base.signal_watches[signo].arg);
        dispatched++;
      }
      continue;
    }

    /* User event source — registered with data.ptr */
    struct xEventSource_ *src = (struct xEventSource_ *)events[i].data.ptr;
    if (!src || src->deleted) continue;

    xEventMask ready = 0;
    if (events[i].events & EPOLLIN) ready |= xEvent_Read;
    if (events[i].events & EPOLLOUT) ready |= xEvent_Write;

    src->fn(src->fd, ready, src->arg);
    dispatched++;
  }

  /* Fire expired timers (batch pop, single lock acquisition) */
  loop_fire_expired_timers(&loop->base);

  /* Sweep sources marked for deletion during this dispatch batch. */
  source_array_sweep(&loop->base.sources);

  return dispatched;
}

/* ───────────────────── Signal watch (self-pipe trick) ───────────────────────
 */
static int signo_valid(int signo) {
  return signo > 0 && signo < MOO_SIGNAL_MAX && signo != SIGKILL &&
         signo != SIGSTOP;
}

xErrno xEventLoopSignalWatch(xEventLoop loop_, int signo, xEventSignalFunc fn,
                             void *arg) {
  struct xEventLoopEpoll_ *loop = (struct xEventLoopEpoll_ *)loop_;
  if (!loop || !signo_valid(signo)) return xErrno_InvalidArg;

  if (fn) {
    /* Register or replace */
    if (loop->signal_pipe_r[signo] >= 0) {
      /* Already have a pipe — just replace the callback */
      loop->base.signal_watches[signo].fn  = fn;
      loop->base.signal_watches[signo].arg = arg;
      return xErrno_Ok;
    }

    int fds[2];
    if (pipe(fds) != 0) return xErrno_SysError;
    if (set_nonblock(fds[0]) != 0 || set_nonblock(fds[1]) != 0) {
      close(fds[0]);
      close(fds[1]);
      return xErrno_SysError;
    }

    /* Register the pipe read end with epoll (edge-triggered) */
    struct epoll_event ev;
    ev.events  = EPOLLIN | EPOLLET;
    ev.data.fd = fds[0];
    if (epoll_ctl(loop->epfd, EPOLL_CTL_ADD, fds[0], &ev) != 0) {
      close(fds[0]);
      close(fds[1]);
      return xErrno_SysError;
    }

    loop->signal_pipe_r[signo] = fds[0];
    loop->signal_pipe_w[signo] = fds[1];
    g_signal_pipe_w[signo]     = fds[1];

    loop->base.signal_watches[signo].fn  = fn;
    loop->base.signal_watches[signo].arg = arg;

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = signal_handler;
    sa.sa_flags   = SA_RESTART;
    sigemptyset(&sa.sa_mask);
    if (sigaction(signo, &sa, NULL) < 0) {
      epoll_ctl(loop->epfd, EPOLL_CTL_DEL, fds[0], NULL);
      close(fds[0]);
      close(fds[1]);
      loop->signal_pipe_r[signo]           = -1;
      loop->signal_pipe_w[signo]           = -1;
      g_signal_pipe_w[signo]               = -1;
      loop->base.signal_watches[signo].fn  = NULL;
      loop->base.signal_watches[signo].arg = NULL;
      return xErrno_SysError;
    }
  } else {
    /* Cancel */
    if (loop->signal_pipe_r[signo] < 0)
      return xErrno_Ok; /* nothing to cancel */

    signal(signo, SIG_DFL);
    g_signal_pipe_w[signo] = -1;

    epoll_ctl(loop->epfd, EPOLL_CTL_DEL, loop->signal_pipe_r[signo], NULL);
    close(loop->signal_pipe_r[signo]);
    close(loop->signal_pipe_w[signo]);
    loop->signal_pipe_r[signo] = -1;
    loop->signal_pipe_w[signo] = -1;

    loop->base.signal_watches[signo].fn  = NULL;
    loop->base.signal_watches[signo].arg = NULL;
  }

  return xErrno_Ok;
}

/* ───────────────────── Wake ───────────────────── */

xErrno xEventWake(xEventLoop loop_) {
  struct xEventLoopEpoll_ *loop = (struct xEventLoopEpoll_ *)loop_;
  if (!loop) return xErrno_InvalidArg;

  /* Coalesce: skip the syscall if another thread already set the flag. */
  if (!loop_coalesced_wake(&loop->base)) return xErrno_Ok;

  uint64_t val = 1;
  ssize_t  r;
  do {
    r = write(loop->base.wake_rfd, &val, sizeof(val));
  } while (r < 0 && errno == EINTR);

  return (r == (ssize_t)sizeof(val) || (r < 0 && errno == EAGAIN))
           ? xErrno_Ok
           : xErrno_SysError;
}

#endif /* MOO_HAS_EPOLL */
