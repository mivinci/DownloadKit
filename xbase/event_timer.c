/*
 * Copyright 2025 The xKit Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * event_timer.c - Builtin event loop timer API (Plan B)
 */

#include "event_private.h"

/* ───────────────────── Helpers ───────────────────── */

static xEventTimer submit_timer(xEventLoop loop_, xEventTimerFunc fn,
                                void *arg, uint64_t abs_ms) {
  struct xEventLoop_ *loop = (struct xEventLoop_ *)loop_;
  if (!loop || !fn) return NULL;

  struct xEventTimer_ *t =
      (struct xEventTimer_ *)calloc(1, sizeof(struct xEventTimer_));
  if (!t) return NULL;

  t->deadline = abs_ms;
  t->fn       = fn;
  t->arg      = arg;
  t->heap_idx = EVENT_TIMER_INVALID_IDX;
  t->fired    = 0;

  pthread_mutex_lock(&loop->timer_mu);
  xErrno err = xHeapPush(loop->timer_heap, t);
  pthread_mutex_unlock(&loop->timer_mu);

  if (err != xErrno_Ok) {
    free(t);
    return NULL;
  }

  /* Wake the event loop so it recalculates the timeout */
  xEventWake(loop_);

  return (xEventTimer)t;
}

/* ───────────────────── Public API ───────────────────── */

uint64_t xEventLoopNowMs(void) {
  return xMonoMs();
}

xEventTimer xEventLoopTimerAfter(xEventLoop loop, xEventTimerFunc fn,
                                 void *arg, uint64_t delay_ms) {
  return submit_timer(loop, fn, arg, xMonoMs() + delay_ms);
}

xEventTimer xEventLoopTimerAt(xEventLoop loop, xEventTimerFunc fn,
                              void *arg, uint64_t abs_ms) {
  return submit_timer(loop, fn, arg, abs_ms);
}

xErrno xEventLoopTimerCancel(xEventLoop loop_, xEventTimer timer_) {
  struct xEventLoop_   *loop  = (struct xEventLoop_ *)loop_;
  struct xEventTimer_  *timer = (struct xEventTimer_ *)timer_;
  if (!loop || !timer) return xErrno_InvalidArg;

  pthread_mutex_lock(&loop->timer_mu);

  if (timer->fired || timer->heap_idx == EVENT_TIMER_INVALID_IDX) {
    pthread_mutex_unlock(&loop->timer_mu);
    return xErrno_InvalidState;
  }

  xHeapRemove(loop->timer_heap, timer->heap_idx);
  timer->heap_idx = EVENT_TIMER_INVALID_IDX;

  pthread_mutex_unlock(&loop->timer_mu);

  free(timer);
  return xErrno_Ok;
}
