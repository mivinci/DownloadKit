/*
 * Copyright 2025 The libx Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * logger_private.h - Internal structures for the async logger
 */

#ifndef XLOG_LOGGER_PRIVATE_H
#define XLOG_LOGGER_PRIVATE_H

#include <x/log/logger.h>

#include <stdio.h>

#include <x/base/mpsc.h>

/* ── Default flush interval (ms) ── */

#define XLOG_DEFAULT_FLUSH_MS 100

/* ── Thread-local freelist size for log entries ── */

#ifndef XLOG_FREELIST_SIZE
#define XLOG_FREELIST_SIZE 64
#endif

/* ── Log entry (queued per message) ── */

struct xLogEntry_ {
  xMpsc     node;                     /**< MPSC queue linkage              */
  xLogLevel level;                    /**< Severity of this entry          */
  int       len;                      /**< Bytes written into buf (excl NUL) */
  char      buf[XLOG_ENTRY_BUF_SIZE]; /**< Pre-formatted message         */
  struct xLogEntry_ *free_next;       /**< Freelist linkage                */
};

/* ── Global lock-free freelist ──
 * Uses a CAS-based lock-free stack shared between producer and consumer
 * threads, so entries freed by the event loop thread can be reused by
 * any producer thread. */

struct xLogFreeList_ {
  struct xLogEntry_ *volatile head;
  volatile int count;
};

/* Global freelist instance (defined in logger.c) */
extern struct xLogFreeList_ g_entry_freelist;

/* ── Logger instance ── */

struct xLogger_ {
  /* Event loop */
  xEventLoop loop;

  /* Output */
  FILE *fp;   /**< Target file, or stderr          */
  char *path; /**< Heap-copied file path, or NULL  */

  /* Mode & level */
  xLogMode  mode;
  xLogLevel level;

  /* MPSC queue (producer: any thread, consumer: loop thread) */
  xMpsc *head;
  xMpsc *tail;

  /* Timer mode fields */
  xEventTimer timer; /**< Active timer handle, or NULL    */
  uint64_t    flush_interval_ms;

  /* Notify / Mixed mode fields */
  int          pipe_rfd; /**< Pipe read end (-1 if unused)    */
  int          pipe_wfd; /**< Pipe write end (-1 if unused)   */
  xEventSource pipe_src; /**< Registered event source         */

  /* File rotation */
  size_t max_size;
  int    max_files;
  size_t written; /**< Bytes written to current file   */

  /* Synchronous flush support */
  int          flush_req_rfd; /**< Flush request pipe read end     */
  int          flush_req_wfd; /**< Flush request pipe write end    */
  xEventSource flush_req_src; /**< Event source for flush request  */
};

/* ── Internal helpers (defined in logger.c) ── */

static inline struct xLogger_ *lgr(xLogger handle) {
  return (struct xLogger_ *)handle;
}

#endif /* XLOG_LOGGER_PRIVATE_H */
