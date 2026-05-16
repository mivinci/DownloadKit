/*
 * Copyright 2025 The moo Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * task.c - N:M concurrent task model implementation
 *
 * Thread pool with lazy thread creation: threads are spawned on-demand
 * when tasks are submitted and no idle thread is available, up to the
 * configured max. Beyond that, tasks are queued.
 */

#include <x/base/task.h>

#include <x/base/atomic.h>
#include <x/base/mpsc.h>
#include <x/base/note.h>
#include <x/base/slab.h>

#include <stdlib.h>
#include <string.h>
#include "thread_private.h"

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

  /* Task lifecycle state — used for cancel and drain.
   * Type is long for xAtomic* portability (MSVC Interlocked* operates on long). */
  long state;
};

struct xTaskGroup_ {
  xThread *workers;
  size_t     max_threads;
  size_t     nthreads;

  /* Task queue (protected by qlock) */
  xMutex        qlock;
  xCond         qcond;
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

  long pending;    /* submitted - finished (was atomic_size_t) */
  long done_count; /* tasks that have completed (was atomic_size_t) */

  /* Dedicated condition for xTaskGroupWait(), separate from qcond
   * which is shared with idle workers.  Using a single cond caused
   * lost wake-ups: signal could wake an idle worker instead of the
   * GroupWait caller, leaving it blocked forever. */
  xCond wcond;

  bool shutdown;

  /* Task slab pool — replaces per-thread freelist.  Lock-free multi-threaded
   * slab so submit thread and wait thread can differ without pessimising. */
  xSlabMt *task_pool;
};

static inline struct xTaskGroup_ *grp(xTaskGroup g) {
  return (struct xTaskGroup_ *)g;
}

static inline struct xTask_ *tsk(xTask t) {
  return (struct xTask_ *)t;
}

/* Task states for the CAS-based cancel protocol.
 * Transitions:
 *   QUEUED   → RUNNING    (worker picks up)
 *   QUEUED   → CANCELLED  (xTaskCancel succeeds)
 *   RUNNING  → DONE       (worker finishes fn)
 *   CANCELLED stays CANCELLED
 */
enum {
  TASK_QUEUED    = 0,
  TASK_RUNNING   = 1,
  TASK_DONE      = 2,
  TASK_CANCELLED = 3,
};

/* ───────────────── Task allocator (xSlabMt) ────────────── */

static inline struct xTask_ *task_alloc(struct xTaskGroup_ *g) {
  return (struct xTask_ *)xSlabMtAlloc(g->task_pool);
}

static inline void task_free(struct xTaskGroup_ *g, struct xTask_ *t) {
  xSlabMtFree(g->task_pool, t);
}

/* ───────────────────── Worker ───────────────────── */

static void *worker_loop(void *arg) {
  struct xTaskGroup_ *g = grp(arg);

  for (;;) {
    xMutexLock(&g->qlock);

    /* Mark this thread as idle — waiting for work */
    g->idle++;
    while (!g->qhead && !g->shutdown) {
      xCondWait(&g->qcond, &g->qlock);
    }
    g->idle--;

    if (g->shutdown && !g->qhead) {
      xMutexUnlock(&g->qlock);
      return NULL;
    }

    /* Dequeue one task */
    struct xTask_ *task = g->qhead;
    g->qhead            = task->next;
    if (!g->qhead) g->qtail = NULL;
    g->qsize--;

    xMutexUnlock(&g->qlock);

    /* Try to transition QUEUED → RUNNING.  If the task was cancelled
     * between enqueue and here, the CAS fails and we skip execution. */
    long expected = TASK_QUEUED;
    if (xAtomicCasStrong(&task->state, &expected, TASK_RUNNING,
                         xAtomicAcqRel)) {
      /* Execute the task */
      void *result = task->fn(task->arg);
      task->result = result;
      xAtomicStore(&task->state, TASK_DONE, xAtomicRelease);
    }
    /* else: task was cancelled — skip execution, result stays NULL. */

    /* Append to done list (lock-free) BEFORE signaling the note.
     *
     * xMpscPush is wait-free for producers.  The task must be on the
     * done list before anyone can observe completion, so that
     * xTaskGroupDestroy can always find and free it. */
    xMpscPush(&g->done_head, &g->done_tail, &task->done_link);

    /* Signal the note so xTaskWait unblocks. */
    xNoteSignal(&task->note);

    /* Update counters and wake GroupWait if all done.
     * These use group-level atomics, not the task pointer. */
    xAtomicFetchAdd(&g->done_count, 1, xAtomicRelaxed);
    if (xAtomicFetchSub(&g->pending, 1, xAtomicRelaxed) == 1) {
      xMutexLock(&g->qlock);
      xCondSignal(&g->wcond);
      xMutexUnlock(&g->qlock);
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
    task_free(g, t);
  }
}

static bool spawn_one_worker(struct xTaskGroup_ *g) {
  xThread *new_workers;

  if (g->nthreads >= g->max_threads) return false;

  new_workers =
    (xThread *)realloc(g->workers, (g->nthreads + 1) * sizeof(xThread));
  if (!new_workers) return false;

  if (xThreadCreate(&new_workers[g->nthreads], worker_loop, g) != 0) {
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

  xMutexInit(&g->qlock);
  xCondInit(&g->qcond);
  xCondInit(&g->wcond);

  g->task_pool = xSlabMtCreate(sizeof(struct xTask_), 0, 0);
  if (!g->task_pool) {
    xMutexDestroy(&g->qlock);
    xCondDestroy(&g->qcond);
    xCondDestroy(&g->wcond);
    free(g);
    return NULL;
  }

  xAtomicStore(&g->pending, 0, xAtomicSeqCst);
  xAtomicStore(&g->done_count, 0, xAtomicSeqCst);
  g->idle     = 0;
  g->shutdown = false;

  return g;
}

void xTaskGroupDestroy(xTaskGroup g_) {
  struct xTaskGroup_ *g = grp(g_);
  size_t              i;

  if (!g) return;

  xMutexLock(&g->qlock);
  g->shutdown = true;
  xCondBroadcast(&g->qcond); /* wake all idle workers */
  xMutexUnlock(&g->qlock);

  for (i = 0; i < g->nthreads; i++) {
    xThreadJoin(g->workers[i]);
  }

  /* Drain and free any remaining queued tasks */
  while (g->qhead) {
    struct xTask_ *t = g->qhead;
    g->qhead         = t->next;
    xSlabMtFree(g->task_pool, t);
  }

  /* Free completed tasks (both waited and not-waited) */
  drain_done(g);

  free(g->workers);
  xMutexDestroy(&g->qlock);
  xCondDestroy(&g->qcond);
  xCondDestroy(&g->wcond);
  xSlabMtDestroy(g->task_pool);
  free(g);
}

xTask xTaskSubmit(xTaskGroup g_, xTaskFunc fn, void *arg) {
  struct xTaskGroup_ *g = grp(g_);
  struct xTask_      *task;

  if (!g_ || !fn) return NULL;

  task = task_alloc(g);
  if (!task) return NULL;

  task->fn     = fn;
  task->arg    = arg;
  task->note   = (xNote)X_NOTE_INIT;
  task->result = NULL;
  task->group  = g;
  task->next   = NULL;
  xAtomicStore(&task->state, TASK_QUEUED, xAtomicRelaxed);

  xMutexLock(&g->qlock);

  /* Check queue capacity */
  if (g->qcap > 0 && g->qsize >= g->qcap) {
    xMutexUnlock(&g->qlock);
    xSlabMtFree(g->task_pool, task);
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

  xAtomicFetchAdd(&g->pending, 1, xAtomicRelaxed);

  /* Try to dispatch to an idle worker first */
  if (g->idle > 0) {
    xCondSignal(&g->qcond);
    xMutexUnlock(&g->qlock);
    return task;
  }

  /* No idle worker — try to spawn a new one if under the cap */
  if (spawn_one_worker(g)) {
    xCondSignal(&g->qcond);
  }
  /* If at cap, just leave the task in the queue; existing workers
   * or future spawns will pick it up. */

  xMutexUnlock(&g->qlock);
  return task;
}

xErrno xTaskWait(xTask t_, void **result) {
  struct xTask_ *t = tsk(t_);

  if (!t) return xErrno_InvalidArg;

  /* Wait for the worker to signal completion.  In the common
   * event-loop offload path the note is already signaled by the
   * time we get here, so this is a single atomic load. */
  xNoteWait(&t->note);

  long s = xAtomicLoad(&t->state, xAtomicAcquire);
  if (s == TASK_CANCELLED) {
    return xErrno_Cancelled;
  }

  if (result) *result = t->result;
  return xErrno_Ok;
}

xErrno xTaskCancel(xTask t_) {
  struct xTask_ *t = tsk(t_);

  if (!t) return xErrno_InvalidArg;

  /* Try to transition QUEUED → CANCELLED.
   * If the CAS succeeds the task was still queued — the worker will
   * see CANCELLED when it dequeues and skip fn().  The caller may
   * safely release the arg.
   *
   * If the CAS fails the task is already RUNNING or DONE — we
   * return xErrno_Busy so the caller knows fn() is (or was) in
   * flight and must xTaskWait() before releasing the arg. */
  long expected = TASK_QUEUED;
  if (xAtomicCasStrong(&t->state, &expected, TASK_CANCELLED,
                        xAtomicAcqRel)) {
    return xErrno_Ok;
  }

  return xErrno_Busy;
}

xErrno xTaskGroupWait(xTaskGroup g_) {
  struct xTaskGroup_ *g = grp(g_);

  if (!g_) return xErrno_InvalidArg;

  xMutexLock(&g->qlock);
  while (xAtomicLoad(&g->pending, xAtomicAcquire) > 0) {
    xCondWait(&g->wcond, &g->qlock);
  }
  xMutexUnlock(&g->qlock);

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
  return (size_t)xAtomicLoad(&grp(g_)->pending, xAtomicSeqCst);
}

/* ───────────────────── Global task group ───────────────────── */

static xTaskGroup g_global_group = NULL;
static xOnce    g_global_once  = X_ONCE_INIT;

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
  xOnceCall(&g_global_once, global_group_init);
  return g_global_group;
}
