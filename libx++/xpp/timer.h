/*
 * Copyright 2025 The libx++ Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * timer.h - C++ RAII wrapper for xTimer.
 */

#ifndef XPP_TIMER_H
#define XPP_TIMER_H

#include <xpp/handle.h>
#include <xpp/result.h>

#include <x/base/timer.h>

#include <cstdint>

namespace xpp {

/**
 * @brief RAII wrapper around xTimer.
 *
 * Move-only. Destructor calls xTimerDestroy which signals the background
 * timer thread to stop, discards all pending entries, and releases the handle.
 */
class Timer : public Handle<xTimer, xTimerDestroy> {
  using Base = Handle<xTimer, xTimerDestroy>;

public:
  /**
   * @brief Create a timer.
   *
   * Spawns one background thread that manages the internal heap and
   * drives all timers created from this instance.
   *
   * @param group  Optional task group (selects fire mode):
   *               - Non-NULL → Push mode: expired callbacks are submitted to @p group.
   *               - NULL     → Poll mode: expired callbacks are enqueued internally;
   *                 call poll() to execute them on the calling thread.
   * @return       Ok(Timer) on success, Err(xErrno) on failure.
   */
  static Result<Timer, xErrno> create(xTaskGroup group = nullptr);

  using Base::Base;
  using Base::operator=;

  /**
   * @brief Schedule a callback to fire after a relative delay.
   *
   * @param fn        Callback to invoke on expiry (must not be NULL).
   * @param arg       Argument forwarded to @p fn.
   * @param delay_ms  Delay in milliseconds from now.
   * @return          Ok(xTimerTask) on success, Err(xErrno) on failure.
   */
  Result<xTimerTask, xErrno> after(xTimerFunc fn, void *arg, uint64_t delay_ms);

  /**
   * @brief Schedule a callback to fire at an absolute monotonic time.
   *
   * @param fn      Callback to invoke on expiry (must not be NULL).
   * @param arg     Argument forwarded to @p fn.
   * @param abs_ms  Absolute deadline in milliseconds (CLOCK_MONOTONIC).
   * @return        Ok(xTimerTask) on success, Err(xErrno) on failure.
   */
  Result<xTimerTask, xErrno> at(xTimerFunc fn, void *arg, uint64_t abs_ms);

  /**
   * @brief Cancel a pending timer entry.
   *
   * Safe to call concurrently with the timer thread. If the entry has
   * already fired the cancel is a no-op and xErrno_Unknown is returned.
   * After a successful cancel the handle must not be used again.
   *
   * @param task  Handle returned by after() / at().
   * @return      xErrno_Ok if cancelled before firing, xErrno_Unknown otherwise.
   */
  xErrno cancel(xTimerTask task);

  /**
   * @brief Execute all currently due callbacks (poll mode only).
   *
   * Drains the internal MPSC queue and invokes each expired callback on
   * the calling thread. Returns immediately if the queue is empty.
   * In push mode this function is a no-op and returns 0.
   *
   * @return Number of callbacks executed.
   */
  int poll();

private:
  Timer(xTimer h, FromRaw) noexcept : Base(h, FromRaw{}) {}
};

} // namespace xpp

#endif // XPP_TIMER_H
