/*
 * Copyright 2025 The xKit Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * event_offload.c - Async offload: submit work to a thread pool,
 *                   deliver completion callback on the event loop thread.
 */

#include "event_private.h"

#include <stdlib.h>

/* ───────────────────── Worker wrapper ───────────────────── */

static void *offload_worker(void *arg) {
  struct xEventWork_ *w = (struct xEventWork_ *)arg;

  /* Execute the user's work function on the worker thread. */
  w->result = w->work_fn(w->arg);

  /* Enqueue the work item into the done queue (lock-free). */
  xMpscPush(&((struct xEventLoop_ *)w->loop)->done_head,
            &((struct xEventLoop_ *)w->loop)->done_tail, &w->mpsc);

  /*
   * Wake the event loop so it drains the done queue promptly.
   *
   * Return value intentionally ignored: EAGAIN means the pipe already
   * has data so the loop will wake anyway; a real failure (closed fd)
   * is a bug in the caller.  Either way the done-queue item is not
   * lost — it will be picked up on the next loop iteration.
   */
  xEventWake(w->loop);

  return NULL;
}

/* ───────────────────── Public API ───────────────────── */

xErrno xEventLoopSubmit(xEventLoop loop, xTaskGroup group, xTaskFunc work_fn,
                        xEventDoneFunc done_fn, void *arg) {
  if (!loop || !work_fn) return xErrno_InvalidArg;

  if (!group) {
    group = ((struct xEventLoop_ *)loop)->task_group;
    if (!group) group = xTaskGroupGlobal();
    if (!group) return xErrno_InvalidState;
  }

  struct xEventWork_ *w = event_work_alloc((struct xEventLoop_ *)loop);
  if (!w) return xErrno_NoMemory;

  w->work_fn = work_fn;
  w->done_fn = done_fn;
  w->arg     = arg;
  w->result  = NULL;
  w->loop    = loop;

  xTask t = xTaskSubmit(group, offload_worker, w);
  if (!t) {
    event_work_free((struct xEventLoop_ *)loop, w);
    return xErrno_SysError;
  }

  w->task = t;
  xAtomicFetchAdd(&((struct xEventLoop_ *)loop)->inflight, 1, xAtomicRelaxed);

  return xErrno_Ok;
}
