/*
 * Copyright 2025 The moo Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * logger.h - Async logging module
 *
 * Provides a high-performance asynchronous logger that formats log entries
 * on the calling thread and flushes them to a file (or stderr) on the
 * event loop thread. Three operating modes are supported: Timer, Notify,
 * and Mixed.
 */

#ifndef XLOG_LOGGER_H
#define XLOG_LOGGER_H

#include <stdint.h>
#include <xbase/base.h>
#include <xbase/error.h>
#include <xbase/event.h>

/**
 * @brief Log severity levels (ascending order).
 */
XDEF_ENUM(xLogLevel){
  xLogLevel_Debug = 0, /**< Verbose debug information */
  xLogLevel_Info,      /**< Informational messages    */
  xLogLevel_Warn,      /**< Warning conditions        */
  xLogLevel_Error,     /**< Error conditions          */
  xLogLevel_Fatal,     /**< Fatal error, abort()      */
};

/**
 * @brief Logger operating modes.
 */
XDEF_ENUM(xLogMode){
  xLogMode_Timer = 0, /**< Periodic timer flush (default)          */
  xLogMode_Notify,    /**< Pipe-based immediate notification       */
  xLogMode_Mixed,     /**< Timer + pipe for high-severity entries  */
};

/**
 * @brief Opaque handle to an async logger instance.
 */
XDEF_HANDLE(xLogger);

/**
 * @brief Configuration for creating a logger.
 */
XDEF_STRUCT(xLoggerConf) {
  xEventLoop  loop;           /**< Event loop (required, must not be NULL) */
  const char *path;           /**< Log file path, or NULL for stderr       */
  xLogMode    mode;           /**< Operating mode (default: Timer)         */
  xLogLevel   level;          /**< Minimum log level (default: Info)       */
  size_t      max_size;       /**< Max file size in bytes before rotation
                                   (0 = no rotation)                       */
  int max_files;              /**< Total files to keep including current
                                   (0 or 1 = no rotation)                 */
  uint64_t flush_interval_ms; /**< Timer/Mixed flush interval in ms
                                   (0 = use default 100ms)                */
};

/**
 * @brief Create an async logger.
 *
 * Opens the log file (if path is specified) in append mode and initialises
 * the internal MPSC queue, timer, and/or pipe according to the chosen mode.
 *
 * @param conf Logger configuration.
 * @return     A logger handle, or NULL on failure.
 */
XCAPI(xLogger) xLoggerCreate(xLoggerConf conf);

/**
 * @brief Destroy a logger and release all resources.
 *
 * Synchronously flushes any remaining log entries before closing the file
 * handle, cancelling timers, and freeing memory. Passing NULL is a no-op.
 *
 * @param logger The logger to destroy.
 */
XCAPI(void) xLoggerDestroy(xLogger logger);

/**
 * @brief Write a log entry at the specified level.
 *
 * The message is formatted on the calling thread and enqueued for async
 * I/O on the event loop thread. Fatal-level messages are written
 * synchronously and followed by abort().
 *
 * Thread-safe: may be called from any thread.
 *
 * @param logger The logger.
 * @param level  Severity level.
 * @param fmt    printf-style format string.
 * @param ...    Format arguments.
 */
XCAPI(void) xLoggerLog(xLogger logger, xLogLevel level, const char *fmt, ...);

/**
 * @brief Synchronously flush all pending log entries to disk.
 *
 * Blocks the calling thread until the event loop thread has written and
 * fflush'd all queued entries. May be called from any thread.
 *
 * @param logger The logger.
 */
XCAPI(void) xLoggerFlush(xLogger logger);

/**
 * @brief Enter a logger context on the current thread.
 *
 * After this call, all XLOG_*() macros and xLog() invocations on the
 * current thread are redirected to the specified logger.
 *
 * @param logger The logger to use as the thread-local context.
 */
XCAPI(void) xLoggerEnter(xLogger logger);

/**
 * @brief Leave the current logger context.
 *
 * Clears the thread-local logger. XLOG_*() macros will silently
 * drop messages until a new context is entered.
 */
XCAPI(void) xLoggerLeave(void);

/**
 * @brief Return the current thread-local logger.
 *
 * Returns the logger set by the most recent xLoggerEnter() on this
 * thread, or NULL if no context is active.
 *
 * @return The current logger, or NULL.
 */
XCAPI(xLogger) xLoggerCurrent(void);

/* ── Convenience macros ── */

/**
 * @brief Default format buffer size for a single log entry (bytes).
 *        Override at compile time by defining XLOG_ENTRY_BUF_SIZE.
 */
#ifndef XLOG_ENTRY_BUF_SIZE
#define XLOG_ENTRY_BUF_SIZE 512
#endif

/* Macros using the thread-local logger context (set via xLoggerEnter). */
#define XLOG_DEBUG(fmt, ...) \
  xLoggerLog(xLoggerCurrent(), xLogLevel_Debug, (fmt), ##__VA_ARGS__)
#define XLOG_INFO(fmt, ...) \
  xLoggerLog(xLoggerCurrent(), xLogLevel_Info, (fmt), ##__VA_ARGS__)
#define XLOG_WARN(fmt, ...) \
  xLoggerLog(xLoggerCurrent(), xLogLevel_Warn, (fmt), ##__VA_ARGS__)
#define XLOG_ERROR(fmt, ...) \
  xLoggerLog(xLoggerCurrent(), xLogLevel_Error, (fmt), ##__VA_ARGS__)
#define XLOG_FATAL(fmt, ...) \
  xLoggerLog(xLoggerCurrent(), xLogLevel_Fatal, (fmt), ##__VA_ARGS__)

/* Explicit-logger variants: pass a specific logger handle. */
#define XLOG_DEBUG_L(logger, fmt, ...) \
  xLoggerLog((logger), xLogLevel_Debug, (fmt), ##__VA_ARGS__)
#define XLOG_INFO_L(logger, fmt, ...) \
  xLoggerLog((logger), xLogLevel_Info, (fmt), ##__VA_ARGS__)
#define XLOG_WARN_L(logger, fmt, ...) \
  xLoggerLog((logger), xLogLevel_Warn, (fmt), ##__VA_ARGS__)
#define XLOG_ERROR_L(logger, fmt, ...) \
  xLoggerLog((logger), xLogLevel_Error, (fmt), ##__VA_ARGS__)
#define XLOG_FATAL_L(logger, fmt, ...) \
  xLoggerLog((logger), xLogLevel_Fatal, (fmt), ##__VA_ARGS__)

#endif /* XLOG_LOGGER_H */
