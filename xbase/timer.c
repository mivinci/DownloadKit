/*
 * Copyright 2025 The xKit Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * timer.c - Monotonic timer implementation
 */

#include <xbase/timer.h>
#include <xbase/heap.h>

#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ───────────────────── Internal types ───────────────────── */

#define TIMER_INVALID_IDX ((size_t)-1)

struct xTimerTask_ {
  uint64_t    deadline; /* ms, CLOCK_MONOTONIC */
  xTimerFunc  fn;
  void       *arg;
  size_t      heap_idx;
  int         cancelled; /* atomic-ish: written under lock */
};

struct xTimer_ {
  xHeap           heap;
  xTaskGroup      group;  /* NULL → run callbacks on timer thread */
  pthread_t       thread;
  pthread_mutex_t mu;
  pthread_cond_t  cond;
  int             stopped;
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

/* ───────────────────── Timer thread ───────────────────── */

/*
 * Wrapper submitted to xTaskGroup so the callback runs on a worker thread.
 * The task_ memory is owned here: freed after fn() returns.
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
    /* Hand off to worker pool; dispatch_wrapper owns task memory */
    if (!xTaskSubmit(t->group, dispatch_wrapper, task)) {
      /* Submit failed — still need to free */
      free(task);
    }
  } else {
    /* Run directly on timer thread */
    task->fn(task->arg);
    free(task);
  }
}

static void *timer_thread(void *arg) {
  struct xTimer_ *t = (struct xTimer_ *)arg;

  pthread_mutex_lock(&t->mu);

  for (;;) {
    /* Wait while heap is empty and not stopped */
    while (!t->stopped && xHeapSize(t->heap) == 0) {
      pthread_cond_wait(&t->cond, &t->mu);
    }

    if (t->stopped) break;

    struct xTimerTask_ *top = (struct xTimerTask_ *)xHeapPeek(t->heap);
    uint64_t now            = xTimerNowMs();

    if (top->deadline <= now) {
      /* Entry expired: pop and fire */
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

  /* Drain and discard remaining entries */
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
  if (!t->heap) {
    free(t);
    return NULL;
  }

  if (pthread_mutex_init(&t->mu, NULL) != 0) {
    xHeapDestroy(t->heap);
    free(t);
    return NULL;
  }

  if (pthread_cond_init(&t->cond, NULL) != 0) {
    pthread_mutex_destroy(&t->mu);
    xHeapDestroy(t->heap);
    free(t);
    return NULL;
  }

  if (pthread_create(&t->thread, NULL, timer_thread, t) != 0) {
    pthread_cond_destroy(&t->cond);
    pthread_mutex_destroy(&t->mu);
    xHeapDestroy(t->heap);
    free(t);
    return NULL;
  }

  return (xTimer)t;
}

void xTimerDestroy(xTimer t_) {
  struct xTimer_ *t = (struct xTimer_ *)t_;
  if (!t) return;

  pthread_mutex_lock(&t->mu);
  t->stopped = 1;
  pthread_cond_signal(&t->cond);
  pthread_mutex_unlock(&t->mu);

  pthread_join(t->thread, NULL);

  pthread_cond_destroy(&t->cond);
  pthread_mutex_destroy(&t->mu);
  xHeapDestroy(t->heap);
  free(t);
}

static xTimerTask submit(xTimer t_, xTimerFunc fn, void *arg,
                         uint64_t abs_ms) {
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
  /* Wake timer thread in case this is the new earliest deadline */
  pthread_cond_signal(&t->cond);
  pthread_mutex_unlock(&t->mu);

  return (xTimerTask)task;
}

xTimerTask xTimerSubmitAfter(xTimer t, xTimerFunc fn, void *arg,
                              uint64_t delay_ms) {
  return submit(t, fn, arg, xTimerNowMs() + delay_ms);
}

xTimerTask xTimerSubmitAt(xTimer t, xTimerFunc fn, void *arg,
                           uint64_t abs_ms) {
  return submit(t, fn, arg, abs_ms);
}

xErrno xTimerCancel(xTimer t_, xTimerTask task_) {
  struct xTimer_     *t    = (struct xTimer_ *)t_;
  struct xTimerTask_ *task = (struct xTimerTask_ *)task_;
  if (!t || !task) return xErrno_Unknown;

  pthread_mutex_lock(&t->mu);

  if (task->cancelled) {
    pthread_mutex_unlock(&t->mu);
    return xErrno_Unknown; /* already cancelled */
  }

  if (task->heap_idx == TIMER_INVALID_IDX) {
    /* Already popped (fired or being fired) */
    pthread_mutex_unlock(&t->mu);
    return xErrno_Unknown;
  }

  /* Remove from heap and mark cancelled */
  xHeapRemove(t->heap, task->heap_idx);
  task->heap_idx  = TIMER_INVALID_IDX;
  task->cancelled = 1;

  pthread_mutex_unlock(&t->mu);

  free(task);
  return xErrno_Ok;
}
