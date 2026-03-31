/*
 * Copyright 2025 The xKit Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * event_base.h - Shared base types for event loop backends
 */

#ifndef XBASE_EVENT_BASE_H
#define XBASE_EVENT_BASE_H

#include <xbase/event.h>
#include <xbase/timer.h>

#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* ───────────────────── Source ───────────────────── */

struct xEventSource_ {
  int         fd;
  xEventMask  mask;
  xEventFunc  fn;
  void       *arg;
};

/* ───────────────────── Source list (simple dynamic array) ───────────────── */

struct xEventSources_ {
  struct xEventSource_ **items;
  size_t                 len;
  size_t                 cap;
};

static inline void sources_init(struct xEventSources_ *s) {
  s->items = NULL;
  s->len   = 0;
  s->cap   = 0;
}

static inline void sources_free(struct xEventSources_ *s) {
  for (size_t i = 0; i < s->len; i++)
    free(s->items[i]);
  free(s->items);
  s->items = NULL;
  s->len   = 0;
  s->cap   = 0;
}

static inline struct xEventSource_ *sources_add(struct xEventSources_ *s,
                                                 int fd, xEventMask mask,
                                                 xEventFunc fn, void *arg) {
  if (s->len == s->cap) {
    size_t newcap = s->cap ? s->cap * 2 : 16;
    struct xEventSource_ **tmp = (struct xEventSource_ **)realloc(
        s->items, newcap * sizeof(*s->items));
    if (!tmp) return NULL;
    s->items = tmp;
    s->cap   = newcap;
  }
  struct xEventSource_ *src =
      (struct xEventSource_ *)calloc(1, sizeof(struct xEventSource_));
  if (!src) return NULL;
  src->fd   = fd;
  src->mask = mask;
  src->fn   = fn;
  src->arg  = arg;
  s->items[s->len++] = src;
  return src;
}

static inline int sources_remove(struct xEventSources_ *s,
                                 struct xEventSource_ *src) {
  for (size_t i = 0; i < s->len; i++) {
    if (s->items[i] == src) {
      free(s->items[i]);
      s->items[i] = s->items[--s->len];
      return 0;
    }
  }
  return -1;
}

static inline struct xEventSource_ *sources_find_fd(struct xEventSources_ *s,
                                                     int fd) {
  for (size_t i = 0; i < s->len; i++) {
    if (s->items[i]->fd == fd)
      return s->items[i];
  }
  return NULL;
}

/* ───────────────────── Loop base ───────────────────── */

struct xEventLoop_ {
  struct xEventSources_ sources;
  int                   wake_rfd; /* read end of wake pipe  */
  int                   wake_wfd; /* write end of wake pipe */
  xTimer                timer;    /* attached poll-mode timer, or NULL */
  int                   stopped;  /* set by xEventLoopStop()           */
};

static inline int loop_init_wake(struct xEventLoop_ *loop) {
  int fds[2];
  if (pipe(fds) != 0) return -1;
  loop->wake_rfd = fds[0];
  loop->wake_wfd = fds[1];
  return 0;
}

static inline void loop_close_wake(struct xEventLoop_ *loop) {
  if (loop->wake_rfd >= 0) close(loop->wake_rfd);
  if (loop->wake_wfd >= 0) close(loop->wake_wfd);
}

static inline void loop_drain_wake(struct xEventLoop_ *loop) {
  char buf[64];
  while (read(loop->wake_rfd, buf, sizeof(buf)) > 0)
    ;
}

#endif /* XBASE_EVENT_BASE_H */
