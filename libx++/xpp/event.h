/*
 * Copyright 2025 The libx++ Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * event.h - C++ RAII wrapper for xEventLoop.
 */

#ifndef XPP_EVENT_H
#define XPP_EVENT_H

#include <xpp/handle.h>
#include <xpp/result.h>

#include <x/base/event.h>

namespace xpp {

/**
 * @brief RAII wrapper around xEventLoop.
 *
 * Move-only. Destructor calls xEventLoopDestroy which removes all
 * registered sources and releases the handle. The caller is still
 * responsible for closing the underlying file descriptors.
 *
 * Phase 1 mirrors the C callback API directly (xEventFunc, xEventTimerFunc,
 * etc.) for zero overhead. Higher-level std::function/lambda wrappers can
 * be layered on top later.
 */
class EventLoop : public Handle<xEventLoop, xEventLoopDestroy> {
  using Base = Handle<xEventLoop, xEventLoopDestroy>;

public:
  /**
   * @brief Create an event loop.
   * @return Ok(EventLoop) on success, Err(xErrno) on failure.
   */
  static Result<EventLoop, xErrno> create();

  /**
   * @brief Create an event loop with a default task group for offloading.
   *
   * When submit() is called with a NULL group, the loop will use
   * @p group instead of falling back to xTaskGroupGlobal().
   *
   * @param group  Default task group, or NULL (same as create()).
   * @return       Ok(EventLoop) on success, Err(xErrno) on failure.
   */
  static Result<EventLoop, xErrno> create(xTaskGroup group);

  using Base::Base;
  using Base::operator=;

  /**
   * @brief Run the event loop.
   *
   * Enters a blocking main loop that repeatedly dispatches events
   * until stop() is called. Builtin timers are automatically dispatched.
   */
  void run();

  /**
   * @brief Stop a running event loop.
   *
   * Sets an internal stop flag and wakes the loop so that run()
   * or wait() returns promptly. Safe to call from any thread.
   */
  void stop();

  /**
   * @brief Run the event loop until stopped or a timeout expires.
   *
   * Like run(), but with an overall deadline. The loop repeatedly
   * dispatches events until either:
   *   - stop() is called (returns xErrno_Ok), or
   *   - @p timeout_ms elapses (returns xErrno_Timeout).
   *
   * @param timeout_ms  Maximum total wait in milliseconds.
   *                    -1 = block indefinitely (same as run()).
   * @return            xErrno_Ok if stopped, xErrno_Timeout if expired.
   */
  xErrno wait(int timeout_ms = -1);

  /**
   * @brief Register a file descriptor for I/O event monitoring.
   *
   * @param fd    File descriptor to monitor.
   * @param mask  Events to watch for (xEvent_Read, xEvent_Write, or both).
   * @param fn    Callback invoked when the fd is ready (must not be NULL).
   * @param arg   Argument forwarded to @p fn.
   * @return      Ok(xEventSource) on success, Err(xErrno) on failure.
   */
  Result<xEventSource, xErrno> add(int fd, xEventMask mask, xEventFunc fn, void *arg);

  /**
   * @brief Modify the watched events for an existing source.
   *
   * @param src   Source handle returned by add().
   * @param mask  New event mask.
   * @return      xErrno_Ok on success.
   */
  xErrno mod(xEventSource src, xEventMask mask);

  /**
   * @brief Remove a registered event source.
   *
   * After this call the source handle is invalid. The underlying fd
   * is NOT closed.
   *
   * @param src  Source handle to remove.
   * @return     xErrno_Ok on success.
   */
  xErrno del(xEventSource src);

  /**
   * @brief Wake up a blocked event loop from another thread.
   *
   * Safe to call from any thread or signal handler. Multiple wakes
   * before the next dispatch are coalesced.
   *
   * @return xErrno_Ok on success.
   */
  xErrno wake();

  /**
   * @brief Schedule a callback to fire after a relative delay.
   *
   * Thread-safe: may be called from any thread. If the loop is blocked,
   * it will be woken to re-evaluate the timeout.
   *
   * @param fn        Callback to invoke on expiry (must not be NULL).
   * @param arg       Argument forwarded to @p fn.
   * @param delay_ms  Delay in milliseconds from now.
   * @return          Ok(xEventTimer) on success, Err(xErrno) on failure.
   */
  Result<xEventTimer, xErrno> timerAfter(xEventTimerFunc fn, void *arg, uint64_t delay_ms);

  /**
   * @brief Schedule a callback to fire at an absolute monotonic time.
   *
   * Thread-safe.
   *
   * @param fn      Callback to invoke on expiry (must not be NULL).
   * @param arg     Argument forwarded to @p fn.
   * @param abs_ms  Absolute deadline in milliseconds (CLOCK_MONOTONIC).
   * @return        Ok(xEventTimer) on success, Err(xErrno) on failure.
   */
  Result<xEventTimer, xErrno> timerAt(xEventTimerFunc fn, void *arg, uint64_t abs_ms);

  /**
   * @brief Cancel a pending builtin event timer.
   *
   * Thread-safe.
   *
   * @param t  Timer handle to cancel.
   * @return   xErrno_Ok if cancelled before firing, xErrno_Unknown otherwise.
   */
  xErrno timerCancel(xEventTimer t);

  /**
   * @brief Submit work to a thread pool; run @p done_fn on the loop
   *        thread when finished.
   *
   * The @p work_fn is executed on a worker thread from @p group. Once it
   * returns, @p done_fn is queued to the event loop and will be dispatched
   * during the next event cycle, serialised with I/O and timer callbacks.
   *
   * @param group    Task group (thread pool). NULL = use xTaskGroupGlobal().
   * @param work_fn  Function executed on a worker thread (must not be NULL).
   * @param done_fn  Completion callback on the loop thread, or NULL for
   *                 fire-and-forget.
   * @param arg      Argument forwarded to both @p work_fn and @p done_fn.
   * @param out      If non-NULL, receives an xEventWork handle that can be
   *                 passed to workCancel().
   * @return         xErrno_Ok on success, or an error code.
   */
  xErrno submit(xTaskGroup group, xTaskFunc work_fn, xEventDoneFunc done_fn, void *arg,
                xEventWork *out = nullptr);

  /**
   * @brief Cancel a previously submitted offload work item.
   *
   * If the work function has not yet started, it is cancelled and
   * @p done_fn will NOT be invoked. The caller may safely release the
   * argument after a successful cancel.
   *
   * If the work function is already running or has completed, the cancel
   * fails and xErrno_InvalidState is returned. In that case @p done_fn
   * will still be called normally on the loop thread.
   *
   * Thread-safe: may be called from any thread.
   *
   * @param w  Work handle returned by submit().
   * @return   xErrno_Ok if cancelled, xErrno_InvalidState if already
   *           running or done, xErrno_InvalidArg if arguments are NULL.
   */
  xErrno workCancel(xEventWork w);

  /**
   * @brief Post a callback to be executed on the event loop thread.
   *
   * The callback is queued and will be dispatched during the next event
   * cycle, serialised with I/O, timer, and offload callbacks. Unlike
   * submit(), no thread pool is involved — the callback runs directly
   * on the loop thread.
   *
   * Thread-safe: may be called from any thread.
   *
   * @param fn   Callback to invoke on the loop thread (must not be NULL).
   * @param arg  Argument forwarded to @p fn.
   * @return     xErrno_Ok on success, or an error code.
   */
  xErrno post(xEventPostFunc fn, void *arg);

  /**
   * @brief Watch for a POSIX signal on the event loop.
   *
   * Registers a callback to be invoked on the event loop thread when the
   * specified signal is delivered. The callback runs outside of signal
   * context, so it may safely call any function (including stop()).
   *
   *   - Register:  pass a non-NULL @p fn.
   *   - Replace:   call again with the same @p signo and a new @p fn / @p arg.
   *   - Cancel:    pass NULL for @p fn (and NULL for @p arg); the signal
   *                disposition is restored to SIG_DFL.
   *
   * @param signo  Signal number to watch (e.g. SIGUSR1). SIGKILL and
   *               SIGSTOP are rejected.
   * @param fn     Callback, or NULL to cancel.
   * @param arg    Argument forwarded to @p fn.
   * @return       xErrno_Ok on success, xErrno_InvalidArg for bad arguments,
   *               xErrno_SysError if the underlying OS call fails.
   */
  xErrno signalWatch(int signo, xEventSignalFunc fn, void *arg);

private:
  EventLoop(xEventLoop h, FromRaw) noexcept : Base(h, FromRaw{}) {}
};

} // namespace xpp

#endif // XPP_EVENT_H
