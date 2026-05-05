/*
 * Copyright 2025 The moo Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * event_private.c - Non-inline implementations for event loop internals
 */

#include "event_private.h"

/* ───────────────────── Source array ───────────────────── */

void source_array_free(struct xEventSourceArray_ *s) {
  for (size_t i = 0; i < s->len; i++)
    free(s->items[i]);
  free(s->items);
  s->items = NULL;
  s->len   = 0;
  s->cap   = 0;
}

struct xEventSource_ *source_array_add(struct xEventSourceArray_ *s, int fd,
                                       xEventMask mask, xEventFunc fn,
                                       void *arg) {
  if (s->len == s->cap) {
    size_t                 newcap = s->cap ? s->cap * 2 : 16;
    struct xEventSource_ **tmp =
      (struct xEventSource_ **)realloc(s->items, newcap * sizeof(*s->items));
    if (!tmp) return NULL;
    s->items = tmp;
    s->cap   = newcap;
  }
  struct xEventSource_ *src =
    (struct xEventSource_ *)calloc(1, sizeof(struct xEventSource_));
  if (!src) return NULL;
  src->fd            = fd;
  src->mask          = mask;
  src->fn            = fn;
  src->arg           = arg;
  s->items[s->len++] = src;
  return src;
}

/**
 * @brief Sweep deleted sources after a dispatch batch completes.
 *
 * Must be called at the end of each xEventWait() to actually free
 * sources that were removed during callback dispatch.
 */
void source_array_sweep(struct xEventSourceArray_ *s) {
  size_t i = 0;
  while (i < s->len) {
    if (s->items[i]->deleted) {
      free(s->items[i]);
      s->items[i] = s->items[--s->len];
    } else {
      i++;
    }
  }
}

/* ───────────────────── Timer firing ───────────────────── */

/*
 * Pop all expired timers under a single lock acquisition, then fire
 * them and return each struct to the pool.  Replaces the per-pop
 * lock/unlock pattern in all three backends.
 */
int loop_fire_expired_timers(struct xEventLoop_ *loop) {
  /* Scratch buffer on stack; fall back to heap for huge batches. */
  struct xEventTimer_  *stack_buf[128];
  struct xEventTimer_ **batch     = stack_buf;
  size_t                batch_cap = 128;
  size_t                batch_len = 0;

  xMutexLock(&loop->timer_mu);
  uint64_t now = xMonoMs();
  while (xHeapSize(loop->timer_heap) > 0) {
    struct xEventTimer_ *t = (struct xEventTimer_ *)xHeapPeek(loop->timer_heap);
    if (t->deadline > now) break;
    xHeapPop(loop->timer_heap);
    t->fired = 1;
    /* Grow batch if needed */
    if (batch_len == batch_cap) {
      size_t                newcap = batch_cap * 2;
      struct xEventTimer_ **heap_buf;
      if (batch == stack_buf) {
        heap_buf = (struct xEventTimer_ **)malloc(
          newcap * sizeof(struct xEventTimer_ *));
        if (heap_buf) memcpy(heap_buf, stack_buf, batch_len * sizeof(*batch));
      } else {
        heap_buf = (struct xEventTimer_ **)realloc(
          batch, newcap * sizeof(struct xEventTimer_ *));
      }
      if (!heap_buf) {
        /* Out of memory — fire what we have so far, recycle, retry later */
        break;
      }
      batch     = heap_buf;
      batch_cap = newcap;
    }
    batch[batch_len++] = t;
  }
  xMutexUnlock(&loop->timer_mu);

  /* Fire callbacks outside the lock */
  for (size_t i = 0; i < batch_len; i++) {
    batch[i]->fn(batch[i]->arg);
  }

  /* Return structs to the pool (re-acquire lock once) */
  if (batch_len > 0) {
    xMutexLock(&loop->timer_mu);
    for (size_t i = 0; i < batch_len; i++) {
      event_timer_free(loop, batch[i]);
    }
    xMutexUnlock(&loop->timer_mu);
  }

  if (batch != stack_buf) free(batch);
  return (int)batch_len;
}

/* ───────────────────── Done queue dispatch ───────────────────── */

/* Dispatch all completed work items (offload + post) from the done queue. */
void loop_dispatch_done(struct xEventLoop_ *loop) {
  xMpsc *node;
  while ((node = xMpscPop(&loop->done_head, &loop->done_tail)) != NULL) {
    struct xEventWork_ *w = xContainerOf(node, struct xEventWork_, mpsc);
    if (w->task) {
      /* Offload item: release the xTask handle, then invoke done_fn.
       * If the task was cancelled, xTaskWait returns xErrno_Cancelled
       * and we skip the done callback — the work_fn never ran. */
      xErrno err = xTaskWait(w->task, NULL);
      if (err != xErrno_Cancelled && w->done_fn) w->done_fn(w->arg, w->result);
      xAtomicFetchSub(&loop->inflight, 1, xAtomicRelaxed);
    } else {
      /* Posted item (xEventLoopPost): invoke the callback directly. */
      if (w->post_fn) w->post_fn(w->arg);
    }
    event_work_free(loop, w);
  }
}

/* Drain remaining work items without executing callbacks (for destroy). */
void loop_cleanup_done(struct xEventLoop_ *loop) {
  xMpsc *node;
  while ((node = xMpscPop(&loop->done_head, &loop->done_tail)) != NULL) {
    struct xEventWork_ *w = xContainerOf(node, struct xEventWork_, mpsc);
    if (w->task) xTaskWait(w->task, NULL);
    free(w); /* truly free — loop is being destroyed */
  }
}

/*
 * Spin-wait until all in-flight offload workers have finished and
 * pushed their results into the done queue.  Must be called before
 * loop_cleanup_done() during destroy to avoid use-after-free.
 */
void loop_wait_inflight(struct xEventLoop_ *loop) {
  while (xAtomicLoad(&loop->inflight, xAtomicAcquire) > 0) {
    /* Brief yield to let worker threads finish. */
#ifdef _WIN32
    Sleep(0);
#else
    usleep(100);
#endif
  }
}
