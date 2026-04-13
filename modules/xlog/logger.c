/*
 * Copyright 2025 The xKit Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * logger.c - Async logging implementation
 *
 * Implements the three operating modes (Timer / Notify / Mixed), log
 * entry formatting, file rotation, synchronous flush, and xbase/log
 * bridging.
 */

#include <xlog/logger.h>

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <xbase/atomic.h>
#include <xbase/log.h>
#include <xbase/mpsc.h>
#include <xbase/time.h>

#include "logger_private.h"

/* ── Level name table ── */

static const char *level_names[] = {"DEBUG", "INFO ", "WARN ", "ERROR",
                                    "FATAL"};

/* ── Forward declarations ── */

static void logger_flush_entries(struct xLogger_ *lg);
static void logger_rotate(struct xLogger_ *lg);
static void logger_timer_cb(void *arg);
static void logger_pipe_cb(int fd, xEventMask mask, void *arg);
static void logger_flush_req_cb(int fd, xEventMask mask, void *arg);
static int  logger_make_pipe(int fds[2]);
static void logger_write_entry(struct xLogger_ *lg, const char *buf, int len);
static void logger_format_timestamp(char *buf, size_t cap);

/* ── Global lock-free entry freelist ── */

/* A CAS-based lock-free stack shared between all producer threads and
 * the consumer (event loop) thread.  This avoids the producer-consumer
 * mismatch that a thread-local freelist would have: entries freed on
 * the event loop thread are visible to any producer thread. */

struct xLogFreeList_ g_entry_freelist = {
  .head  = NULL,
  .count = 0,
};

/* Allocate an entry: pop from global freelist, fallback to malloc. */
static struct xLogEntry_ *entry_alloc(void) {
  struct xLogEntry_ *e = xAtomicLoad(&g_entry_freelist.head, xAtomicAcquire);
  while (e) {
    if (xAtomicCasWeak(&g_entry_freelist.head, &e, e->free_next,
                       xAtomicAcqRel)) {
      xAtomicFetchSub(&g_entry_freelist.count, 1, xAtomicRelaxed);
      return e;
    }
    /* CAS failed, e is reloaded by CAS */
  }
  return (struct xLogEntry_ *)malloc(sizeof(struct xLogEntry_));
}

/* Return entry to global freelist, or free it if the list is full.
 * Note: the count check is intentionally racy (TOCTOU).  This is a
 * soft cap — a few extra entries beyond XLOG_FREELIST_SIZE are harmless
 * and avoiding a CAS loop on the count keeps the fast path lean. */
static void entry_free(struct xLogEntry_ *e) {
  int cnt = xAtomicLoad(&g_entry_freelist.count, xAtomicRelaxed);
  if (cnt >= XLOG_FREELIST_SIZE) {
    free(e);
    return;
  }
  e->free_next = xAtomicLoad(&g_entry_freelist.head, xAtomicRelaxed);
  while (
    !xAtomicCasWeak(&g_entry_freelist.head, &e->free_next, e, xAtomicAcqRel)) {
    /* CAS failed, e->free_next is reloaded */
  }
  xAtomicFetchAdd(&g_entry_freelist.count, 1, xAtomicRelaxed);
}

/* ═══════════════════════════════════════════════════════════════════
 *  Create / Destroy
 * ═══════════════════════════════════════════════════════════════════ */

xLogger xLoggerCreate(xLoggerConf conf) {
  if (!conf.loop) return NULL;

  struct xLogger_ *lg = calloc(1, sizeof(*lg));
  if (!lg) return NULL;

  lg->loop      = conf.loop;
  lg->mode      = conf.mode;
  lg->level     = conf.level;
  lg->max_size  = conf.max_size;
  lg->max_files = conf.max_files;
  lg->flush_interval_ms =
    conf.flush_interval_ms ? conf.flush_interval_ms : XLOG_DEFAULT_FLUSH_MS;
  lg->head          = NULL;
  lg->tail          = NULL;
  lg->timer         = NULL;
  lg->pipe_rfd      = -1;
  lg->pipe_wfd      = -1;
  lg->pipe_src      = NULL;
  lg->flush_req_rfd = -1;
  lg->flush_req_wfd = -1;
  lg->flush_req_src = NULL;

  /* Copy path */
  if (conf.path) {
    lg->path = strdup(conf.path);
    if (!lg->path) goto fail;
  }

  /* Open file or use stderr */
  if (lg->path) {
    lg->fp = fopen(lg->path, "a");
    if (!lg->fp) goto fail;
    /* Recover written offset for rotation */
    fseek(lg->fp, 0, SEEK_END);
    lg->written = (size_t)ftell(lg->fp);
  } else {
    lg->fp      = stderr;
    lg->written = 0;
  }

  /* Timer mode: register periodic timer */
  if (lg->mode == xLogMode_Timer || lg->mode == xLogMode_Mixed) {
    lg->timer = xEventLoopTimerAfter(lg->loop, logger_timer_cb, lg,
                                     lg->flush_interval_ms);
    if (!lg->timer) goto fail;
  }

  /* Notify / Mixed mode: create pipe */
  if (lg->mode == xLogMode_Notify || lg->mode == xLogMode_Mixed) {
    int fds[2];
    if (logger_make_pipe(fds) != 0) goto fail;
    lg->pipe_rfd = fds[0];
    lg->pipe_wfd = fds[1];
    lg->pipe_src =
      xEventAdd(lg->loop, lg->pipe_rfd, xEvent_Read, logger_pipe_cb, lg);
    if (!lg->pipe_src) goto fail;
  }

  /* Flush request pipe (for synchronous flush from any thread) */
  {
    int fds[2];
    if (logger_make_pipe(fds) != 0) goto fail;
    lg->flush_req_rfd = fds[0];
    lg->flush_req_wfd = fds[1];
    lg->flush_req_src = xEventAdd(lg->loop, lg->flush_req_rfd, xEvent_Read,
                                  logger_flush_req_cb, lg);
    if (!lg->flush_req_src) goto fail;
  }

  return (xLogger)lg;

fail:
  if (lg->flush_req_src) xEventDel(lg->loop, lg->flush_req_src);
  if (lg->flush_req_rfd >= 0) close(lg->flush_req_rfd);
  if (lg->flush_req_wfd >= 0) close(lg->flush_req_wfd);
  if (lg->pipe_src) xEventDel(lg->loop, lg->pipe_src);
  if (lg->pipe_rfd >= 0) close(lg->pipe_rfd);
  if (lg->pipe_wfd >= 0) close(lg->pipe_wfd);
  if (lg->timer) xEventLoopTimerCancel(lg->loop, lg->timer);
  if (lg->fp && lg->fp != stderr) fclose(lg->fp);
  free(lg->path);
  free(lg);
  return NULL;
}

void xLoggerDestroy(xLogger logger) {
  if (!logger) return;
  struct xLogger_ *lg = lgr(logger);

  /* Synchronously drain remaining entries */
  logger_flush_entries(lg);
  if (lg->fp && lg->fp != stderr) fflush(lg->fp);

  /* Cancel timer */
  if (lg->timer) {
    xEventLoopTimerCancel(lg->loop, lg->timer);
    lg->timer = NULL;
  }

  /* Remove pipe event source and close pipe */
  if (lg->pipe_src) {
    xEventDel(lg->loop, lg->pipe_src);
    lg->pipe_src = NULL;
  }
  if (lg->pipe_rfd >= 0) {
    close(lg->pipe_rfd);
    lg->pipe_rfd = -1;
  }
  if (lg->pipe_wfd >= 0) {
    close(lg->pipe_wfd);
    lg->pipe_wfd = -1;
  }

  /* Remove flush request pipe */
  if (lg->flush_req_src) {
    xEventDel(lg->loop, lg->flush_req_src);
    lg->flush_req_src = NULL;
  }
  if (lg->flush_req_rfd >= 0) {
    close(lg->flush_req_rfd);
    lg->flush_req_rfd = -1;
  }
  if (lg->flush_req_wfd >= 0) {
    close(lg->flush_req_wfd);
    lg->flush_req_wfd = -1;
  }

  /* Close file */
  if (lg->fp && lg->fp != stderr) fclose(lg->fp);

  free(lg->path);
  free(lg);
}

/* ═══════════════════════════════════════════════════════════════════
 *  Timer mode
 * ═══════════════════════════════════════════════════════════════════ */

static void logger_timer_cb(void *arg) {
  struct xLogger_ *lg = (struct xLogger_ *)arg;

  if (!xMpscEmpty(&lg->head)) {
    logger_flush_entries(lg);
  }

  /* Re-arm timer */
  lg->timer =
    xEventLoopTimerAfter(lg->loop, logger_timer_cb, lg, lg->flush_interval_ms);
}

/* ═══════════════════════════════════════════════════════════════════
 *  Notify mode (pipe)
 * ═══════════════════════════════════════════════════════════════════ */

static void logger_pipe_cb(int fd, xEventMask mask, void *arg) {
  (void)mask;
  struct xLogger_ *lg = (struct xLogger_ *)arg;

  /* Drain pipe */
  char drain[64];
  while (read(fd, drain, sizeof(drain)) > 0) {}

  logger_flush_entries(lg);
}

static void logger_notify(struct xLogger_ *lg) {
  if (lg->pipe_wfd >= 0) {
    char c = 1;
    (void)write(lg->pipe_wfd, &c, 1);
  }
}

/* ═══════════════════════════════════════════════════════════════════
 *  Core logging function
 * ═══════════════════════════════════════════════════════════════════ */

void xLoggerLog(xLogger logger, xLogLevel level, const char *fmt, ...) {
  struct xLogger_ *lg = lgr(logger);
  if (!lg) return;

  /* Level filter */
  if (level < lg->level) return;

  /* Fatal path: synchronous write + abort */
  if (level == xLogLevel_Fatal) {
    char buf[XLOG_ENTRY_BUF_SIZE];
    char ts[32];
    logger_format_timestamp(ts, sizeof(ts));

    va_list ap;
    va_start(ap, fmt);
    int hdr = snprintf(buf, sizeof(buf), "%s %s ", ts, level_names[level]);
    if (hdr < 0) hdr = 0;
    if ((size_t)hdr < sizeof(buf)) {
      vsnprintf(buf + hdr, sizeof(buf) - (size_t)hdr, fmt, ap);
    }
    va_end(ap);

    int total = (int)strlen(buf);
    /* Append newline if room */
    if ((size_t)total + 1 < sizeof(buf)) {
      buf[total]     = '\n';
      buf[total + 1] = '\0';
      total++;
    }

    /* Write directly */
    if (lg->fp) {
      fwrite(buf, 1, (size_t)total, lg->fp);
      fflush(lg->fp);
    }
    abort();
    return; /* unreachable */
  }

  /* Async path: format on calling thread, enqueue */
  struct xLogEntry_ *entry = entry_alloc();
  if (!entry) return; /* drop on OOM */

  entry->level     = level;
  entry->node.next = NULL;

  char ts[32];
  logger_format_timestamp(ts, sizeof(ts));

  va_list ap;
  va_start(ap, fmt);
  int hdr =
    snprintf(entry->buf, sizeof(entry->buf), "%s %s ", ts, level_names[level]);
  if (hdr < 0) hdr = 0;
  if ((size_t)hdr < sizeof(entry->buf)) {
    vsnprintf(entry->buf + hdr, sizeof(entry->buf) - (size_t)hdr, fmt, ap);
  }
  va_end(ap);

  entry->len = (int)strlen(entry->buf);
  /* Append newline if room */
  if ((size_t)entry->len + 1 < sizeof(entry->buf)) {
    entry->buf[entry->len] = '\n';
    entry->len++;
    entry->buf[entry->len] = '\0';
  }

  /* Enqueue */
  xMpscPush(&lg->head, &lg->tail, &entry->node);

  /* Notify based on mode */
  switch (lg->mode) {
  case xLogMode_Notify:
    logger_notify(lg);
    break;
  case xLogMode_Mixed:
    if (level >= xLogLevel_Error) logger_notify(lg);
    break;
  default: /* Timer: no notification */
    break;
  }
}

/* ═══════════════════════════════════════════════════════════════════
 *  Flush entries (runs on event loop thread)
 * ═══════════════════════════════════════════════════════════════════ */

static void logger_flush_entries(struct xLogger_ *lg) {
  xMpsc *node;
  while ((node = xMpscPop(&lg->head, &lg->tail)) != NULL) {
    struct xLogEntry_ *entry = xContainerOf(node, struct xLogEntry_, node);
    logger_write_entry(lg, entry->buf, entry->len);
    entry_free(entry);
  }
  if (lg->fp) fflush(lg->fp);
}

static void logger_write_entry(struct xLogger_ *lg, const char *buf, int len) {
  if (!lg->fp || len <= 0) return;

  size_t n = fwrite(buf, 1, (size_t)len, lg->fp);
  lg->written += n;

  /* Check rotation */
  if (lg->max_size > 0 && lg->max_files > 1 && lg->written >= lg->max_size) {
    logger_rotate(lg);
  }
}

/* ═══════════════════════════════════════════════════════════════════
 *  File rotation
 * ═══════════════════════════════════════════════════════════════════ */

static void logger_rotate(struct xLogger_ *lg) {
  if (!lg->path) return; /* stderr: no rotation */

  fclose(lg->fp);
  lg->fp = NULL;

  /* Stack buffers for rename operations.  The suffix is at most
   * ".<max_files>" which fits comfortably in 16 extra bytes. */
  size_t plen = strlen(lg->path);
  size_t cap  = plen + 16;
  char   old_path[cap];
  char   new_path[cap];

  /* Delete the oldest file: path.{max_files-1} */
  snprintf(old_path, cap, "%s.%d", lg->path, lg->max_files - 1);
  remove(old_path);

  /* Cascade rename: path.{i-1} -> path.{i} */
  for (int i = lg->max_files - 1; i >= 2; i--) {
    snprintf(old_path, cap, "%s.%d", lg->path, i - 1);
    snprintf(new_path, cap, "%s.%d", lg->path, i);
    rename(old_path, new_path);
  }

  /* Rename current -> path.1 */
  snprintf(new_path, cap, "%s.1", lg->path);
  rename(lg->path, new_path);

  /* Reopen */
  lg->fp      = fopen(lg->path, "a");
  lg->written = 0;
}

/* ═══════════════════════════════════════════════════════════════════
 *  Synchronous flush
 * ═══════════════════════════════════════════════════════════════════ */

static void logger_flush_req_cb(int fd, xEventMask mask, void *arg) {
  (void)mask;
  struct xLogger_ *lg = (struct xLogger_ *)arg;

  /* Drain the request pipe */
  char drain[64];
  while (read(fd, drain, sizeof(drain)) > 0) {}

  /* Flush all entries.  The caller (xLoggerFlush) polls xMpscEmpty()
   * to detect completion, so no explicit signal-back is needed. */
  logger_flush_entries(lg);
}

void xLoggerFlush(xLogger logger) {
  if (!logger) return;
  struct xLogger_ *lg = lgr(logger);

  /* Write a byte to the flush request pipe to trigger flush on loop thread */
  if (lg->flush_req_wfd >= 0) {
    char c = 1;
    (void)write(lg->flush_req_wfd, &c, 1);
  }

  /* Busy-wait until the queue is drained.
   * This is acceptable because flush is rare and the loop thread
   * processes entries quickly. */
  for (int i = 0; i < 1000; i++) {
    if (xMpscEmpty(&lg->head)) {
      /* Also ensure fflush has been called */
      return;
    }
    usleep(1000); /* 1ms */
  }
}

/* ═══════════════════════════════════════════════════════════════════
 *  xbase/log bridging
 * ═══════════════════════════════════════════════════════════════════ */

static __thread xLogger tl_logger;

static void bridge_callback(const char *msg, const char *backtrace,
                            void *userdata) {
  (void)backtrace;
  xLogger logger = (xLogger)userdata;
  if (!logger) return;

  /* The message from xLog() is already formatted. We treat it as Info
   * level unless it came from a fatal xLog() call (but fatal calls
   * abort() inside xLog itself, so we just log as Error here). */
  struct xLogger_ *lg = lgr(logger);

  /* Respect the logger's level filter */
  if (xLogLevel_Info < lg->level) return;

  struct xLogEntry_ *entry = entry_alloc();
  if (!entry) return;

  entry->node.next = NULL;
  entry->level     = xLogLevel_Info;

  char ts[32];
  logger_format_timestamp(ts, sizeof(ts));

  int n = snprintf(entry->buf, sizeof(entry->buf), "%s %s %s\n", ts,
                   level_names[xLogLevel_Info], msg);
  if (n < 0) n = 0;
  if ((size_t)n >= sizeof(entry->buf)) n = (int)sizeof(entry->buf) - 1;
  entry->len = n;

  xMpscPush(&lg->head, &lg->tail, &entry->node);

  /* Notify based on mode */
  switch (lg->mode) {
  case xLogMode_Notify:
    logger_notify(lg);
    break;
  case xLogMode_Mixed:
    /* Bridge messages are treated as Info, no urgent notify */
    break;
  default:
    break;
  }
}

void xLoggerEnter(xLogger logger) {
  tl_logger = logger;
  xLogSetCallback(bridge_callback, logger);
}

void xLoggerLeave(void) {
  tl_logger = NULL;
  xLogSetCallback(NULL, NULL);
}

xLogger xLoggerCurrent(void) {
  return tl_logger;
}

/* ═══════════════════════════════════════════════════════════════════
 *  Utility helpers
 * ═══════════════════════════════════════════════════════════════════ */

static int logger_make_pipe(int fds[2]) {
  if (pipe(fds) != 0) return -1;

  /* Set both ends non-blocking */
  for (int i = 0; i < 2; i++) {
    int flags = fcntl(fds[i], F_GETFL, 0);
    if (flags < 0) goto fail;
    if (fcntl(fds[i], F_SETFL, flags | O_NONBLOCK) < 0) goto fail;
  }
  return 0;

fail:
  close(fds[0]);
  close(fds[1]);
  return -1;
}

static void logger_format_timestamp(char *buf, size_t cap) {
  if (cap == 0) return;
  buf[0] = '\0';

  struct timespec ts;
  clock_gettime(CLOCK_REALTIME, &ts);

  struct tm tm;
  localtime_r(&ts.tv_sec, &tm);

  int n = (int)strftime(buf, cap, "%Y-%m-%d %H:%M:%S", &tm);
  if (n > 0 && (size_t)n + 5 <= cap) {
    snprintf(buf + n, cap - (size_t)n, ".%03d", (int)(ts.tv_nsec / 1000000));
  }
}
