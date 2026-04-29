/*
 * Copyright 2025 The xKit Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * event.h - Cross-platform event loop (edge-triggered)
 *
 * Provides a thin abstraction over OS-specific I/O multiplexing:
 *
 *   kqueue   (macOS / BSD)   — selected when XK_HAS_KQUEUE is defined
 *   epoll    (Linux)         — selected when XK_HAS_EPOLL  is defined
 *   poll     (POSIX fallback)— used when neither of the above is available
 *
 * All backends operate in edge-triggered mode. Callers must drain the fd
 * completely on each notification or re-arm explicitly.
 */

#ifndef XBASE_EVENT_H
#define XBASE_EVENT_H

#include <stdint.h>
#include <xbase/base.h>
#include <xbase/error.h>
#include <xbase/task.h>
#include <xbase/time.h>

/**
 * @brief Bitmask of I/O events.
 */
XDEF_ENUM(xEventMask){
  xEvent_Read = 1 << 0, xEvent_Write = 1 << 1,
  xEvent_Timeout = 1 << 2, /* Used by higher-level modules (e.g., xSocket) */
};

/**
 * @brief Callback invoked when an fd becomes ready.
 * @param fd    The file descriptor that triggered.
 * @param mask  Bitmask of ready events (xEvent_Read / xEvent_Write).
 * @param arg   User-provided argument.
 */
typedef void (*xEventFunc)(int fd, xEventMask mask, void *arg);

/**
 * @brief Opaque handle to an event loop.
 */
XDEF_HANDLE(xEventLoop);

/**
 * @brief Opaque handle to a registered event source.
 *
 * Valid until the source is removed via xEventDel().
 */
XDEF_HANDLE(xEventSource);

/**
 * @brief Opaque handle to a builtin event timer.
 *
 * Returned by xEventLoopTimerAfter / xEventLoopTimerAt.
 * Valid until the timer fires or is cancelled.
 */
XDEF_HANDLE(xEventTimer);

/**
 * @brief Opaque handle to a submitted offload work item.
 *
 * Returned by xEventLoopSubmit() when a non-NULL @p out parameter is
 * provided. Can be passed to xEventLoopWorkCancel() to attempt
 * cancellation.
 */
XDEF_HANDLE(xEventWork);

/**
 * @brief Callback invoked when a builtin event timer fires.
 * @param arg User-provided argument.
 */
typedef void (*xEventTimerFunc)(void *arg);

/**
 * @brief Callback invoked when a watched signal is delivered.
 * @param signo The signal number that was caught.
 * @param arg   User-provided argument.
 */
typedef void (*xEventSignalFunc)(int signo, void *arg);

/**
 * @brief Create an event loop.
 * @return A new event loop, or NULL on failure.
 */
XCAPI(xEventLoop) xEventLoopCreate(void);

/**
 * @brief Create an event loop with a default task group for offloading.
 *
 * When xEventLoopSubmit() is called with a NULL group, the loop will
 * use @p group instead of falling back to xTaskGroupGlobal().
 *
 * @param group  Default task group, or NULL (same as xEventLoopCreate).
 * @return A new event loop, or NULL on failure.
 */
XCAPI(xEventLoop) xEventLoopCreateWithGroup(xTaskGroup group);

/**
 * @brief Destroy an event loop.
 *
 * All registered sources are implicitly removed. The caller is still
 * responsible for closing the underlying file descriptors.
 *
 * @param loop The event loop to destroy.
 */
XCAPI(void) xEventLoopDestroy(xEventLoop loop);

/**
 * @brief Register a file descriptor with the event loop.
 *
 * @param loop  The event loop.
 * @param fd    File descriptor to monitor.
 * @param mask  Events to watch for (xEvent_Read, xEvent_Write, or both).
 * @param fn    Callback invoked when the fd is ready (must not be NULL).
 * @param arg   Argument forwarded to @p fn.
 * @return      An event source handle, or NULL on failure.
 */
XCAPI(xEventSource) xEventAdd(xEventLoop loop, int fd, xEventMask mask,
                              xEventFunc fn, void *arg);

/**
 * @brief Modify the watched events for an existing source.
 *
 * @param loop  The event loop.
 * @param src   Source handle returned by xEventAdd().
 * @param mask  New event mask.
 * @return      xErrno_Ok on success.
 */
XCAPI(xErrno) xEventMod(xEventLoop loop, xEventSource src, xEventMask mask);

/**
 * @brief Remove a registered event source.
 *
 * After this call the source handle is invalid. The underlying fd is NOT
 * closed.
 *
 * @param loop  The event loop.
 * @param src   Source handle to remove.
 * @return      xErrno_Ok on success.
 */
XCAPI(xErrno) xEventDel(xEventLoop loop, xEventSource src);

/**
 * @brief Wait for events and dispatch callbacks.
 *
 * Blocks until at least one event is ready or the timeout expires.
 * Each ready source's callback is invoked on the calling thread.
 *
 * @param loop        The event loop.
 * @param timeout_ms  Maximum time to wait in milliseconds.
 *                    0 = return immediately (poll), -1 = block indefinitely.
 * @return Number of callbacks dispatched, or -1 on error.
 */
XCAPI(int) xEventWait(xEventLoop loop, int timeout_ms);

/**
 * @brief Wake up a blocked xEventWait() from another thread.
 *
 * Safe to call from any thread or signal handler. Multiple wakes before
 * the next xEventWait() are coalesced.
 *
 * @param loop The event loop.
 * @return     xErrno_Ok on success.
 */
XCAPI(xErrno) xEventWake(xEventLoop loop);

/**
 * @brief Schedule a callback to fire after a relative delay.
 *
 * Thread-safe: may be called from any thread. If the loop is blocked in
 * xEventWait(), it will be woken to re-evaluate the timeout.
 *
 * @param loop      The event loop.
 * @param fn        Callback to invoke on expiry (must not be NULL).
 * @param arg       Argument forwarded to @p fn.
 * @param delay_ms  Delay in milliseconds from now.
 * @return          A timer handle, or NULL on failure.
 */
XCAPI(xEventTimer) xEventLoopTimerAfter(xEventLoop loop, xEventTimerFunc fn,
                                        void *arg, uint64_t delay_ms);

/**
 * @brief Schedule a callback to fire at an absolute monotonic time.
 *
 * Thread-safe.
 *
 * @param loop    The event loop.
 * @param fn      Callback to invoke on expiry (must not be NULL).
 * @param arg     Argument forwarded to @p fn.
 * @param abs_ms  Absolute deadline in milliseconds (CLOCK_MONOTONIC).
 * @return        A timer handle, or NULL on failure.
 */
XCAPI(xEventTimer) xEventLoopTimerAt(xEventLoop loop, xEventTimerFunc fn,
                                     void *arg, uint64_t abs_ms);

/**
 * @brief Cancel a pending builtin event timer.
 *
 * Thread-safe.
 *
 * @param loop  The event loop.
 * @param timer Timer handle to cancel.
 * @return      xErrno_Ok if cancelled before firing, xErrno_Unknown otherwise.
 */
XCAPI(xErrno) xEventLoopTimerCancel(xEventLoop loop, xEventTimer timer);

/**
 * @brief Callback invoked on the event loop thread when offloaded work
 *        completes.
 * @param arg     User-provided argument (same as passed to xEventLoopSubmit).
 * @param result  Return value of the work function.
 */
typedef void (*xEventDoneFunc)(void *arg, void *result);

/**
 * @brief Submit work to a thread pool; run @p done_fn on the loop thread
 *        when finished.
 *
 * The @p work_fn is executed on a worker thread from @p group. Once it
 * returns, @p done_fn is queued to the event loop and will be dispatched
 * during the next xEventWait(), serialised with I/O and timer callbacks.
 *
 * @param loop     The event loop (must not be NULL).
 * @param group    Task group (thread pool). NULL = use xTaskGroupGlobal().
 * @param work_fn  Function executed on a worker thread (must not be NULL).
 * @param done_fn  Completion callback on the loop thread, or NULL for
 *                 fire-and-forget.
 * @param arg      Argument forwarded to both @p work_fn and @p done_fn.
 * @param out      If non-NULL, receives an xEventWork handle that can be
 *                 passed to xEventLoopWorkCancel(). May be NULL.
 * @return         xErrno_Ok on success, or an error code.
 */
XCAPI(xErrno) xEventLoopSubmit(xEventLoop loop, xTaskGroup group,
                               xTaskFunc work_fn, xEventDoneFunc done_fn,
                               void *arg, xEventWork *out);

/**
 * @brief Cancel a previously submitted offload work item.
 *
 * If the work function has not yet started on a worker thread, it is
 * cancelled and @p done_fn will NOT be invoked. The caller may safely
 * release the argument after a successful cancel.
 *
 * If the work function is already running or has completed, the cancel
 * fails and xErrno_InvalidState is returned. In that case @p done_fn
 * will still be called normally on the loop thread.
 *
 * Thread-safe: may be called from any thread.
 *
 * @param loop  The event loop (must not be NULL).
 * @param work  Work handle returned by xEventLoopSubmit().
 * @return      xErrno_Ok if cancelled, xErrno_InvalidState if already
 *              running or done, xErrno_InvalidArg if arguments are NULL.
 */
XCAPI(xErrno) xEventLoopWorkCancel(xEventLoop loop, xEventWork work);

/**
 * @brief Callback invoked on the event loop thread by xEventLoopPost().
 * @param arg User-provided argument.
 */
typedef void (*xEventPostFunc)(void *arg);

/**
 * @brief Post a callback to be executed on the event loop thread.
 *
 * The callback is queued and will be dispatched during the next
 * xEventWait(), serialised with I/O, timer, and offload callbacks.
 * Unlike xEventLoopSubmit(), no thread pool is involved — the callback
 * runs directly on the loop thread.
 *
 * Thread-safe: may be called from any thread.
 *
 * @param loop  The event loop (must not be NULL).
 * @param fn    Callback to invoke on the loop thread (must not be NULL).
 * @param arg   Argument forwarded to @p fn.
 * @return      xErrno_Ok on success, or an error code.
 */
XCAPI(xErrno) xEventLoopPost(xEventLoop loop, xEventPostFunc fn, void *arg);

/**
 * @brief Run the event loop.
 *
 * Enters a blocking main loop that repeatedly calls xEventWait() until
 * xEventLoopStop() is called. Builtin timers are automatically dispatched.
 *
 * @param loop The event loop.
 */
XCAPI(void) xEventLoopRun(xEventLoop loop);

/**
 * @brief Stop a running event loop.
 *
 * Sets an internal stop flag and wakes the loop so that xEventLoopRun()
 * or xEventLoopWait() returns promptly. Safe to call from any thread.
 *
 * @param loop The event loop.
 */
XCAPI(void) xEventLoopStop(xEventLoop loop);

/**
 * @brief Run the event loop until stopped or a timeout expires.
 *
 * Like xEventLoopRun(), but with an overall deadline. The loop repeatedly
 * calls xEventWait() until either:
 *   - xEventLoopStop() is called (returns xErrno_Ok), or
 *   - @p timeout_ms elapses (returns xErrno_Timeout).
 *
 * Typical usage in tests:
 * @code
 *   xErrno rc = xEventLoopWait(loop, 5000);
 *   EXPECT_EQ(rc, xErrno_Ok);  // stopped by callback
 * @endcode
 *
 * @param loop        The event loop (must not be NULL).
 * @param timeout_ms  Maximum total wait in milliseconds.
 *                    -1 = block indefinitely (same as xEventLoopRun).
 * @return            xErrno_Ok if stopped, xErrno_Timeout if deadline
 *                    expired, xErrno_InvalidArg if loop is NULL.
 */
XCAPI(xErrno) xEventLoopWait(xEventLoop loop, int timeout_ms);

/**
 * @brief Watch for a POSIX signal on the event loop.
 *
 * Registers a callback to be invoked on the event loop thread when the
 * specified signal is delivered. The callback runs outside of signal
 * context, so it may safely call any function (including xEventLoopStop).
 *
 * - Register:  pass a non-NULL @p fn.
 * - Replace:   call again with the same @p signo and a new @p fn / @p arg.
 * - Cancel:    pass NULL for @p fn (and NULL for @p arg); the signal
 *              disposition is restored to SIG_DFL.
 *
 * @param loop  The event loop (must not be NULL).
 * @param signo Signal number to watch (e.g. SIGUSR1). SIGKILL and SIGSTOP
 *              are rejected.
 * @param fn    Callback, or NULL to cancel.
 * @param arg   Argument forwarded to @p fn.
 * @return      xErrno_Ok on success, xErrno_InvalidArg for bad arguments,
 *              xErrno_SysError if the underlying OS call fails.
 */
XCAPI(xErrno) xEventLoopSignalWatch(xEventLoop loop, int signo,
                                    xEventSignalFunc fn, void *arg);

/**
 * @brief Return the current monotonic time in milliseconds.
 *
 * Convenience wrapper around CLOCK_MONOTONIC.
 *
 * @return Current time in milliseconds.
 * @deprecated Use xMonoMs() from <xbase/time.h> instead.
 */
XCAPI(uint64_t) xEventLoopNowMs(void);

#endif /* XBASE_EVENT_H */
