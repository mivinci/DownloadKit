/*
 * Copyright 2025 The libx++ Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * event.cpp - C++ RAII wrapper for xEventLoop.
 */

#include <xpp/base/event.h>

namespace xpp {
namespace {

// Bridge a raw xErrno return into Result<void, Error>. xErrno_Ok
// becomes Ok; anything else becomes Err(Error{static_cast<int>(rc)}).
// xpp::Error is C-vocabulary-agnostic, so we cast through int here;
// the caller can still recover the original code via Error::code()
// and compare against xErrno_* constants. Inlined so the compiler
// typically folds it into a single compare-and-branch on the
// caller's side. Concentrating the bridging here keeps every other
// site free of the C-side error vocabulary.
inline Result<void, Error> from_errno(xErrno rc) {
  if (rc == xErrno_Ok) return Result<void, Error>(ok);
  return Result<void, Error>(err, Error{static_cast<int>(rc)});
}

} // namespace

Result<EventLoop, Error> EventLoop::create() {
  xEventLoop h = xEventLoopCreate();
  if (!h) return Result<EventLoop, Error>(err, Error{static_cast<int>(xErrno_NoMemory)});
  return Result<EventLoop, Error>(ok, EventLoop(h, FromRaw{}));
}

Result<EventLoop, Error> EventLoop::create(xTaskGroup group) {
  xEventLoop h = xEventLoopCreateWithGroup(group);
  if (!h) return Result<EventLoop, Error>(err, Error{static_cast<int>(xErrno_NoMemory)});
  return Result<EventLoop, Error>(ok, EventLoop(h, FromRaw{}));
}

void EventLoop::run() {
  xEventLoopRun(m_h);
}
void EventLoop::stop() {
  xEventLoopStop(m_h);
}

Result<void, Error> EventLoop::wait(int timeout_ms) {
  return from_errno(xEventLoopWait(m_h, timeout_ms));
}

Result<xEventSource, Error> EventLoop::add(int fd, xEventMask mask, xEventFunc fn, void *arg) {
  xEventSource s = xEventAdd(m_h, fd, mask, fn, arg);
  if (!s) return Result<xEventSource, Error>(err, Error{static_cast<int>(xErrno_Unknown)});
  return Result<xEventSource, Error>(ok, s);
}

Result<void, Error> EventLoop::mod(xEventSource src, xEventMask mask) {
  return from_errno(xEventMod(m_h, src, mask));
}
Result<void, Error> EventLoop::del(xEventSource src) {
  return from_errno(xEventDel(m_h, src));
}
Result<void, Error> EventLoop::wake() {
  return from_errno(xEventWake(m_h));
}

Result<xEventTimer, Error> EventLoop::timerAfter(xEventTimerFunc fn, void *arg,
                                                 uint64_t delay_ms) {
  xEventTimer t = xEventLoopTimerAfter(m_h, fn, arg, delay_ms);
  if (!t) return Result<xEventTimer, Error>(err, Error{static_cast<int>(xErrno_Unknown)});
  return Result<xEventTimer, Error>(ok, t);
}

Result<xEventTimer, Error> EventLoop::timerAt(xEventTimerFunc fn, void *arg, uint64_t abs_ms) {
  xEventTimer t = xEventLoopTimerAt(m_h, fn, arg, abs_ms);
  if (!t) return Result<xEventTimer, Error>(err, Error{static_cast<int>(xErrno_Unknown)});
  return Result<xEventTimer, Error>(ok, t);
}

Result<void, Error> EventLoop::timerCancel(xEventTimer t) {
  return from_errno(xEventLoopTimerCancel(m_h, t));
}

Result<void, Error> EventLoop::submit(xTaskGroup group, xTaskFunc work_fn,
                                      xEventDoneFunc done_fn, void *arg, xEventWork *out) {
  return from_errno(xEventLoopSubmit(m_h, group, work_fn, done_fn, arg, out));
}

Result<void, Error> EventLoop::workCancel(xEventWork w) {
  return from_errno(xEventLoopWorkCancel(m_h, w));
}
Result<void, Error> EventLoop::post(xEventPostFunc fn, void *arg) {
  return from_errno(xEventLoopPost(m_h, fn, arg));
}
Result<void, Error> EventLoop::signalWatch(int signo, xEventSignalFunc fn, void *arg) {
  return from_errno(xEventLoopSignalWatch(m_h, signo, fn, arg));
}

} // namespace xpp
