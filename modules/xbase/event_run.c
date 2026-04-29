/*
 * Copyright 2025 The xKit Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * event_run.c - Backend-independent event loop driver (Plan B)
 */

#include "event_private.h"
#include <limits.h>

void xEventLoopRun(xEventLoop loop_) {
  struct xEventLoop_ *loop = (struct xEventLoop_ *)loop_;
  if (!loop) return;

  loop->stopped = 0;

  while (!loop->stopped) {
    xEventWait(loop_, -1);
  }
}

void xEventLoopStop(xEventLoop loop_) {
  struct xEventLoop_ *loop = (struct xEventLoop_ *)loop_;
  if (!loop) return;

  loop->stopped = 1;
  xEventWake(loop_);
}

xErrno xEventLoopWait(xEventLoop loop_, int timeout_ms) {
  int                 wait_ms;
  uint64_t            deadline, remaining;
  struct xEventLoop_ *loop = (struct xEventLoop_ *)loop_;

  if (!loop) return xErrno_InvalidArg;

  loop->stopped = 0;
  deadline      = (timeout_ms >= 0) ? xMonoMs() + (uint64_t)timeout_ms : 0;

  while (!loop->stopped) {
    wait_ms = -1;
    if (timeout_ms >= 0) {
      remaining = (int64_t)(deadline - xMonoMs());
      if (remaining <= 0) return xErrno_Timeout;
      wait_ms = (remaining > INT_MAX) ? INT_MAX : (int)remaining;
    }
    xEventWait(loop_, wait_ms);
  }
  /* Clear the stop flag before returning so that an enclosing
   * xEventLoopRun() is not inadvertently terminated.  Callers
   * that use xEventLoopWait as a "block until done" primitive
   * (e.g. shell_handler) call xEventLoopStop from their
   * completion callback; that flag must not leak to the outer
   * loop. */
  loop->stopped = 0;
  return xErrno_Ok;
}
