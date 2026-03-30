/*
 * Copyright 2025 The xKit Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * timer.c - Monotonic timer implementation
 */

#include <xbase/timer.h>
#include <xbase/heap.h>
#include <xbase/mpsc.h>

#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ───────────────────── Internal types ───────────────────── */

#define TIMER_INVALID_IDX ((size_t)-1)

struct xTimerTask_ {
  xMpsc        node;      /* intrusive MPSC node; xContainerOf is used to   */
                          /* recover the enclosing struct, so this field may */
                          /* appear anywhere in the layout                   */
  uint64_t     deadline;  /* expiry time in ms, CLOCK_MONOTONIC             */
  xTimerFunc   fn;
  void        *arg;
  size_t       heap_idx;  /* current position in the min-heap;              */
                          /* TIMER_INVALID_IDX when not in the heap          */
  int          cancelled; /* set to 1 under mu before removing from heap    */
};

struct xTimer_ {
  xHeap            heap;      /* min-heap ordered by deadline               */
  xTaskGroup       group;     /* non-NULL → push mode; NULL → poll mode     */

  /* Poll-mode queue (lock-free MPSC)                                       */
  xMpsc           *mq_head;
  xMpsc           *mq_tail;

  pthread_t        thread;
  pthread_mutex_t  mu;
  pthread_cond_t   cond;
  int              stopped;
};

/* ───────────────────── Helpers ───────────────────── */

uint64_t xTimerNowMs(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

static int cmp_task(const void *a, const void *b) {
  const struct xTimerTask_ *ta = (const struct xTimerTask_ *)a;
  const struct xTimerTask_ *tb = (const struct xTimerTask_ *)b;
  if (ta->deadline < tb->deadline) return -1;
  if (ta->deadline > tb->deadline) return  1;
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
  task->fn(task->arg);
  free(task);
  return NULL;
}

static void fire(struct xTimer_ *t, struct xTimerTask_ *task) {
  if (task->cancelled) {
    free(task);
    return;
  }

  if (t->group) {
    /* Push mode: hand off to worker pool */
    if (!xTaskSubmit(t->group, dispatch_wrapper, task))
      free(task); /* submit failed, must still free */
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

  pthread_mutex_lock(&t->mu);

  for (;;) {
    /* Wait while heap is empty and not stopped */
    while (!t->stopped && xHeapSize(t->heap) == 0)
      pthread_cond_wait(&t->cond, &t->mu);

    if (t->stopped) break;

    struct xTimerTask_ *top = (struct xTimerTask_ *)xHeapPeek(t->heap);
    uint64_t now            = xTimerNowMs();

    if (top->deadline <= now) {
      xHeapPop(t->heap);
      pthread_mutex_unlock(&t->mu);
      fire(t, top);
      pthread_mutex_lock(&t->mu);
    } else {
      /* Sleep until next deadline (or until signalled) */
      uint64_t wait_ms = top->deadline - now;
      struct timespec abs_ts;
      clock_gettime(CLOCK_REALTIME, &abs_ts);
      abs_ts.tv_sec  += (time_t)(wait_ms / 1000);
      abs_ts.tv_nsec += (long)((wait_ms % 1000) * 1000000L);
      if (abs_ts.tv_nsec >= 1000000000L) {
        abs_ts.tv_sec++;
        abs_ts.tv_nsec -= 1000000000L;
      }
      pthread_cond_timedwait(&t->cond, &t->mu, &abs_ts);
    }
  }

  /* Drain and discard remaining heap entries */
  while (xHeapSize(t->heap) > 0) {
    struct xTimerTask_ *task = (struct xTimerTask_ *)xHeapPop(t->heap);
    free(task);
  }

  pthread_mutex_unlock(&t->mu);
  return NULL;
}

/* ───────────────────── Public API ───────────────────── */

xTimer xTimerCreate(xTaskGroup g) {
  struct xTimer_ *t = (struct xTimer_ *)calloc(1, sizeof(struct xTimer_));
  if (!t) return NULL;

  t->group = g;

  t->heap = xHeapCreate(cmp_task, set_idx, 0);
  if (!t->heap) goto fail_heap;

  if (pthread_mutex_init(&t->mu, NULL) != 0) goto fail_mutex;
  if (pthread_cond_init(&t->cond, NULL) != 0) goto fail_cond;
  if (pthread_create(&t->thread, NULL, timer_thread, t) != 0) goto fail_thread;

  return (xTimer)t;

fail_thread: pthread_cond_destroy(&t->cond);
fail_cond:   pthread_mutex_destroy(&t->mu);
fail_mutex:  xHeapDestroy(t->heap);
fail_heap:   free(t);
  return NULL;
}

void xTimerDestroy(xTimer t_) {
  struct xTimer_ *t = (struct xTimer_ *)t_;
  if (!t) return;

  pthread_mutex_lock(&t->mu);
  t->stopped = 1;
  pthread_cond_signal(&t->cond);
  pthread_mutex_unlock(&t->mu);

  pthread_join(t->thread, NULL);

  /* Drain poll-mode queue (timer thread is gone, no concurrency) */
  if (!t->group) {
    xMpsc *node;
    while ((node = xMpscPop(&t->mq_head, &t->mq_tail)) != NULL) {
      struct xTimerTask_ *task =
          xContainerOf(node, struct xTimerTask_, node);
      free(task);
    }
  }

  pthread_cond_destroy(&t->cond);
  pthread_mutex_destroy(&t->mu);
  xHeapDestroy(t->heap);
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

  struct xTimerTask_ *task =
      (struct xTimerTask_ *)calloc(1, sizeof(struct xTimerTask_));
  if (!task) return NULL;

  task->deadline  = abs_ms;
  task->fn        = fn;
  task->arg       = arg;
  task->heap_idx  = TIMER_INVALID_IDX;
  task->cancelled = 0;

  pthread_mutex_lock(&t->mu);
  xErrno err = xHeapPush(t->heap, task);
  if (err != xErrno_Ok) {
    pthread_mutex_unlock(&t->mu);
    free(task);
    return NULL;
  }
  pthread_cond_signal(&t->cond);
  pthread_mutex_unlock(&t->mu);

  return (xTimerTask)task;
}

xTimerTask xTimerSubmitAfter(xTimer t, xTimerFunc fn, void *arg,
                              uint64_t delay_ms) {
  return submit(t, fn, arg, xTimerNowMs() + delay_ms);
}

xTimerTask xTimerSubmitAt(xTimer t, xTimerFunc fn, void *arg, uint64_t abs_ms) {
  return submit(t, fn, arg, abs_ms);
}

xErrno xTimerCancel(xTimer t_, xTimerTask task_) {
  struct xTimer_     *t    = (struct xTimer_ *)t_;
  struct xTimerTask_ *task = (struct xTimerTask_ *)task_;
  if (!t || !task) return xErrno_Unknown;

  pthread_mutex_lock(&t->mu);

  if (task->cancelled || task->heap_idx == TIMER_INVALID_IDX) {
    pthread_mutex_unlock(&t->mu);
    return xErrno_Unknown; /* already fired or cancelled */
  }

  xHeapRemove(t->heap, task->heap_idx);
  task->heap_idx  = TIMER_INVALID_IDX;
  task->cancelled = 1;

  pthread_mutex_unlock(&t->mu);

  free(task);
  return xErrno_Ok;
}

int xTimerPoll(xTimer t_) {
  struct xTimer_ *t = (struct xTimer_ *)t_;
  if (!t || t->group) return 0; /* push mode: no-op */

  int count = 0;
  xMpsc *node;
  while ((node = xMpscPop(&t->mq_head, &t->mq_tail)) != NULL) {
    struct xTimerTask_ *task =
        xContainerOf(node, struct xTimerTask_, node);
    task->fn(task->arg);
    free(task);
    count++;
  }
  return count;
}
