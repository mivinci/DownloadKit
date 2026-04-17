/*
 * Copyright 2025 The xKit Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * log.h - Per-thread lightweight logging mechanism
 *
 * Provides a thread-local callback-based error reporting channel.
 * Each thread may register its own callback via xLogSetCallback();
 * when xLog() is called, the formatted message is dispatched to
 * that callback (or to stderr as a fallback).
 */

#ifndef XBASE_LOG_H
#define XBASE_LOG_H

#include <stdbool.h>
#include <xbase/base.h>

/**
 * @brief Default format buffer size (bytes). Override at compile time
 *        by defining XLOG_BUF_SIZE before including this header.
 */
#ifndef XLOG_BUF_SIZE
#define XLOG_BUF_SIZE 512
#endif

/**
 * @brief Callback type invoked by xLog().
 * @param msg        Formatted error message (never NULL).
 * @param backtrace  Stack trace string when fatal is true; NULL otherwise.
 * @param userdata   User-provided context pointer.
 */
typedef void (*xLogCallback)(const char *msg, const char *backtrace,
                             void *userdata);

/**
 * @brief Register (or clear) the current thread's log callback.
 *
 * Pass NULL for both arguments to clear the callback; subsequent
 * xLog() calls on this thread will fall back to stderr.
 *
 * @param cb       The callback function, or NULL to clear.
 * @param userdata Opaque pointer forwarded to cb on each invocation.
 */
XCAPI(void) xLogSetCallback(xLogCallback cb, void *userdata);

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
XCAPI(void) xLog(bool fatal, const char *fmt, ...);

/**
 * @brief Level-based debug logging macros.
 *
 * Controlled by the compile-time integer XK_DEBUG_LEVEL (default 0).
 *
 * XDEBUGL0: Critical debug messages  (shown if XK_DEBUG_LEVEL >= 0)
 * XDEBUGL1: Important debug messages  (shown if XK_DEBUG_LEVEL >= 1)
 * XDEBUGL2: Detailed debug messages   (shown if XK_DEBUG_LEVEL >= 2)
 * XDEBUGL3: Verbose debug messages    (shown if XK_DEBUG_LEVEL >= 3)
 *
 * XDEBUG is an alias for XDEBUGL3 for backward compatibility.
 */
#ifndef XK_DEBUG_LEVEL
#define XK_DEBUG_LEVEL 0
#endif

#if XK_DEBUG_LEVEL >= 0
#define XDEBUGL0(...) xLog(false, __VA_ARGS__)
#else
#define XDEBUGL0(...) ((void)0)
#endif

#if XK_DEBUG_LEVEL >= 1
#define XDEBUGL1(...) xLog(false, __VA_ARGS__)
#else
#define XDEBUGL1(...) ((void)0)
#endif

#if XK_DEBUG_LEVEL >= 2
#define XDEBUGL2(...) xLog(false, __VA_ARGS__)
#else
#define XDEBUGL2(...) ((void)0)
#endif

#if XK_DEBUG_LEVEL >= 3
#define XDEBUGL3(...) xLog(false, __VA_ARGS__)
#else
#define XDEBUGL3(...) ((void)0)
#endif

// Backward compatibility: XDEBUG maps to the most verbose level
#define XDEBUG XDEBUGL3

#endif // XBASE_LOG_H
