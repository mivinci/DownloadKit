/*
 * Copyright 2025 The libx Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * log.c - Per-thread lightweight logging implementation
 */

#include <x/base/log.h>

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <x/base/backtrace.h>

/* ───────────────── Thread-local state ───────────────── */

#define XLOG_BT_SIZE 2048

XDEF_STRUCT(xLogCtx) {
  xLogCallback cb;
  void        *userdata;
  char         buf[XLOG_BUF_SIZE];
  char         bt[XLOG_BT_SIZE];
};

#ifdef _WIN32
static __declspec(thread) xLogCtx tl_ctx;
static __declspec(thread) bool    tl_in_fatal = false;
#else
static __thread xLogCtx tl_ctx;
static __thread bool    tl_in_fatal = false;
#endif

/* ───────────────── Public API ───────────────── */

void xLogSetCallback(xLogCallback cb, void *userdata) {
  tl_ctx.cb       = cb;
  tl_ctx.userdata = userdata;
}

void xLog(bool fatal, const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  xLogV(fatal, fmt, ap);
  va_end(ap);
}

void xLogV(bool fatal, const char *fmt, va_list ap) {
  const char *msg;

  /* Recursion guard: a fatal xLog must terminate the process. If the
   * registered callback re-enters xLog(fatal=true), or a signal handler
   * fires another fatal log mid-abort, skip the callback path and abort
   * immediately to avoid unbounded recursion / stack overflow. */
  if (fatal && tl_in_fatal) {
    fprintf(stderr, "xLog: recursive fatal — aborting\n");
    abort();
  }
  if (fatal) tl_in_fatal = true;

  if (!fmt) {
    /* Defend against NULL format string */
    msg = "(null)";
  } else {
    vsnprintf(tl_ctx.buf, sizeof(tl_ctx.buf), fmt, ap);
    msg = tl_ctx.buf;
  }

  const char *bt_str = NULL;
  if (fatal) {
    int n = xBacktraceSkip(2, tl_ctx.bt, sizeof(tl_ctx.bt));
    if (n > 0) {
      bt_str = tl_ctx.bt;
    }
  }

  if (tl_ctx.cb) {
    tl_ctx.cb(msg, bt_str, tl_ctx.userdata);
  } else {
    fprintf(stderr, "%s\n", msg);
    if (bt_str) {
      fprintf(stderr, "xLog fatal backtrace:\n%s", bt_str);
    }
  }

  if (fatal) {
    abort();
  }
}
