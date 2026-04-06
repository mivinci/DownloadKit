/*
 * Copyright 2025 The xKit Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * backtrace.h - Platform-adaptive stack backtrace
 *
 * Captures the current call stack and formats it into a human-readable
 * multi-line string.  The actual unwinding backend is selected at build
 * time: libunwind > execinfo > stub.
 */

#ifndef XBASE_BACKTRACE_H
#define XBASE_BACKTRACE_H

#include <stddef.h>
#include <xbase/base.h>

/**
 * @brief Capture the current call stack into @p buf.
 *
 * Automatically skips the frame of xBacktrace() itself so that the
 * output starts from the caller.  Equivalent to xBacktraceSkip(0, buf, size).
 *
 * Each frame is formatted as:
 *   #N 0xADDR symbol+offset
 * or:
 *   #N 0xADDR <unknown>
 *
 * @param buf   Destination buffer (may be NULL).
 * @param size  Size of @p buf in bytes.
 * @return Number of bytes written (excluding the trailing '\\0'),
 *         or 0 if @p buf is NULL or @p size is 0.
 */
XCAPI(int) xBacktrace(char *buf, size_t size);

/**
 * @brief Capture the current call stack, skipping extra frames.
 *
 * The function always skips its own internal frames.  The @p skip
 * parameter specifies how many *additional* frames to skip on top
 * of that (0 means no extra skipping).
 *
 * @param skip  Number of additional frames to skip (>= 0).
 * @param buf   Destination buffer (may be NULL).
 * @param size  Size of @p buf in bytes.
 * @return Number of bytes written (excluding the trailing '\\0'),
 *         or 0 if @p buf is NULL or @p size is 0.
 */
XCAPI(int) xBacktraceSkip(int skip, char *buf, size_t size);

/**
 * @brief Register signal handlers that print a backtrace on crash.
 *
 * Installs handlers for SIGSEGV, SIGABRT, and SIGBUS.  When any of
 * these signals is received the handler prints the signal name and a
 * full stack trace to stderr, then re-raises the signal with the
 * default handler so that a core dump can still be produced.
 *
 * Typical usage — call once at the beginning of main():
 * @code
 *   int main(void) {
 *       xPrintBacktraceOnCrash();
 *       // ...
 *   }
 * @endcode
 */
XCAPI(void) xPrintBacktraceOnCrash(void);

#endif // XBASE_BACKTRACE_H
