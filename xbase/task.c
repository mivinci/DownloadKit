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

/* ───────────────────── Internal types ───────────────────── */

struct xTask_ {
  xTaskFunc fn;
  void     *arg;

  /* Completion notification */
  pthread_mutex_t lock;
  pthread_cond_t  cond;
  bool            done;
  void           *result;

  /* Intrusive queue linkage */
  struct xTask_ *next;
};

struct xTaskGroup_ {
  pthread_t *workers;
  size_t     max_threads;
  size_t     nthreads;

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
  size_t idle;

  atomic_size_t pending;    /* submitted - finished */
  atomic_size_t done_count; /* tasks that have completed */

  /* Dedicated condition for xTaskGroupWait(), separate from qcond
   * which is shared with idle workers.  Using a single cond caused
   * lost wake-ups: pthread_cond_signal() could wake an idle worker
   * instead of the GroupWait caller, leaving it blocked forever. */
  pthread_cond_t wcond;

  bool shutdown;
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

  for (;;) {
    pthread_mutex_lock(&g->qlock);

    /* Mark this thread as idle — waiting for work */
    g->idle++;
    while (!g->qhead && !g->shutdown) {
      pthread_cond_wait(&g->qcond, &g->qlock);
    }
    g->idle--;

    if (g->shutdown && !g->qhead) {
      pthread_mutex_unlock(&g->qlock);
      return NULL;
    }

    /* Dequeue one task */
    struct xTask_ *task = g->qhead;
    g->qhead            = task->next;
    if (!g->qhead) g->qtail = NULL;
    g->qsize--;

    pthread_mutex_unlock(&g->qlock);

    /* Execute the task */
    void *result = task->fn(task->arg);

    /* Mark done and wake waiters */
    pthread_mutex_lock(&task->lock);
    task->done   = true;
    task->result = result;
    pthread_cond_broadcast(&task->cond);
    pthread_mutex_unlock(&task->lock);

    /* Update counters and wake GroupWait if all done */
    atomic_fetch_add(&g->done_count, 1);
    if (atomic_fetch_sub(&g->pending, 1) == 1) {
      pthread_mutex_lock(&g->qlock);
      pthread_cond_signal(&g->wcond);
      pthread_mutex_unlock(&g->qlock);
    }
  }
}

/* ───────────────────── Helpers ───────────────────── */

static bool spawn_one_worker(struct xTaskGroup_ *g) {
  pthread_t *new_workers;

  if (g->nthreads >= g->max_threads) return false;

  new_workers =
    (pthread_t *)realloc(g->workers, (g->nthreads + 1) * sizeof(pthread_t));
  if (!new_workers) return false;

  if (pthread_create(&new_workers[g->nthreads], NULL, worker_loop, g) != 0) {
    return false;
  }

  g->workers = new_workers;
  g->nthreads++;
  return true;
}

/* ───────────────────── xTaskGroup API ───────────────────── */

xTaskGroup xTaskGroupCreate(const xTaskGroupConf *conf) {
  struct xTaskGroup_ *g;

  g = (struct xTaskGroup_ *)calloc(1, sizeof(struct xTaskGroup_));
  if (!g) return NULL;

  /* max_threads: 0 means unlimited (no cap) — use a large default cap */
  g->max_threads = (conf && conf->nthreads) ? conf->nthreads : (size_t)-1;
  g->nthreads    = 0;
  g->workers     = NULL;
  g->qcap        = (conf && conf->queue_cap) ? conf->queue_cap : 0;

  pthread_mutex_init(&g->qlock, NULL);
  pthread_cond_init(&g->qcond, NULL);
  pthread_cond_init(&g->wcond, NULL);

  atomic_store(&g->pending, 0);
  atomic_store(&g->done_count, 0);
  g->idle     = 0;
  g->shutdown = false;

  return g;
}

void xTaskGroupDestroy(xTaskGroup g_) {
  struct xTaskGroup_ *g = grp(g_);
  size_t              i;

  if (!g) return;

  pthread_mutex_lock(&g->qlock);
  g->shutdown = true;
  pthread_cond_broadcast(&g->qcond); /* wake all idle workers */
  pthread_mutex_unlock(&g->qlock);

  for (i = 0; i < g->nthreads; i++) {
    pthread_join(g->workers[i], NULL);
  }

  /* Drain and free any remaining queued tasks */
  while (g->qhead) {
    struct xTask_ *t = g->qhead;
    g->qhead         = t->next;
    pthread_mutex_destroy(&t->lock);
    pthread_cond_destroy(&t->cond);
    free(t);
  }

  free(g->workers);
  pthread_mutex_destroy(&g->qlock);
  pthread_cond_destroy(&g->qcond);
  pthread_cond_destroy(&g->wcond);
  free(g);
}

xTask xTaskSubmit(xTaskGroup g_, xTaskFunc fn, void *arg) {
  struct xTaskGroup_ *g = grp(g_);
  struct xTask_      *task;

  if (!g_ || !fn) return NULL;

  task = (struct xTask_ *)calloc(1, sizeof(struct xTask_));
  if (!task) return NULL;

  task->fn     = fn;
  task->arg    = arg;
  task->done   = false;
  task->result = NULL;
  task->next   = NULL;

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

xErrno xTaskWait(xTask t_, void **result) {
  struct xTask_ *t = tsk(t_);

  if (!t) return xErrno_InvalidArg;

  /* The mutex MUST be held before entering pthread_cond_wait().
   * Without this lock, the wait/signal pair races with the worker's
   * broadcast in worker_loop(): the worker may set t->done and call
   * pthread_cond_broadcast() right before we enter pthread_cond_wait(),
   * causing us to block forever on a signal that has already been sent.
   * macOS's pthread implementation happens to tolerate the unlocked case,
   * but glibc (Linux) deadlocks reliably. */
  pthread_mutex_lock(&t->lock);
  while (!t->done) {
    pthread_cond_wait(&t->cond, &t->lock);
  }
  if (result) *result = t->result;
  pthread_mutex_unlock(&t->lock);

  pthread_mutex_destroy(&t->lock);
  pthread_cond_destroy(&t->cond);
  free(t);

  return xErrno_Ok;
}

xErrno xTaskGroupWait(xTaskGroup g_) {
  struct xTaskGroup_ *g = grp(g_);

  if (!g_) return xErrno_InvalidArg;

  pthread_mutex_lock(&g->qlock);
  while (atomic_load(&g->pending) > 0) {
    pthread_cond_wait(&g->wcond, &g->qlock);
  }
  pthread_mutex_unlock(&g->qlock);

  return xErrno_Ok;
}

size_t xTaskGroupThreads(xTaskGroup g_) {
  if (!g_) return 0;
  return grp(g_)->nthreads;
}

size_t xTaskGroupPending(xTaskGroup g_) {
  if (!g_) return 0;
  return atomic_load(&grp(g_)->pending);
}

/* ───────────────────── Global task group ───────────────────── */

static xTaskGroup     g_global_group = NULL;
static pthread_once_t g_global_once  = PTHREAD_ONCE_INIT;

static void global_group_destroy(void) {
  if (g_global_group) {
    xTaskGroupDestroy(g_global_group);
    g_global_group = NULL;
  }
}

static void global_group_init(void) {
  g_global_group = xTaskGroupCreate(NULL);
  if (g_global_group) {
    atexit(global_group_destroy);
  }
}

xTaskGroup xTaskGroupGlobal(void) {
  pthread_once(&g_global_once, global_group_init);
  return g_global_group;
}
