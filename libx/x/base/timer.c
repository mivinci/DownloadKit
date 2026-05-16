/*
 * Copyright 2025 The libx Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * timer.c - Monotonic timer implementation
 */

#include <x/base/heap.h>
#include <x/base/mpsc.h>
#include <x/base/slab.h>
#include <x/base/timer.h>

#include "thread_private.h"
#include <stdlib.h>
#include <string.h>

/* ───────────────────── Internal types ───────────────────── */

#define TIMER_INVALID_IDX ((size_t) - 1)

/* Forward declaration so xTimerTask_ can hold a pointer to its owner. */
struct xTimer_;

struct xTimerTask_ {
  xMpsc node;               /* intrusive MPSC node; xContainerOf is used to   */
                            /* recover the enclosing struct, so this field may */
                            /* appear anywhere in the layout                   */
  struct xTimer_ *owner;    /* back-pointer to the timer (for slab free)      */
  uint64_t        deadline; /* expiry time in ms, CLOCK_MONOTONIC             */
  xTimerFunc      fn;
  void           *arg;
  size_t          heap_idx; /* current position in the min-heap;              */
                            /* TIMER_INVALID_IDX when not in the heap          */
  int cancelled;            /* set to 1 under mu before removing from heap    */
};

struct xTimer_ {
  xHeap      heap;  /* min-heap ordered by deadline               */
  xTaskGroup group; /* non-NULL → push mode; NULL → poll mode     */

  /* Poll-mode queue (lock-free MPSC)                                       */
  xMpsc *mq_head;
  xMpsc *mq_tail;

  /* Multi-threaded slab: tasks are allocated by caller threads and freed
   * by either the timer thread, the worker thread (push mode), or the
   * caller (cancel / poll / destroy-drain).  Caller MUST ensure no worker
   * still references a task when xTimerDestroy returns. */
  xSlabMt *task_pool;

  xThread thread;
  xMutex  mu;
  xCond   cond;
  int     stopped;
};

/* ───────────────────── Helpers ───────────────────── */

uint64_t xTimerNowMs(void) {
  return xMonoMs();
}

static int cmp_task(const void *a, const void *b) {
  const struct xTimerTask_ *ta = (const struct xTimerTask_ *)a;
  const struct xTimerTask_ *tb = (const struct xTimerTask_ *)b;
  if (ta->deadline < tb->deadline) return -1;
  if (ta->deadline > tb->deadline) return 1;
  return 0;
}

static void set_idx(void *elem, size_t idx) {
  ((struct xTimerTask_ *)elem)->heap_idx = idx;
}

/* ───────────────────── Fire ───────────────────── */

/*
 * Push-mode wrapper: runs on a worker thread.
 * Owns task memory; frees it after fn() returns.
 */
static void *dispatch_wrapper(void *arg) {
  struct xTimerTask_ *task = (struct xTimerTask_ *)arg;
  struct xTimer_     *t    = task->owner;
  task->fn(task->arg);
  xSlabMtFree(t->task_pool, task);
  return NULL;
}

static void fire(struct xTimer_ *t, struct xTimerTask_ *task) {
  if (task->cancelled) {
    xSlabMtFree(t->task_pool, task);
    return;
  }

  if (t->group) {
    /* Push mode: hand off to worker pool */
    if (!xTaskSubmit(t->group, dispatch_wrapper, task))
      xSlabMtFree(t->task_pool, task); /* submit failed, must still free */
  } else {
    /* Poll mode: enqueue for caller to drain via xTimerPoll() */
    xMpscPush(&t->mq_head, &t->mq_tail, &task->node);
  }
}

/* ───────────────────── Timer thread ───────────────────── */

/*
 * Background thread that drives all timer expiry for one xTimer instance.
 *
 * The thread holds mu for the duration of the loop and releases it only
 * while calling fire() (to avoid holding the lock during potentially slow
 * dispatch_wrapper submissions or MPSC enqueues).
 *
 * Wakeup sources:
 *   - xTimerSubmitAfter / xTimerSubmitAt: signals cond after pushing to heap
 *     so the thread re-evaluates the earliest deadline.
 *   - xTimerDestroy: sets stopped = 1 and signals cond to break the loop.
 */

static void *timer_thread(void *arg) {
  struct xTimer_ *t = (struct xTimer_ *)arg;

  xMutexLock(&t->mu);

  for (;;) {
    /* Wait while heap is empty and not stopped */
    while (!t->stopped && xHeapSize(t->heap) == 0)
      xCondWait(&t->cond, &t->mu);

    if (t->stopped) break;

    struct xTimerTask_ *top = (struct xTimerTask_ *)xHeapPeek(t->heap);
    uint64_t            now = xMonoMs();

    if (top->deadline <= now) {
      xHeapPop(t->heap);
      xMutexUnlock(&t->mu);
      fire(t, top);
      xMutexLock(&t->mu);
    } else {
      /* Sleep until next deadline (or until signalled) */
      uint64_t wait_ms = top->deadline - now;
#ifdef _WIN32
      SleepConditionVariableCS(&t->cond, &t->mu, (DWORD)wait_ms);
#else
      struct timespec abs_ts;
      clock_gettime(CLOCK_REALTIME, &abs_ts);
      abs_ts.tv_sec += (time_t)(wait_ms / 1000);
      abs_ts.tv_nsec += (long)((wait_ms % 1000) * 1000000L);
      if (abs_ts.tv_nsec >= 1000000000L) {
        abs_ts.tv_sec++;
        abs_ts.tv_nsec -= 1000000000L;
      }
      pthread_cond_timedwait(&t->cond, &t->mu, &abs_ts);
#endif
    }
  }

  /* Drain and discard remaining heap entries */
  while (xHeapSize(t->heap) > 0) {
    struct xTimerTask_ *task = (struct xTimerTask_ *)xHeapPop(t->heap);
    xSlabMtFree(t->task_pool, task);
  }

  xMutexUnlock(&t->mu);
  return NULL;
}

/* ───────────────────── Public API ───────────────────── */

xTimer xTimerCreate(xTaskGroup g) {
  struct xTimer_ *t = (struct xTimer_ *)calloc(1, sizeof(struct xTimer_));
  if (!t) return NULL;

  t->group = g;

  t->heap = xHeapCreate(cmp_task, set_idx, 0);
  if (!t->heap) goto fail_heap;

  /* Multi-threaded slab: task alloc/free may cross threads in push mode. */
  t->task_pool = xSlabMtCreate(sizeof(struct xTimerTask_), 0, 0);
  if (!t->task_pool) goto fail_pool;

  xMutexInit(&t->mu);
  xCondInit(&t->cond);
  if (xThreadCreate(&t->thread, timer_thread, t) != 0) goto fail_thread;

  return (xTimer)t;

fail_thread:
  xCondDestroy(&t->cond);
  xMutexDestroy(&t->mu);
  xSlabMtDestroy(t->task_pool);
fail_pool:
  xHeapDestroy(t->heap);
fail_heap:
  free(t);
  return NULL;
}

void xTimerDestroy(xTimer t_) {
  struct xTimer_ *t = (struct xTimer_ *)t_;
  if (!t) return;

  xMutexLock(&t->mu);
  t->stopped = 1;
  xCondSignal(&t->cond);
  xMutexUnlock(&t->mu);

  xThreadJoin(t->thread);

  /* Drain poll-mode queue (timer thread is gone, no concurrency) */
  if (!t->group) {
    xMpsc *node;
    while ((node = xMpscPop(&t->mq_head, &t->mq_tail)) != NULL) {
      struct xTimerTask_ *task = xContainerOf(node, struct xTimerTask_, node);
      xSlabMtFree(t->task_pool, task);
    }
  }

  xCondDestroy(&t->cond);
  xMutexDestroy(&t->mu);
  xHeapDestroy(t->heap);
  /* task_pool destroyed last: any late xSlabMtFree above must still work.
   * Caller contract: no worker thread may still reference a task. */
  xSlabMtDestroy(t->task_pool);
  free(t);
}

/*
 * Internal helper shared by xTimerSubmitAfter and xTimerSubmitAt.
 * Allocates a task, pushes it onto the heap, then signals the timer thread
 * so it can re-evaluate the earliest deadline (in case this entry is now
 * the new minimum).
 */
static xTimerTask submit(xTimer t_, xTimerFunc fn, void *arg, uint64_t abs_ms) {
  struct xTimer_ *t = (struct xTimer_ *)t_;
  if (!t || !fn) return NULL;

  struct xTimerTask_ *task = (struct xTimerTask_ *)xSlabMtAlloc(t->task_pool);
  if (!task) return NULL;

  task->owner     = t;
  task->deadline  = abs_ms;
  task->fn        = fn;
  task->arg       = arg;
  task->heap_idx  = TIMER_INVALID_IDX;
  task->cancelled = 0;

  xMutexLock(&t->mu);
  xErrno err = xHeapPush(t->heap, task);
  if (err != xErrno_Ok) {
    xMutexUnlock(&t->mu);
    xSlabMtFree(t->task_pool, task);
    return NULL;
  }
  xCondSignal(&t->cond);
  xMutexUnlock(&t->mu);

  return (xTimerTask)task;
}

xTimerTask xTimerSubmitAfter(xTimer t, xTimerFunc fn, void *arg, uint64_t delay_ms) {
  return submit(t, fn, arg, xMonoMs() + delay_ms);
}

xTimerTask xTimerSubmitAt(xTimer t, xTimerFunc fn, void *arg, uint64_t abs_ms) {
  return submit(t, fn, arg, abs_ms);
}

xErrno xTimerCancel(xTimer t_, xTimerTask task_) {
  struct xTimer_     *t    = (struct xTimer_ *)t_;
  struct xTimerTask_ *task = (struct xTimerTask_ *)task_;
  if (!t || !task) return xErrno_InvalidArg;

  xMutexLock(&t->mu);

  if (task->cancelled || task->heap_idx == TIMER_INVALID_IDX) {
    xMutexUnlock(&t->mu);
    return xErrno_Cancelled; /* already fired or cancelled */
  }

  xHeapRemove(t->heap, task->heap_idx);
  task->heap_idx  = TIMER_INVALID_IDX;
  task->cancelled = 1;

  xMutexUnlock(&t->mu);

  xSlabMtFree(t->task_pool, task);
  return xErrno_Ok;
}

int xTimerPoll(xTimer t_) {
  struct xTimer_ *t = (struct xTimer_ *)t_;
  if (!t || t->group) return 0; /* push mode: no-op */

  int    count = 0;
  xMpsc *node;
  while ((node = xMpscPop(&t->mq_head, &t->mq_tail)) != NULL) {
    struct xTimerTask_ *task = xContainerOf(node, struct xTimerTask_, node);
    task->fn(task->arg);
    xSlabMtFree(t->task_pool, task);
    count++;
  }
  return count;
}
