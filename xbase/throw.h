/*
 * Copyright 2025 The xKit Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * throw.h - Per-thread lightweight error throwing mechanism
 *
 * Provides a thread-local callback-based error reporting channel.
 * Each thread may register its own callback via xThrowSetCallback();
 * when xThrow() is called, the formatted message is dispatched to
 * that callback (or to stderr as a fallback).
 */

#ifndef XBASE_THROW_H
#define XBASE_THROW_H

#include <stdbool.h>
#include <xbase/base.h>

/**
 * @brief Default format buffer size (bytes). Override at compile time
 *        by defining XTHROW_BUF_SIZE before including this header.
 */
#ifndef XTHROW_BUF_SIZE
#define XTHROW_BUF_SIZE 512
#endif

/**
 * @brief Callback type invoked by xThrow().
 * @param msg  Formatted error message (never NULL).
 * @param userdata  User-provided context pointer.
 */
typedef void (*xThrowCallback)(const char *msg, void *userdata);

/**
 * @brief Register (or clear) the current thread's throw callback.
 *
 * Pass NULL for both arguments to clear the callback; subsequent
 * xThrow() calls on this thread will fall back to stderr.
 *
 * @param cb       The callback function, or NULL to clear.
 * @param userdata Opaque pointer forwarded to cb on each invocation.
 */
XCAPI(void) xThrowSetCallback(xThrowCallback cb, void *userdata);

/**
 * @brief Format an error message and dispatch it to the thread's callback.
 *
 * If no callback has been registered for the calling thread, the
 * message is printed to stderr as a fallback.
 * If @p fatal is true, abort() is called after the callback/stderr output.
 *
 * @param fatal If true, call abort() after dispatching the message.
 * @param fmt   printf-style format string (NULL is handled safely).
 * @param ...   Format arguments.
 */
XCAPI(void) xThrow(bool fatal, const char *fmt, ...);

#endif // XBASE_THROW_H
