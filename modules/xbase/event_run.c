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
  struct xEventLoop_ *loop = (struct xEventLoop_ *)loop_;
  if (!loop) return xErrno_InvalidArg;

  loop->stopped = 0;
  uint64_t deadline = (timeout_ms >= 0) ? xMonoMs() + (uint64_t)timeout_ms : 0;

  while (!loop->stopped) {
    int wait_ms;
    if (timeout_ms < 0) {
      wait_ms = -1;
    } else {
      int64_t remaining = (int64_t)(deadline - xMonoMs());
      if (remaining <= 0) return xErrno_Timeout;
      wait_ms = (remaining > INT_MAX) ? INT_MAX : (int)remaining;
    }
    xEventWait(loop_, wait_ms);
  }
  return xErrno_Ok;
}
