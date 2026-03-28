/*
 * Copyright 2025 The xKit Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * task.c - N:M concurrent task model implementation
 */

#include <xbase/task.h>

#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

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
  pthread_t      *workers;
  size_t          nthreads;

  /* Task queue (MPMC via mutex + cond) */
  pthread_mutex_t qlock;
  pthread_cond_t  qcond;
  struct xTask_  *qhead;
  struct xTask_  *qtail;
  size_t          qsize;
  size_t          qcap;

  /* Number of tasks not yet completed (submitted but not finished) */
  atomic_size_t   pending;

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
  struct xTask_      *task;

  for (;;) {
    pthread_mutex_lock(&g->qlock);

    while (!g->qhead && !g->shutdown) {
      pthread_cond_wait(&g->qcond, &g->qlock);
    }

    if (g->shutdown && !g->qhead) {
      pthread_mutex_unlock(&g->qlock);
      return NULL;
    }

    /* Dequeue */
    task     = g->qhead;
    g->qhead = task->next;
    if (!g->qhead) {
      g->qtail = NULL;
    }
    g->qsize--;
    pthread_mutex_unlock(&g->qlock);

    /* Execute */
    task->fn(task->arg);

    /* Mark done and wake waiters */
    pthread_mutex_lock(&task->lock);
    task->done = true;
    task->err  = xErrno_Ok;
    pthread_cond_broadcast(&task->cond);
    pthread_mutex_unlock(&task->lock);

    /* Decrement pending; wake GroupWait if all done */
    if (atomic_fetch_sub(&g->pending, 1) == 1) {
      pthread_mutex_lock(&g->qlock);
      pthread_cond_signal(&g->qcond);
      pthread_mutex_unlock(&g->qlock);
    }
  }
}

/* ───────────────────── xTaskGroup API ───────────────────── */

xTaskGroup xTaskGroupCreate(const xTaskGroupConf *conf) {
  struct xTaskGroup_ *g;
  size_t              nthreads;

  g = (struct xTaskGroup_ *)malloc(sizeof(struct xTaskGroup_));
  if (!g) return NULL;

  memset(g, 0, sizeof(*g));

  nthreads = (conf && conf->nthreads) ? conf->nthreads : 0;
  if (nthreads == 0) {
    long ncpus = sysconf(_SC_NPROCESSORS_ONLN);
    nthreads   = (ncpus > 0) ? (size_t)ncpus : 4;
  }

  g->nthreads = nthreads;
  g->qcap     = (conf && conf->queue_cap) ? conf->queue_cap : 0;

  pthread_mutex_init(&g->qlock, NULL);
  pthread_cond_init(&g->qcond, NULL);
  atomic_store(&g->pending, 0);
  g->shutdown = false;

  /* Create workers */
  g->workers = (pthread_t *)calloc(nthreads, sizeof(pthread_t));
  if (!g->workers) {
    pthread_mutex_destroy(&g->qlock);
    pthread_cond_destroy(&g->qcond);
    free(g);
    return NULL;
  }

  for (size_t i = 0; i < nthreads; i++) {
    if (pthread_create(&g->workers[i], NULL, worker_loop, g) != 0) {
      /* Cleanup on partial failure */
      g->shutdown = true;
      pthread_cond_broadcast(&g->qcond);
      for (size_t j = 0; j < i; j++) {
        pthread_join(g->workers[j], NULL);
      }
      free(g->workers);
      pthread_mutex_destroy(&g->qlock);
      pthread_cond_destroy(&g->qcond);
      free(g);
      return NULL;
    }
  }

  return g;
}

void xTaskGroupDestroy(xTaskGroup g) {
  struct xTaskGroup_ *ig;

  if (!g) return;

  ig = grp(g);

  xTaskGroupWait(g);

  pthread_mutex_lock(&ig->qlock);
  ig->shutdown = true;
  pthread_cond_broadcast(&ig->qcond);
  pthread_mutex_unlock(&ig->qlock);

  for (size_t i = 0; i < ig->nthreads; i++) {
    pthread_join(ig->workers[i], NULL);
  }

  free(ig->workers);
  pthread_mutex_destroy(&ig->qlock);
  pthread_cond_destroy(&ig->qcond);
  free(ig);
}

xTask xTaskSubmit(xTaskGroup g, xTaskFunc fn, void *arg) {
  struct xTaskGroup_ *ig;
  struct xTask_      *task;

  if (!g || !fn) return NULL;

  ig = grp(g);

  task = (struct xTask_ *)calloc(1, sizeof(struct xTask_));
  if (!task) return NULL;

  task->fn   = fn;
  task->arg  = arg;
  task->done = false;
  task->err  = xErrno_Ok;
  task->next = NULL;

  pthread_mutex_init(&task->lock, NULL);
  pthread_cond_init(&task->cond, NULL);

  pthread_mutex_lock(&ig->qlock);

  /* Check queue capacity */
  if (ig->qcap > 0 && ig->qsize >= ig->qcap) {
    pthread_mutex_unlock(&ig->qlock);
    pthread_mutex_destroy(&task->lock);
    pthread_cond_destroy(&task->cond);
    free(task);
    return NULL;
  }

  /* Enqueue */
  if (ig->qtail) {
    ig->qtail->next = task;
  } else {
    ig->qhead = task;
  }
  ig->qtail = task;
  ig->qsize++;

  atomic_fetch_add(&ig->pending, 1);
  pthread_cond_signal(&ig->qcond);

  pthread_mutex_unlock(&ig->qlock);

  return task;
}

xErrno xTaskWait(xTask t) {
  struct xTask_ *it;
  xErrno         err;

  if (!t) return xErrno_Unknown;

  it = tsk(t);

  pthread_mutex_lock(&it->lock);
  while (!it->done) {
    pthread_cond_wait(&it->cond, &it->lock);
  }
  err = it->err;
  pthread_mutex_unlock(&it->lock);

  pthread_mutex_destroy(&it->lock);
  pthread_cond_destroy(&it->cond);
  free(it);

  return err;
}

xErrno xTaskGroupWait(xTaskGroup g) {
  struct xTaskGroup_ *ig;

  if (!g) return xErrno_Unknown;

  ig = grp(g);

  pthread_mutex_lock(&ig->qlock);
  while (atomic_load(&ig->pending) > 0) {
    pthread_cond_wait(&ig->qcond, &ig->qlock);
  }
  pthread_mutex_unlock(&ig->qlock);

  return xErrno_Ok;
}

size_t xTaskGroupThreads(xTaskGroup g) {
  if (!g) return 0;
  return grp(g)->nthreads;
}

size_t xTaskGroupPending(xTaskGroup g) {
  if (!g) return 0;
  return atomic_load(&grp(g)->pending);
}
