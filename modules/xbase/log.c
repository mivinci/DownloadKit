/*
 * Copyright 2025 The xKit Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * log.c - Per-thread lightweight logging implementation
 */

#include <xbase/log.h>

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <xbase/backtrace.h>

/* ───────────────── Thread-local state ───────────────── */

#define XLOG_BT_SIZE 2048

XDEF_STRUCT(xLogCtx) {
  xLogCallback cb;
  void        *userdata;
  char         buf[XLOG_BUF_SIZE];
  char         bt[XLOG_BT_SIZE];
};

static __thread xLogCtx tl_ctx;

/* ───────────────── Public API ───────────────── */

void xLogSetCallback(xLogCallback cb, void *userdata) {
  tl_ctx.cb       = cb;
  tl_ctx.userdata = userdata;
}

void xLog(bool fatal, const char *fmt, ...) {
  va_list     ap;
  const char *msg;

  if (!fmt) {
    /* Defend against NULL format string */
    msg = "(null)";
  } else {
    va_start(ap, fmt);
    vsnprintf(tl_ctx.buf, sizeof(tl_ctx.buf), fmt, ap);
    va_end(ap);
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
