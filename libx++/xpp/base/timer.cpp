/*
 * Copyright 2025 The libx++ Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * timer.cpp - C++ RAII wrapper for xTimer.
 */

#include <xpp/base/timer.h>

namespace xpp {
namespace {

// Bridge a raw xErrno return into Result<void, Error>. xpp::Error is
// C-vocabulary-agnostic, so we cast through int here; the caller can
// still recover the original code via Error::code(). See the same
// pattern in event.cpp / task.cpp.
inline Result<void, Error> from_errno(xErrno rc) {
  if (rc == xErrno_Ok) return Result<void, Error>(ok);
  return Result<void, Error>(err, Error{static_cast<int>(rc)});
}

} // namespace

Result<Timer, Error> Timer::create(xTaskGroup group) {
  xTimer h = xTimerCreate(group);
  if (!h) return Result<Timer, Error>(err, Error{static_cast<int>(xErrno_NoMemory)});
  return Result<Timer, Error>(ok, Timer(h, FromRaw{}));
}

Result<xTimerTask, Error> Timer::after(xTimerFunc fn, void *arg, uint64_t delay_ms) {
  xTimerTask t = xTimerSubmitAfter(m_h, fn, arg, delay_ms);
  if (!t) return Result<xTimerTask, Error>(err, Error{static_cast<int>(xErrno_NoMemory)});
  return Result<xTimerTask, Error>(ok, t);
}

Result<xTimerTask, Error> Timer::at(xTimerFunc fn, void *arg, uint64_t abs_ms) {
  xTimerTask t = xTimerSubmitAt(m_h, fn, arg, abs_ms);
  if (!t) return Result<xTimerTask, Error>(err, Error{static_cast<int>(xErrno_NoMemory)});
  return Result<xTimerTask, Error>(ok, t);
}

Result<void, Error> Timer::cancel(xTimerTask task) {
  return from_errno(xTimerCancel(m_h, task));
}
int Timer::poll() {
  return xTimerPoll(m_h);
}

} // namespace xpp
