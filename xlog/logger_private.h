/*
 * Copyright 2025 The xKit Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * logger_private.h - Internal structures for the async logger
 */

#ifndef XLOG_LOGGER_PRIVATE_H
#define XLOG_LOGGER_PRIVATE_H

#include <xlog/logger.h>

#include <stdio.h>

#include <xbase/mpsc.h>

/* ── Default flush interval (ms) ── */

#define XLOG_DEFAULT_FLUSH_MS 1000

/* ── Freelist size for log entries ── */

#ifndef XLOG_FREELIST_SIZE
#define XLOG_FREELIST_SIZE 1024
#endif

/* ── Log entry (queued per message) ── */

struct xLogEntry_ {
  xMpsc    node;                    /**< MPSC queue linkage              */
  xLogLevel level;                  /**< Severity of this entry          */
  int       len;                    /**< Bytes written into buf (excl NUL) */
  char      buf[XLOG_ENTRY_BUF_SIZE]; /**< Pre-formatted message         */
};

/* ── Logger instance ── */

struct xLogger_ {
  /* Event loop */
  xEventLoop loop;

  /* Output */
  FILE       *fp;                   /**< Target file, or stderr          */
  char       *path;                 /**< Heap-copied file path, or NULL  */

  /* Mode & level */
  xLogMode    mode;
  xLogLevel   level;

  /* MPSC queue (producer: any thread, consumer: loop thread) */
  xMpsc      *head;
  xMpsc      *tail;

  /* Entry freelist (for reduced malloc overhead) */
  struct xLogEntry_ *free_list;
  int                free_cnt;   /**< Current free entries */
  int                free_max;   /**< Max entries to keep in freelist */

  /* Timer mode fields */
  xEventTimer timer;                /**< Active timer handle, or NULL    */
  uint64_t    flush_interval_ms;

  /* Notify / Mixed mode fields */
  int         pipe_rfd;             /**< Pipe read end (-1 if unused)    */
  int         pipe_wfd;             /**< Pipe write end (-1 if unused)   */
  xEventSource pipe_src;            /**< Registered event source         */

  /* File rotation */
  size_t      max_size;
  int         max_files;
  size_t      written;              /**< Bytes written to current file   */

  /* Synchronous flush support */
  int         flush_req_rfd;        /**< Flush request pipe read end     */
  int         flush_req_wfd;        /**< Flush request pipe write end    */
  xEventSource flush_req_src;       /**< Event source for flush request  */
};

/* ── Internal helpers (defined in logger.c) ── */

static inline struct xLogger_ *lgr(xLogger handle) {
  return (struct xLogger_ *)handle;
}

#endif /* XLOG_LOGGER_PRIVATE_H */
