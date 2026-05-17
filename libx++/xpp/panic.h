/*
 * Copyright 2025 The libx++ Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * panic.h - Fatal-error mechanism for libx++.
 *
 * Provides XPP_PANIC / XPP_ASSERT / XPP_DEBUG_ASSERT macros for reporting
 * unrecoverable contract violations (e.g. unwrap() on a None Option).
 *
 * Panics print the message and terminate the process. Where the panic
 * is routed (stderr only, an installed log callback, with or without a
 * backtrace) is the linked panic implementation's choice — see
 * panic.cpp for the current routing. The header itself stays free of
 * any logging-library dependency so consumers don't transitively
 * acquire one from a primitive type's `unwrap()` call site.
 *
 * For recoverable errors, use Result<T, E> instead — panics are for bugs,
 * not for runtime conditions the caller is expected to handle.
 *
 * C++11-compatible.
 */

#ifndef XPP_PANIC_H
#define XPP_PANIC_H

#include <xpp/compiler.h>

namespace xpp {
namespace _ {

/**
 * @brief Dispatch a panic message and terminate the process.
 *
 * Defined out-of-line in panic.cpp so the routing dependency (today
 * the libx xLog channel) doesn't leak into every TU that uses
 * XPP_PANIC / XPP_ASSERT through a header. Never returns.
 *
 * Prefer the XPP_PANIC / XPP_ASSERT macros over calling this directly;
 * they prepend a "panic at __FILE__:__LINE__: " prefix to the format
 * string at the call site, so file/line capture happens for free.
 *
 * The printf-format attribute (where supported) lets the compiler
 * type-check the fmt/args pairing at every macro use site.
 */
XPP_NORETURN
#if defined(__GNUC__) || defined(__clang__)
__attribute__((format(printf, 1, 2)))
#endif
void do_panic(const char *fmt, ...);

} // namespace _
} // namespace xpp

/**
 * @brief Unconditionally panic with a printf-style message.
 *
 * The format string is prefixed with "panic at <file>:<line>: " at the
 * call site, so the final output looks like:
 *   panic at foo.cpp:42: idx 7 out of range (size=4)
 *
 * Usage:
 *   if (bad_state) XPP_PANIC("invariant X violated");
 *   XPP_PANIC("idx %zu out of range (size=%zu)", idx, size);
 */
#define XPP_PANIC(fmt, ...) \
  ::xpp::_::do_panic("panic at %s:%d: " fmt, __FILE__, __LINE__, ##__VA_ARGS__)

/**
 * @brief Runtime assertion. Panics if @p cond is false.
 *
 * Always evaluated, even in release builds. Use for public-API contract
 * checks whose failure must not be silently ignored (e.g. Option::unwrap).
 *
 * Accepts printf-style fmt + args; the expanded message includes the
 * stringified condition, so panic output looks like:
 *   panic at foo.cpp:42: assertion failed: idx < size — idx=7 size=4
 *
 * Usage:
 *   XPP_ASSERT(idx < size, "index out of range");
 *   XPP_ASSERT(idx < size, "idx=%zu size=%zu", idx, size);
 */
#define XPP_ASSERT(cond, fmt, ...)                                                           \
  do {                                                                                       \
    if (XPP_UNLIKELY(!(cond)))                                                               \
      ::xpp::_::do_panic("panic at %s:%d: assertion failed: " #cond " \u2014 " fmt, __FILE__, \
                        __LINE__, ##__VA_ARGS__);                                            \
  } while (0)

/**
 * @brief Debug-only assertion. Compiled out when NDEBUG is defined.
 *
 * Use for internal invariants on hot paths, or for *Unchecked() APIs
 * where the caller has already promised the precondition.
 *
 * Usage:
 *   XPP_DEBUG_ASSERT(m_has_value, "internal: Option storage uninitialized");
 *   XPP_DEBUG_ASSERT(idx < size, "idx=%zu size=%zu", idx, size);
 */
#ifdef NDEBUG
#define XPP_DEBUG_ASSERT(cond, fmt, ...) ((void)0)
#else
#define XPP_DEBUG_ASSERT(cond, fmt, ...) XPP_ASSERT(cond, fmt, ##__VA_ARGS__)
#endif

#endif // XPP_PANIC_H
