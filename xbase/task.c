/*
 * Copyright 2025 The xKit Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * task.c - N:M concurrent task model implementation
 *
 * Thread pool with lazy thread creation: threads are spawned on-demand
 * when tasks are submitted and no idle thread is available, up to the
 * configured max. Beyond that, tasks are queued.
 */

#include <xbase/task.h>

#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <errno.h>
#include <xbase/malloc.h>

/* ───────────────────── Internal types ───────────────────── */

struct xTask_ {
  xTaskFunc       fn;
  void           *arg;

  /* Completion notification */
  pthread_mutex_t lock;
  pthread_cond_t  cond;
  bool            done;
  xErrno          err;

  /* Intrusive queue linkage */
  struct xTask_  *next;
};

struct xTaskGroup_ {
  void           *workers;       /* dynamic array via xAppend, stores pthread_t */
  size_t          max_threads;   /* upper bound from config */
  atomic_size_t   nthreads;      /* current number of live workers */
  unsigned int    idle_timeout_ms; /* idle timeout before worker exits */

  /* Task queue (protected by qlock) */
  pthread_mutex_t qlock;
  pthread_cond_t  qcond;
  struct xTask_  *qhead;
  struct xTask_  *qtail;
  size_t          qsize;
  size_t          qcap;

  /* Idle worker count: workers that have popped a task and finished
   * their work, waiting for more. When a new task arrives and
   * idle > 0, we signal qcond to wake one instead of spawning. */
  size_t          idle;

  atomic_size_t   pending;       /* submitted - finished */
  atomic_size_t   done_count;   /* tasks that have completed */

  bool            shutdown;
};

static inline struct xTaskGroup_ *grp(xTaskGroup g) {
  return (struct xTaskGroup_ *)g;
}

static inline struct xTask_ *tsk(xTask t) {
  return (struct xTask_ *)t;
}

/* ───────────────────── Worker ───────────────────── */

static void *worker_loop(void *arg) {
  struct xTaskGroup_ *g = grp(arg);
  struct timespec     ts;
  bool                timed_out;

  for (;;) {
    pthread_mutex_lock(&g->qlock);

    /* Mark this thread as idle — waiting for work */
    g->idle++;
    timed_out = false;

    while (!g->qhead && !g->shutdown) {
      if (g->idle_timeout_ms > 0) {
        /* Use timed wait with idle timeout */
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_sec  += g->idle_timeout_ms / 1000;
        ts.tv_nsec += (g->idle_timeout_ms % 1000) * 1000000;
        if (ts.tv_nsec >= 1000000000) {
          ts.tv_sec  += 1;
          ts.tv_nsec -= 1000000000;
        }

        int rc = pthread_cond_timedwait(&g->qcond, &g->qlock, &ts);
        if (rc == ETIMEDOUT && !g->qhead && !g->shutdown) {
          timed_out = true;
          break;
        }
      } else {
        pthread_cond_wait(&g->qcond, &g->qlock);
      }
    }
    g->idle--;

    /* Idle timeout: exit this worker thread */
    if (timed_out) {
      pthread_mutex_unlock(&g->qlock);
      atomic_fetch_sub(&g->nthreads, 1);
      return NULL;
    }

    if (g->shutdown && !g->qhead) {
      pthread_mutex_unlock(&g->qlock);
      return NULL;
    }

    /* Dequeue one task */
    struct xTask_ *task = g->qhead;
    g->qhead = task->next;
    if (!g->qhead) g->qtail = NULL;
    g->qsize--;

    pthread_mutex_unlock(&g->qlock);

    /* Execute the task */
    task->fn(task->arg);

    /* Mark done and wake waiters */
    pthread_mutex_lock(&task->lock);
    task->done = true;
    task->err  = xErrno_Ok;
    pthread_cond_broadcast(&task->cond);
    pthread_mutex_unlock(&task->lock);

    /* Update counters and wake GroupWait if all done */
    atomic_fetch_add(&g->done_count, 1);
    if (atomic_fetch_sub(&g->pending, 1) == 1) {
      pthread_mutex_lock(&g->qlock);
      pthread_cond_signal(&g->qcond);
      pthread_mutex_unlock(&g->qlock);
    }
  }
}

/* ───────────────────── Helpers ───────────────────── */

static bool spawn_one_worker(struct xTaskGroup_ *g) {
  pthread_t new_worker;

  if (atomic_load(&g->nthreads) >= g->max_threads) return false;

  if (pthread_create(&new_worker, NULL, worker_loop, g) != 0) {
    return false;
  }

  /* Store the pthread_t in workers array. The array is never shrunk,
   * but that's fine — exited threads are detached and we only join
   * during Destroy. The array may contain stale pthread_t values,
   * but pthread_join on a detached/joined thread safely returns ESRCH. */
  g->workers = xAppend(g->workers, &new_worker, sizeof(pthread_t));
  if (!g->workers) {
    pthread_detach(new_worker);
    return false;
  }

  atomic_fetch_add(&g->nthreads, 1);
  return true;
}

/* ───────────────────── xTaskGroup API ───────────────────── */

xTaskGroup xTaskGroupCreate(const xTaskGroupConf *conf) {
  struct xTaskGroup_ *g;

  g = (struct xTaskGroup_ *)calloc(1, sizeof(struct xTaskGroup_));
  if (!g) return NULL;

  /* max_threads: 0 means unlimited (no cap) — use a large default cap */
  g->max_threads     = (conf && conf->nthreads) ? conf->nthreads : (size_t)-1;
  g->idle_timeout_ms = (conf && conf->idle_timeout_ms) ? conf->idle_timeout_ms : 0;
  atomic_store(&g->nthreads, 0);
  g->workers = NULL;
  g->qcap    = (conf && conf->queue_cap) ? conf->queue_cap : 0;

  pthread_mutex_init(&g->qlock, NULL);
  pthread_cond_init(&g->qcond, NULL);

  atomic_store(&g->pending, 0);
  atomic_store(&g->done_count, 0);
  g->idle     = 0;
  g->shutdown = false;

  return g;
}

void xTaskGroupDestroy(xTaskGroup g_) {
  struct xTaskGroup_ *g = grp(g_);
  size_t              i, n;

  if (!g) return;

  pthread_mutex_lock(&g->qlock);
  g->shutdown = true;
  pthread_cond_broadcast(&g->qcond);  /* wake all idle workers */
  pthread_mutex_unlock(&g->qlock);

  /* Join all workers. Note: some may have already exited due to idle timeout.
   * pthread_join on an already-exited thread returns ESRCH, which we ignore. */
  n = xLen(g->workers) / sizeof(pthread_t);
  for (i = 0; i < n; i++) {
    pthread_join(((pthread_t *)g->workers)[i], NULL);
  }

  /* Drain and free any remaining queued tasks */
  while (g->qhead) {
    struct xTask_ *t = g->qhead;
    g->qhead = t->next;
    pthread_mutex_destroy(&t->lock);
    pthread_cond_destroy(&t->cond);
    free(t);
  }

  xClear(g->workers);
  pthread_mutex_destroy(&g->qlock);
  pthread_cond_destroy(&g->qcond);
  free(g);
}

xTask xTaskSubmit(xTaskGroup g_, xTaskFunc fn, void *arg) {
  struct xTaskGroup_ *g = grp(g_);
  struct xTask_      *task;

  if (!g_ || !fn) return NULL;

  task = (struct xTask_ *)calloc(1, sizeof(struct xTask_));
  if (!task) return NULL;

  task->fn   = fn;
  task->arg  = arg;
  task->done = false;
  task->err  = xErrno_Ok;
  task->next = NULL;

  pthread_mutex_init(&task->lock, NULL);
  pthread_cond_init(&task->cond, NULL);

  pthread_mutex_lock(&g->qlock);

  /* Check queue capacity */
  if (g->qcap > 0 && g->qsize >= g->qcap) {
    pthread_mutex_unlock(&g->qlock);
    pthread_mutex_destroy(&task->lock);
    pthread_cond_destroy(&task->cond);
    free(task);
    return NULL;
  }

  /* Enqueue the task first */
  if (g->qtail) {
    g->qtail->next = task;
  } else {
    g->qhead = task;
  }
  g->qtail = task;
  g->qsize++;

  atomic_fetch_add(&g->pending, 1);

  /* Try to dispatch to an idle worker first */
  if (g->idle > 0) {
    pthread_cond_signal(&g->qcond);
    pthread_mutex_unlock(&g->qlock);
    return task;
  }

  /* No idle worker — try to spawn a new one if under the cap */
  if (spawn_one_worker(g)) {
    pthread_cond_signal(&g->qcond);
  }
  /* If at cap, just leave the task in the queue; existing workers
   * or future spawns will pick it up. */

  pthread_mutex_unlock(&g->qlock);
  return task;
}

xErrno xTaskWait(xTask t_) {
  struct xTask_ *t = tsk(t_);
  xErrno         err;

  if (!t) return xErrno_Unknown;

  pthread_mutex_lock(&t->lock);
  while (!t->done) {
    pthread_cond_wait(&t->cond, &t->lock);
  }
  err = t->err;
  pthread_mutex_unlock(&t->lock);

  pthread_mutex_destroy(&t->lock);
  pthread_cond_destroy(&t->cond);
  free(t);

  return err;
}

xErrno xTaskGroupWait(xTaskGroup g_) {
  struct xTaskGroup_ *g = grp(g_);

  if (!g_) return xErrno_Unknown;

  pthread_mutex_lock(&g->qlock);
  while (atomic_load(&g->pending) > 0) {
    pthread_cond_wait(&g->qcond, &g->qlock);
  }
  pthread_mutex_unlock(&g->qlock);

  return xErrno_Ok;
}

size_t xTaskGroupThreads(xTaskGroup g_) {
  if (!g_) return 0;
  return atomic_load(&grp(g_)->nthreads);
}

size_t xTaskGroupPending(xTaskGroup g_) {
  if (!g_) return 0;
  return atomic_load(&grp(g_)->pending);
}
