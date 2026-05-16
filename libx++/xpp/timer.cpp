/*
 * Copyright 2025 The libx++ Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * timer.cpp - C++ RAII wrapper for xTimer.
 */

#include "timer.h"

namespace xpp {

Result<Timer, xErrno> Timer::create(xTaskGroup group) {
  xTimer h = xTimerCreate(group);
  if (!h) return Result<Timer, xErrno>(err, xErrno_NoMemory);
  return Result<Timer, xErrno>(ok, Timer(h, FromRaw{}));
}

Result<xTimerTask, xErrno> Timer::after(xTimerFunc fn, void *arg, uint64_t delay_ms) {
  xTimerTask t = xTimerSubmitAfter(m_h, fn, arg, delay_ms);
  if (!t) return Result<xTimerTask, xErrno>(err, xErrno_NoMemory);
  return Result<xTimerTask, xErrno>(ok, t);
}

Result<xTimerTask, xErrno> Timer::at(xTimerFunc fn, void *arg, uint64_t abs_ms) {
  xTimerTask t = xTimerSubmitAt(m_h, fn, arg, abs_ms);
  if (!t) return Result<xTimerTask, xErrno>(err, xErrno_NoMemory);
  return Result<xTimerTask, xErrno>(ok, t);
}

xErrno Timer::cancel(xTimerTask task) {
  return xTimerCancel(m_h, task);
}
int Timer::poll() {
  return xTimerPoll(m_h);
}

} // namespace xpp
