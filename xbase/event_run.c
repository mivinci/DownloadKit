/*
 * Copyright 2025 The xKit Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * event_run.c - Backend-independent event loop driver
 */

#include "event_base.h"

#include <stdint.h>

/* Default maximum timeout when no timer is attached (1 second) */
#define DEFAULT_MAX_TIMEOUT_MS 1000

void xEventLoopRun(xEventLoop loop_) {
  struct xEventLoop_ *loop = (struct xEventLoop_ *)loop_;
  if (!loop) return;

  loop->stopped = 0;

  while (!loop->stopped) {
    int timeout_ms = -1; /* block indefinitely by default */

    if (loop->timer) {
      uint64_t deadline = xTimerNextDeadline(loop->timer);
      if (deadline == UINT64_MAX) {
        timeout_ms = DEFAULT_MAX_TIMEOUT_MS;
      } else {
        uint64_t now = xTimerNowMs();
        if (deadline <= now) {
          timeout_ms = 0;
        } else {
          uint64_t diff = deadline - now;
          timeout_ms = (diff > (uint64_t)DEFAULT_MAX_TIMEOUT_MS)
                           ? DEFAULT_MAX_TIMEOUT_MS
                           : (int)diff;
        }
      }
    }

    xEventWait(loop_, timeout_ms);

    if (loop->timer) {
      xTimerPoll(loop->timer);
    }
  }
}

void xEventLoopStop(xEventLoop loop_) {
  struct xEventLoop_ *loop = (struct xEventLoop_ *)loop_;
  if (!loop) return;

  loop->stopped = 1;
  xEventWake(loop_);
}
