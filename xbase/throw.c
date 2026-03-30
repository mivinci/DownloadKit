/*
 * Copyright 2025 The xKit Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * throw.c - Per-thread lightweight error throwing implementation
 */

#include <xbase/throw.h>

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ───────────────── Thread-local state ───────────────── */

typedef struct {
  xThrowCallback cb;
  void          *userdata;
  char           buf[XTHROW_BUF_SIZE];
} xThrowCtx;

static __thread xThrowCtx tl_ctx;

/* ───────────────── Public API ───────────────── */

void xThrowSetCallback(xThrowCallback cb, void *userdata) {
  tl_ctx.cb       = cb;
  tl_ctx.userdata = userdata;
}

void xThrow(bool fatal, const char *fmt, ...) {
  va_list ap;
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

  if (tl_ctx.cb) {
    tl_ctx.cb(msg, tl_ctx.userdata);
  } else {
    fprintf(stderr, "xThrow: %s\n", msg);
  }

  if (fatal) {
    abort();
  }
}
