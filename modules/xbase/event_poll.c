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

#include "event_private.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/ioctl.h>

/* ───────────────────── Helpers ───────────────────── */

static int set_nonblock(int fd) {
  int flags = fcntl(fd, F_GETFL, 0);
  if (flags < 0) return -1;
  return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

static short mask_to_poll(xEventMask mask) {
  short ev = 0;
  if (mask & xEvent_Read) ev |= POLLIN;
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

  /* Self-pipe trick for signal delivery */
  int signal_pipe_r[XK_SIGNAL_MAX]; /* read end, -1 = unused */
  int signal_pipe_w[XK_SIGNAL_MAX]; /* write end, -1 = unused */
};

static int pfd_grow(struct xEventLoopPoll_ *loop) {
  size_t         newcap = loop->pfd_cap ? loop->pfd_cap * 2 : 16;
  struct pollfd *tmp =
    (struct pollfd *)realloc(loop->pollfds, newcap * sizeof(struct pollfd));
  if (!tmp) return -1;
  loop->pollfds = tmp;
  loop->pfd_cap = newcap;
  return 0;
}

/* Rebuild pollfds array from sources list + wake pipe + signal pipes */
static void pfd_rebuild(struct xEventLoopPoll_ *loop) {
  /* Count active signal pipes */
  size_t nsig = 0;
  for (int i = 1; i < XK_SIGNAL_MAX; i++) {
    if (loop->signal_pipe_r[i] >= 0) nsig++;
  }

  size_t needed =
    1 + loop->base.sources.len + nsig; /* wake + sources + signals */
  while (loop->pfd_cap < needed)
    pfd_grow(loop);

  /* Slot 0: wake pipe */
  loop->pollfds[0].fd      = loop->base.wake_rfd;
  loop->pollfds[0].events  = POLLIN;
  loop->pollfds[0].revents = 0;

  for (size_t i = 0; i < loop->base.sources.len; i++) {
    struct xEventSource_ *src    = loop->base.sources.items[i];
    loop->pollfds[1 + i].fd      = src->fd;
    loop->pollfds[1 + i].events  = mask_to_poll(src->mask);
    loop->pollfds[1 + i].revents = 0;
  }

  /* Append signal pipe read ends */
  size_t idx = 1 + loop->base.sources.len;
  for (int i = 1; i < XK_SIGNAL_MAX; i++) {
    if (loop->signal_pipe_r[i] >= 0) {
      loop->pollfds[idx].fd      = loop->signal_pipe_r[i];
      loop->pollfds[idx].events  = POLLIN;
      loop->pollfds[idx].revents = 0;
      idx++;
    }
  }
  loop->pfd_len = needed;
}

/* ───────────────────── Public API ───────────────────── */

xEventLoop xEventLoopCreate(void) {
  return xEventLoopCreateWithGroup(NULL);
}

xEventLoop xEventLoopCreateWithGroup(xTaskGroup group) {
  struct xEventLoopPoll_ *loop =
    (struct xEventLoopPoll_ *)calloc(1, sizeof(*loop));
  if (!loop) return NULL;

  loop->base.wake_rfd   = -1;
  loop->base.wake_wfd   = -1;
  loop->base.stopped    = 0;
  loop->base.timer_heap = NULL;
  loop->base.task_group = group;
  source_array_init(&loop->base.sources);
  loop->base.done_head     = NULL;
  loop->base.done_tail     = NULL;
  loop->base.work_freelist = NULL;
  xAtomicStore(&loop->base.inflight, 0, xAtomicRelaxed);
  xAtomicStore(&loop->base.wake_pending, 0, xAtomicRelaxed);

  loop->base.timer_heap = xHeapCreate(event_timer_cmp, event_timer_set_idx, 0);
  if (!loop->base.timer_heap) goto fail;
  if (pthread_mutex_init(&loop->base.timer_mu, NULL) != 0) goto fail;

  if (loop_init_wake(&loop->base) != 0) goto fail;
  if (set_nonblock(loop->base.wake_rfd) != 0) goto fail;
  if (set_nonblock(loop->base.wake_wfd) != 0) goto fail;

  for (int i = 0; i < XK_SIGNAL_MAX; i++) {
    loop->signal_pipe_r[i] = -1;
    loop->signal_pipe_w[i] = -1;
  }

  return (xEventLoop)loop;

fail:
  loop_close_wake(&loop->base);
  source_array_free(&loop->base.sources);
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

  /* Close signal pipes */
  for (int i = 0; i < XK_SIGNAL_MAX; i++) {
    if (loop->signal_pipe_r[i] >= 0) close(loop->signal_pipe_r[i]);
    if (loop->signal_pipe_w[i] >= 0) close(loop->signal_pipe_w[i]);
  }

  loop_close_wake(&loop->base);
  source_array_free(&loop->base.sources);
  free(loop->pollfds);
  free(loop);
}

xEventSource xEventAdd(xEventLoop loop_, int fd, xEventMask mask, xEventFunc fn,
                       void *arg) {
  struct xEventLoopPoll_ *loop = (struct xEventLoopPoll_ *)loop_;
  if (!loop || !fn) return NULL;

  struct xEventSource_ *src =
    source_array_add(&loop->base.sources, fd, mask, fn, arg);
  if (!src) return NULL;

  if (set_nonblock(fd) != 0) {
    source_array_remove(&loop->base.sources, src);
    return NULL;
  }

  return (xEventSource)src;
}

xErrno xEventMod(xEventLoop loop_, xEventSource src_, xEventMask mask) {
  struct xEventLoopPoll_ *loop = (struct xEventLoopPoll_ *)loop_;
  struct xEventSource_   *src  = (struct xEventSource_ *)src_;
  if (!loop || !src) return xErrno_InvalidArg;

  src->mask = mask;
  return xErrno_Ok;
}

xErrno xEventDel(xEventLoop loop_, xEventSource src_) {
  struct xEventLoopPoll_ *loop = (struct xEventLoopPoll_ *)loop_;
  struct xEventSource_   *src  = (struct xEventSource_ *)src_;
  if (!loop || !src) return xErrno_InvalidArg;

  source_array_remove(&loop->base.sources, src);
  return xErrno_Ok;
}

int xEventWait(xEventLoop loop_, int timeout_ms) {
  struct xEventLoopPoll_ *loop = (struct xEventLoopPoll_ *)loop_;
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

  pfd_rebuild(loop);

  int n = poll(loop->pollfds, (nfds_t)loop->pfd_len, effective_timeout);
  if (n < 0) n = 0;

  int dispatched = 0;

  /* Check wake pipe (slot 0) */
  if (loop->pollfds[0].revents & POLLIN) {
    loop_drain_wake(&loop->base);
    loop_clear_wake_pending(&loop->base);
    loop_dispatch_done(&loop->base);
  }

  /* Check sources (slots 1..pfd_len-1) */
  for (size_t i = 0; i < loop->base.sources.len; i++) {
    struct pollfd *pfd = &loop->pollfds[1 + i];
    if (pfd->revents == 0) continue;

    struct xEventSource_ *src = loop->base.sources.items[i];
    if (src->deleted) continue;
    xEventMask ready = 0;
    if (pfd->revents & POLLIN) ready |= xEvent_Read;
    if (pfd->revents & POLLOUT) ready |= xEvent_Write;

    if (ready) {
      xEventMask orig_mask = src->mask;
      src->mask =
        0; /* edge-triggered: disable to prevent level-triggered re-fire */
      src->fn(src->fd, ready, src->arg);

      /* Re-arm the source if the fd was fully drained.
       * This emulates EPOLLET's "empty → non-empty" edge semantics:
       * after the callback drains the buffer, restoring the mask
       * allows the next data arrival to trigger a new edge. */
      if (!src->deleted && src->mask == 0) {
        xEventMask restore = 0;
        if (orig_mask & xEvent_Read) {
          /* Check if the read buffer is empty (works for both pipes and
           * sockets) */
          int avail = 0;
          if (ioctl(src->fd, FIONREAD, &avail) == 0 && avail == 0) {
            restore |= (orig_mask & xEvent_Read);
          }
          /* avail > 0: data still available, keep read disabled (no new edge)
           */
        }
        if (orig_mask & xEvent_Write) {
          /* Re-arm write: if the write buffer was full (blocking write),
           * the fire indicated it became writable again. Restoring allows
           * detecting the next "full → writable" transition. */
          restore |= (orig_mask & xEvent_Write);
        }
        src->mask = restore;
      }
      dispatched++;
    }
  }

  /* Check signal pipes */
  size_t sig_base = 1 + loop->base.sources.len;
  size_t sig_idx  = 0;
  for (int s = 1; s < XK_SIGNAL_MAX; s++) {
    if (loop->signal_pipe_r[s] < 0) continue;
    struct pollfd *pfd = &loop->pollfds[sig_base + sig_idx];
    sig_idx++;
    if (!(pfd->revents & POLLIN)) continue;

    /* Drain the pipe */
    char buf[64];
    while (read(loop->signal_pipe_r[s], buf, sizeof(buf)) > 0)
      ;

    if (loop->base.signal_watches[s].fn) {
      loop->base.signal_watches[s].fn(s, loop->base.signal_watches[s].arg);
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
  struct xEventLoopPoll_ *loop = (struct xEventLoopPoll_ *)loop_;
  if (!loop) return xErrno_InvalidArg;

  /* Coalesce: skip the syscall if another thread already set the flag. */
  if (!loop_coalesced_wake(&loop->base)) return xErrno_Ok;

  char    c = 1;
  ssize_t r;
  do {
    r = write(loop->base.wake_wfd, &c, 1);
  } while (r < 0 && errno == EINTR);

  return (r == 1 || (r < 0 && errno == EAGAIN)) ? xErrno_Ok : xErrno_SysError;
}

/* ───────────────────── Signal watch (self-pipe trick) ───────────────────── */

/* Global write-end array for the signal handler (async-signal-safe). */
static volatile int g_signal_pipe_w[XK_SIGNAL_MAX];

static void signal_handler(int signo) {
  if (signo > 0 && signo < XK_SIGNAL_MAX) {
    int wfd = g_signal_pipe_w[signo];
    if (wfd >= 0) {
      char c = (char)signo;
      (void)write(wfd, &c, 1); /* async-signal-safe */
    }
  }
}

static int signo_valid(int signo) {
  return signo > 0 && signo < XK_SIGNAL_MAX && signo != SIGKILL &&
         signo != SIGSTOP;
}

xErrno xEventLoopSignalWatch(xEventLoop loop_, int signo, xEventSignalFunc fn,
                             void *arg) {
  struct xEventLoopPoll_ *loop = (struct xEventLoopPoll_ *)loop_;
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

    close(loop->signal_pipe_r[signo]);
    close(loop->signal_pipe_w[signo]);
    loop->signal_pipe_r[signo] = -1;
    loop->signal_pipe_w[signo] = -1;

    loop->base.signal_watches[signo].fn  = NULL;
    loop->base.signal_watches[signo].arg = NULL;
  }

  return xErrno_Ok;
}

#endif /* !XK_HAS_KQUEUE && !XK_HAS_EPOLL */
