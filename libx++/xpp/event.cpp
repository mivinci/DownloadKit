/*
 * Copyright 2025 The libx++ Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * event.cpp - C++ RAII wrapper for xEventLoop.
 */

#include <xpp/event.h>

namespace xpp {

Result<EventLoop, xErrno> EventLoop::create() {
  xEventLoop h = xEventLoopCreate();
  if (!h) return Result<EventLoop, xErrno>(err, xErrno_NoMemory);
  return Result<EventLoop, xErrno>(ok, EventLoop(h, FromRaw{}));
}

Result<EventLoop, xErrno> EventLoop::create(xTaskGroup group) {
  xEventLoop h = xEventLoopCreateWithGroup(group);
  if (!h) return Result<EventLoop, xErrno>(err, xErrno_NoMemory);
  return Result<EventLoop, xErrno>(ok, EventLoop(h, FromRaw{}));
}

void EventLoop::run() {
  xEventLoopRun(m_h);
}
void EventLoop::stop() {
  xEventLoopStop(m_h);
}

xErrno EventLoop::wait(int timeout_ms) {
  return xEventLoopWait(m_h, timeout_ms);
}

Result<xEventSource, xErrno> EventLoop::add(int fd, xEventMask mask, xEventFunc fn, void *arg) {
  xEventSource s = xEventAdd(m_h, fd, mask, fn, arg);
  if (!s) return Result<xEventSource, xErrno>(err, xErrno_Unknown);
  return Result<xEventSource, xErrno>(ok, s);
}

xErrno EventLoop::mod(xEventSource src, xEventMask mask) {
  return xEventMod(m_h, src, mask);
}
xErrno EventLoop::del(xEventSource src) {
  return xEventDel(m_h, src);
}
xErrno EventLoop::wake() {
  return xEventWake(m_h);
}

Result<xEventTimer, xErrno> EventLoop::timerAfter(xEventTimerFunc fn, void *arg,
                                                  uint64_t delay_ms) {
  xEventTimer t = xEventLoopTimerAfter(m_h, fn, arg, delay_ms);
  if (!t) return Result<xEventTimer, xErrno>(err, xErrno_Unknown);
  return Result<xEventTimer, xErrno>(ok, t);
}

Result<xEventTimer, xErrno> EventLoop::timerAt(xEventTimerFunc fn, void *arg, uint64_t abs_ms) {
  xEventTimer t = xEventLoopTimerAt(m_h, fn, arg, abs_ms);
  if (!t) return Result<xEventTimer, xErrno>(err, xErrno_Unknown);
  return Result<xEventTimer, xErrno>(ok, t);
}

xErrno EventLoop::timerCancel(xEventTimer t) {
  return xEventLoopTimerCancel(m_h, t);
}

xErrno EventLoop::submit(xTaskGroup group, xTaskFunc work_fn, xEventDoneFunc done_fn, void *arg,
                         xEventWork *out) {
  return xEventLoopSubmit(m_h, group, work_fn, done_fn, arg, out);
}

xErrno EventLoop::workCancel(xEventWork w) {
  return xEventLoopWorkCancel(m_h, w);
}
xErrno EventLoop::post(xEventPostFunc fn, void *arg) {
  return xEventLoopPost(m_h, fn, arg);
}
xErrno EventLoop::signalWatch(int signo, xEventSignalFunc fn, void *arg) {
  return xEventLoopSignalWatch(m_h, signo, fn, arg);
}

} // namespace xpp
