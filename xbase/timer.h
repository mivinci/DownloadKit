/*
 * Copyright 2025 The xKit Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * timer.h - Monotonic timer with delayed task dispatch
 *
 * Supports two fire modes selected at creation time:
 *
 *   Push mode  (g != NULL): expired callbacks are submitted to the supplied
 *              xTaskGroup for execution on a worker thread.
 *
 *   Poll mode  (g == NULL): expired callbacks are pushed onto an internal
 *              lock-free MPSC queue. The caller drains the queue by calling
 *              xTimerPoll(), which executes all due callbacks on the calling
 *              thread. This avoids any thread-switch overhead and is suitable
 *              for latency-sensitive code that already has an event loop.
 */

#ifndef XBASE_TIMER_H
#define XBASE_TIMER_H

#include <stdint.h>
#include <xbase/base.h>
#include <xbase/error.h>
#include <xbase/task.h>

/**
 * @brief Callback invoked when a timer entry fires.
 * @param arg User-provided argument.
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
 * @param g  Optional task group (selects fire mode):
 *           - Non-NULL → Push mode: expired callbacks are submitted to @p g.
 *           - NULL     → Poll mode: expired callbacks are enqueued internally;
 *             call xTimerPoll() to execute them on the calling thread.
 * @return   A new timer, or NULL on failure.
 */
XCAPI(xTimer) xTimerCreate(xTaskGroup g);

/**
 * @brief Destroy a timer.
 *
 * Signals the background thread to stop and waits for it to exit.
 * Pending entries that have not yet fired are discarded (callbacks NOT called).
 * In poll mode, any entries already in the internal queue are also discarded.
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
 * @param abs_ms  Absolute deadline in milliseconds (CLOCK_MONOTONIC).
 *                Use xTimerNowMs() to obtain the current time.
 * @return        A task handle, or NULL on failure.
 */
XCAPI(xTimerTask) xTimerSubmitAt(xTimer t, xTimerFunc fn, void *arg,
                                 uint64_t abs_ms);

/**
 * @brief Cancel a pending timer entry.
 *
 * Safe to call concurrently with the timer thread. If the entry has already
 * fired the cancel is a no-op and xErrno_Unknown is returned.
 * After a successful cancel the handle must not be used again.
 *
 * @param t     The timer.
 * @param task  Handle returned by xTimerSubmitAfter / xTimerSubmitAt.
 * @return      xErrno_Ok if cancelled before firing, xErrno_Unknown otherwise.
 */
XCAPI(xErrno) xTimerCancel(xTimer t, xTimerTask task);

/**
 * @brief Execute all currently due callbacks (poll mode only).
 *
 * Drains the internal MPSC queue and invokes each expired callback on the
 * calling thread. Returns immediately if the queue is empty.
 *
 * In push mode this function is a no-op and returns 0.
 *
 * @param t The timer.
 * @return  Number of callbacks executed.
 */
XCAPI(int) xTimerPoll(xTimer t);

/**
 * @brief Return the current monotonic time in milliseconds (CLOCK_MONOTONIC).
 * @return Current time in milliseconds.
 */
XCAPI(uint64_t) xTimerNowMs(void);

/**
 * @brief Return the deadline of the earliest pending timer entry.
 *
 * Returns the absolute monotonic time (in milliseconds) of the heap-top
 * entry. If the heap is empty or @p t is NULL, returns UINT64_MAX.
 *
 * Thread-safe: acquires the internal mutex.
 *
 * @param t The timer.
 * @return  Earliest deadline in ms, or UINT64_MAX if none.
 */
XCAPI(uint64_t) xTimerNextDeadline(xTimer t);

/**
 * @brief Check whether a timer is in poll mode.
 *
 * @param t The timer.
 * @return  1 if poll mode (created with g == NULL), 0 otherwise.
 *          Returns 0 if @p t is NULL.
 */
XCAPI(int) xTimerIsPollMode(xTimer t);

#endif /* XBASE_TIMER_H */
