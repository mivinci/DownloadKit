/*
 * Copyright 2025 The xKit Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * event_run.c - Backend-independent event loop driver (Plan B)
 */

#include "event_base.h"

void xEventLoopRun(xEventLoop loop_) {
  struct xEventLoop_ *loop = (struct xEventLoop_ *)loop_;
  if (!loop) return;

  xAtomicStore(&loop->stopped, 0, xAtomicRelaxed);

  while (!xAtomicLoad(&loop->stopped, xAtomicAcquire)) {
    xEventWait(loop_, -1);
  }
}

void xEventLoopStop(xEventLoop loop_) {
  struct xEventLoop_ *loop = (struct xEventLoop_ *)loop_;
  if (!loop) return;

  xAtomicStore(&loop->stopped, 1, xAtomicRelease);
  xEventWake(loop_);
}
