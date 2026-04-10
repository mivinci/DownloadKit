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

#include <xbase/mpsc.h>
#include <xbase/note.h>

#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* ───────────────────── Internal types ───────────────────── */

struct xTask_ {
  xTaskFunc fn;
  void     *arg;

  /* Completion notification — lightweight one-shot (4 bytes, no destroy). */
  xNote  note;
  void  *result;

  /* Back-pointer to owning group */
  struct xTaskGroup_ *group;

  /* Intrusive queue linkage (task queue) */
  struct xTask_ *next;

  /* Lock-free done-list linkage (xMpsc) */
  xMpsc done_link;

  /* Set by xTaskWait() so drain knows this task was already collected. */
  atomic_bool waited;
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

  /* Completed tasks — lock-free MPSC queue.
   * Workers push via xMpscPush (multi-producer), drain happens on a
   * single thread in xTaskGroupWait / xTaskGroupDestroy. */
  xMpsc *done_head;
  xMpsc *done_tail;

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

/* ───────────────── Thread-local task freelist ────────────── */

/*
 * In the common event-loop offload path, xTaskSubmit (alloc) and
 * xTaskWait (free) happen on the same thread.  A per-thread freelist
 * eliminates malloc/free overhead entirely — zero locks, zero atomics.
 *
 * We reuse task->next as the freelist link pointer (zero extra memory).
 * A per-thread cap prevents unbounded caching when one thread submits
 * many tasks that are waited-on by different threads.
 */
#define TASK_FREELIST_CAP 64

struct task_freelist {
  struct xTask_ *head;
  size_t         count;
};

static __thread struct task_freelist tl_free = {NULL, 0};

static struct xTask_ *task_alloc(void) {
  if (tl_free.head) {
    struct xTask_ *t = tl_free.head;
    tl_free.head     = t->next;
    tl_free.count--;
    return t;
  }
  return (struct xTask_ *)malloc(sizeof(struct xTask_));
}

static void task_free(struct xTask_ *t) {
  if (tl_free.count >= TASK_FREELIST_CAP) {
    free(t);
    return;
  }
  t->next       = tl_free.head;
  tl_free.head  = t;
  tl_free.count++;
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

    /* Append to done list (lock-free) BEFORE signaling the note.
     *
     * xMpscPush is wait-free for producers.  The task must be on the
     * done list before anyone can observe completion, so that
     * xTaskGroupDestroy can always find and free it. */
    xMpscPush(&g->done_head, &g->done_tail, &task->done_link);

    /* Store the result and signal the note. */
    /* After xNoteSignal the caller of xTaskWait may mark the task as
     * waited, but the task memory stays alive until drain. */
    task->result = result;
    xNoteSignal(&task->note);

    /* Update counters and wake GroupWait if all done.
     * These use group-level atomics, not the task pointer. */
    atomic_fetch_add(&g->done_count, 1);
    if (atomic_fetch_sub(&g->pending, 1) == 1) {
      pthread_mutex_lock(&g->qlock);
      pthread_cond_signal(&g->wcond);
      pthread_mutex_unlock(&g->qlock);
    }
  }
}

/* ───────────────────── Helpers ───────────────────── */

/* Drain the done queue, freeing all completed tasks.
 * Must be called from a single thread (no concurrent pop). */
static void drain_done(struct xTaskGroup_ *g) {
  xMpsc *node;
  while ((node = xMpscPop(&g->done_head, &g->done_tail)) != NULL) {
    struct xTask_ *t = xContainerOf(node, struct xTask_, done_link);
    task_free(t);
  }
}

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
    free(t);
  }

  /* Free completed tasks (both waited and not-waited) */
  drain_done(g);

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

  task = task_alloc();
  if (!task) return NULL;

  task->fn     = fn;
  task->arg    = arg;
  task->note   = (xNote)X_NOTE_INIT;
  task->result = NULL;
  task->group  = g;
  task->next   = NULL;
  atomic_store_explicit(&task->waited, false, memory_order_relaxed);

  pthread_mutex_lock(&g->qlock);

  /* Check queue capacity */
  if (g->qcap > 0 && g->qsize >= g->qcap) {
    pthread_mutex_unlock(&g->qlock);
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

  /* Wait for the worker to signal completion.  In the common
   * event-loop offload path the note is already signaled by the
   * time we get here, so this is a single atomic load. */
  xNoteWait(&t->note);
  if (result) *result = t->result;

  /* Mark as waited so the drain in GroupWait/Destroy can free it.
   * We do NOT free the task here — the done-list is a lock-free
   * MPSC queue that does not support random removal.  Memory is
   * reclaimed when the done queue is drained. */
  atomic_store_explicit(&t->waited, true, memory_order_release);

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

  /* All tasks finished — drain the done queue to reclaim memory.
   * No workers are producing into the done queue at this point
   * (pending == 0), so single-consumer drain is safe. */
  drain_done(g);

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
