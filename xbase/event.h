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

#include <xbase/base.h>
#include <xbase/error.h>

/**
 * @brief Bitmask of I/O events.
 */
XDEF_ENUM(xEventMask){
  xEvent_Read  = 1 << 0,
  xEvent_Write = 1 << 1,
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
 * @brief Create an event loop.
 * @return A new event loop, or NULL on failure.
 */
XCAPI(xEventLoop) xEventLoopCreate(void);

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

#endif /* XBASE_EVENT_H */
