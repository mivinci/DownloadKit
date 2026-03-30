/*
 * Copyright 2025 The xKit Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * timer.h - Monotonic timer with delayed task dispatch
 *
 * A single background thread manages a min-heap of pending timer entries
 * ordered by deadline. On expiry the callback is either:
 *   - dispatched to a user-supplied xTaskGroup (non-blocking for the timer
 *     thread), or
 *   - called directly on the timer thread when no group is provided (the
 *     callback must be short and non-blocking in that case).
 */

#ifndef XBASE_TIMER_H
#define XBASE_TIMER_H

#include <stdint.h>
#include <xbase/base.h>
#include <xbase/error.h>
#include <xbase/task.h>

/**
 * @brief Callback invoked when a timer entry fires.
 * @param arg User-provided argument passed to xTimerSubmitAfter / xTimerSubmitAt.
 */
typedef void (*xTimerFunc)(void *arg);

/**
 * @brief Opaque handle to a timer instance.
 */
XDEF_HANDLE(xTimer);

/**
 * @brief Opaque handle to a submitted timer entry.
 *
 * Valid until the entry fires or is successfully cancelled.
 * Do NOT use the handle after either event.
 */
XDEF_HANDLE(xTimerTask);

/**
 * @brief Create a timer.
 *
 * Spawns one background thread that manages the internal heap and drives
 * all timers created from this instance.
 *
 * @param g  Optional task group. When non-NULL, expired callbacks are
 *           submitted to @p g for execution on a worker thread.
 *           When NULL, callbacks run directly on the timer thread — they
 *           must be short and must not block.
 * @return   A new timer, or NULL on failure.
 */
XCAPI(xTimer) xTimerCreate(xTaskGroup g);

/**
 * @brief Destroy a timer.
 *
 * Signals the background thread to stop, waits for it to exit, then
 * releases all resources. Pending entries that have not yet fired are
 * discarded (their callbacks will NOT be called).
 *
 * @param t The timer to destroy.
 */
XCAPI(void) xTimerDestroy(xTimer t);

/**
 * @brief Schedule a callback to fire after a relative delay.
 *
 * @param t         The timer.
 * @param fn        Callback to invoke on expiry (must not be NULL).
 * @param arg       Argument forwarded to @p fn.
 * @param delay_ms  Delay in milliseconds from now.
 * @return          A task handle, or NULL on failure.
 */
XCAPI(xTimerTask) xTimerSubmitAfter(xTimer t, xTimerFunc fn, void *arg,
                                    uint64_t delay_ms);

/**
 * @brief Schedule a callback to fire at an absolute monotonic time.
 *
 * @param t       The timer.
 * @param fn      Callback to invoke on expiry (must not be NULL).
 * @param arg     Argument forwarded to @p fn.
 * @param abs_ms  Absolute deadline in milliseconds (CLOCK_MONOTONIC epoch).
 *                Use xTimerNowMs() to obtain the current time in the same
 *                reference frame.
 * @return        A task handle, or NULL on failure.
 */
XCAPI(xTimerTask) xTimerSubmitAt(xTimer t, xTimerFunc fn, void *arg,
                                 uint64_t abs_ms);

/**
 * @brief Cancel a pending timer entry.
 *
 * Safe to call concurrently with the timer thread. If the entry has already
 * fired (or is in the process of firing) the cancel is a no-op and
 * xErrno_Unknown is returned.
 *
 * After a successful cancel the handle must not be used again.
 *
 * @param t     The timer.
 * @param task  The task handle returned by xTimerSubmitAfter / xTimerSubmitAt.
 * @return      xErrno_Ok if cancelled before firing, xErrno_Unknown otherwise.
 */
XCAPI(xErrno) xTimerCancel(xTimer t, xTimerTask task);

/**
 * @brief Return the current monotonic time in milliseconds.
 *
 * Uses CLOCK_MONOTONIC so the value is unaffected by wall-clock adjustments.
 * Suitable as the @p abs_ms argument to xTimerSubmitAt.
 *
 * @return Current time in milliseconds.
 */
XCAPI(uint64_t) xTimerNowMs(void);

#endif /* XBASE_TIMER_H */
