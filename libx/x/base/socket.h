/*
 * Copyright 2025 The libx Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * socket.h - Async socket abstraction over xEventLoop
 */

#ifndef XBASE_SOCKET_H
#define XBASE_SOCKET_H

#include <x/base/base.h>
#include <x/base/error.h>
#include <x/base/event.h>

/* ───────────────────── Types ───────────────────── */

/**
 * @brief Opaque handle to an async socket.
 */
XDEF_HANDLE(xSocket);

/**
 * @brief Callback invoked when a socket event occurs.
 *
 * @param sock  The socket handle that triggered.
 * @param mask  Bitmask of ready events (xEvent_Read / xEvent_Write /
 *              xEvent_Timeout).
 * @param arg   User-provided argument passed to xSocketCreate().
 */
typedef void (*xSocketFunc)(xSocket sock, xEventMask mask, void *arg);

/* ───────────────────── Lifecycle ───────────────────── */

/**
 * @brief Create an async socket and register it with an event loop.
 *
 * Creates a socket via socket(family, type, protocol), sets O_NONBLOCK
 * and FD_CLOEXEC, and registers it with the event loop for the given
 * event mask.
 *
 * @param loop      Event loop to bind to (must not be NULL).
 * @param family    Address family (AF_INET, AF_INET6, AF_UNIX, …).
 * @param type      Socket type (SOCK_STREAM, SOCK_DGRAM, …).
 * @param protocol  Protocol number (usually 0).
 * @param mask      Initial event mask (xEvent_Read, xEvent_Write, or both).
 * @param callback  Callback for I/O and timeout events (must not be NULL).
 * @param userp     User data forwarded to @p callback.
 * @return          A new xSocket handle, or NULL on failure.
 */
XCAPI(xSocket) xSocketCreate(xEventLoop loop, int family, int type,
                             int protocol, xEventMask mask,
                             xSocketFunc callback, void *userp);

/**
 * @brief Create an async socket from an existing file descriptor.
 *
 * Wraps an already-open fd (e.g. from accept()) into an xSocket.
 * The fd is set to O_NONBLOCK and FD_CLOEXEC if not already.
 * Ownership of the fd is transferred to the xSocket; it will be
 * closed when xSocketDestroy() is called.
 *
 * @param loop      Event loop to bind to (must not be NULL).
 * @param fd        An open file descriptor.
 * @param mask      Initial event mask (xEvent_Read, xEvent_Write, or both).
 * @param callback  Callback for I/O and timeout events (must not be NULL).
 * @param userp     User data forwarded to @p callback.
 * @return          A new xSocket handle, or NULL on failure.
 */
XCAPI(xSocket) xSocketCreateFromFd(xEventLoop loop, int fd, xEventMask mask,
                                   xSocketFunc callback, void *userp);

/**
 * @brief Destroy a socket, removing it from the event loop.
 *
 * Cancels any pending timeout timers, removes the event source via
 * xEventDel(), closes the underlying fd, and frees the handle.
 * Safe to call with NULL (no-op).
 *
 * @param loop  The event loop the socket is bound to.
 * @param sock  Socket handle to destroy, or NULL.
 */
XCAPI(void) xSocketDestroy(xEventLoop loop, xSocket sock);

/* ───────────────────── Event mask ───────────────────── */

/**
 * @brief Modify the watched event mask for a socket.
 *
 * @param loop  The event loop.
 * @param sock  Socket handle (must not be NULL).
 * @param mask  New event mask.
 * @return      xErrno_Ok on success, xErrno_InvalidArg if sock is NULL.
 */
XCAPI(xErrno) xSocketSetMask(xEventLoop loop, xSocket sock, xEventMask mask);

/* ───────────────────── Timeout ───────────────────── */

/**
 * @brief Set read/write idle timeouts for a socket.
 *
 * When a timeout fires, the xSocketFunc callback is invoked with
 * xEvent_Timeout in the mask. Normal I/O events reset the corresponding
 * idle timer (idle-timeout semantics).
 *
 * Pass 0 for either parameter to cancel the corresponding timer.
 * Calling this function multiple times replaces previous settings.
 * The event loop bound at xSocketCreate() time is used internally.
 *
 * @param sock              Socket handle (must not be NULL).
 * @param read_timeout_ms   Read idle timeout in milliseconds, or 0 to cancel.
 * @param write_timeout_ms  Write idle timeout in milliseconds, or 0 to cancel.
 * @return                  xErrno_Ok on success, xErrno_InvalidArg if sock
 *                          is NULL.
 */
XCAPI(xErrno) xSocketSetTimeout(xSocket sock, int read_timeout_ms,
                                int write_timeout_ms);

/**
 * @brief Replace the callback and user data for a socket.
 *
 * Allows changing the event handler after creation, e.g. when
 * transferring socket ownership (WebSocket upgrade).
 *
 * @param sock      Socket handle (must not be NULL).
 * @param callback  New callback (must not be NULL).
 * @param userp     New user data.
 * @return          xErrno_Ok on success.
 */
XCAPI(xErrno) xSocketSetCallback(xSocket sock, xSocketFunc callback,
                                 void *userp);

/* ───────────────────── Query ───────────────────── */

/**
 * @brief Return the underlying file descriptor.
 *
 * @param sock  Socket handle, or NULL.
 * @return      The fd, or -1 if sock is NULL.
 */
XCAPI(int) xSocketFd(xSocket sock);

/**
 * @brief Return the current event mask.
 *
 * @param sock  Socket handle, or NULL.
 * @return      The event mask, or 0 if sock is NULL.
 */
XCAPI(xEventMask) xSocketMask(xSocket sock);

#endif /* XBASE_SOCKET_H */
