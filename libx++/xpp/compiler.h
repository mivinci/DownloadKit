/*
 * Copyright 2025 The libx++ Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * compiler.h - Portable compiler-specific attribute and intrinsic macros.
 *
 * Wraps non-portable extensions (__builtin_*, __attribute__, __declspec)
 * behind XPP_-prefixed macros that degrade gracefully on toolchains that
 * lack them. Keep this header self-contained — no project dependencies —
 * so it can be included from anywhere in libxpp.
 */

#ifndef XPP_COMPILER_H
#define XPP_COMPILER_H

/**
 * @brief Branch-prediction hint: the expression is expected to be false.
 *
 * Wraps __builtin_expect on GCC/Clang to mark cold paths (e.g. assertion
 * failure branches). Falls back to a plain expression elsewhere.
 */
#if defined(__GNUC__) || defined(__clang__)
#define XPP_UNLIKELY(x) (__builtin_expect(!!(x), 0))
#else
#define XPP_UNLIKELY(x) (x)
#endif

/**
 * @brief Branch-prediction hint: the expression is expected to be true.
 *
 * Symmetric counterpart to XPP_UNLIKELY. Use sparingly — modern branch
 * predictors and PGO usually do a better job than hand-annotated hints,
 * so reserve this for paths where the bias is obvious and stable.
 */
#if defined(__GNUC__) || defined(__clang__)
#define XPP_LIKELY(x) (__builtin_expect(!!(x), 1))
#else
#define XPP_LIKELY(x) (x)
#endif

/**
 * @brief Mark a function as never returning.
 *
 * Prefers C++11's standard [[noreturn]] attribute. Falls back to
 * compiler-specific extensions on toolchains that reject it in some
 * positions (older MSVC pre-VS2019).
 */
#if defined(__cplusplus) && __cplusplus >= 201103L
#define XPP_NORETURN [[noreturn]]
#elif defined(__GNUC__) || defined(__clang__)
#define XPP_NORETURN __attribute__((noreturn))
#elif defined(_MSC_VER)
#define XPP_NORETURN __declspec(noreturn)
#else
#define XPP_NORETURN
#endif

/**
 * @brief Mark a control-flow point as unreachable.
 *
 * Tells the optimizer the path can never execute, enabling dead-code
 * elimination and tighter codegen (e.g. omitting a default branch in a
 * fully-covered switch). Triggering it at runtime is undefined behavior;
 * use XPP_PANIC if you need a checked terminator.
 *
 * The fallback calls std::abort() to keep behavior defined on toolchains
 * lacking an intrinsic — slightly worse codegen but safer than UB.
 */
#if defined(__GNUC__) || defined(__clang__)
#define XPP_UNREACHABLE() (__builtin_unreachable())
#elif defined(_MSC_VER)
#define XPP_UNREACHABLE() (__assume(0))
#else
#include <cstdlib>
#define XPP_UNREACHABLE() (std::abort())
#endif

/**
 * @brief Suppress -Wimplicit-fallthrough on an intentional switch fallthrough.
 *
 * Prefers C++17's [[fallthrough]] attribute. Falls back to GCC/Clang's
 * statement attribute on older C++ standards. Place immediately before
 * the next case label, with a trailing semicolon:
 *
 *   case 1:
 *     do_thing();
 *     XPP_FALLTHROUGH;
 *   case 2:
 *     ...
 */
#if defined(__cplusplus) && __cplusplus >= 201703L
#define XPP_FALLTHROUGH [[fallthrough]]
#elif defined(__clang__) || (defined(__GNUC__) && __GNUC__ >= 7)
#define XPP_FALLTHROUGH __attribute__((fallthrough))
#else
#define XPP_FALLTHROUGH ((void)0)
#endif

/**
 * @brief Strongly request that a function be inlined at every call site.
 *
 * Prefer plain `inline` first — `XPP_FORCE_INLINE` overrides the compiler's
 * cost heuristics and can hurt code size or even performance if misused.
 * Reserve it for tiny hot-path helpers where inlining is critical.
 */
#if defined(__GNUC__) || defined(__clang__)
#define XPP_FORCE_INLINE inline __attribute__((always_inline))
#elif defined(_MSC_VER)
#define XPP_FORCE_INLINE __forceinline
#else
#define XPP_FORCE_INLINE inline
#endif

/**
 * @brief Forbid inlining a function.
 *
 * Useful for cold paths (e.g. error reporters) where keeping the body
 * out-of-line shrinks the caller and improves icache behavior, or for
 * functions you want to see distinctly in profiles and stack traces.
 */
#if defined(__GNUC__) || defined(__clang__)
#define XPP_NOINLINE __attribute__((noinline))
#elif defined(_MSC_VER)
#define XPP_NOINLINE __declspec(noinline)
#else
#define XPP_NOINLINE
#endif

/**
 * @brief Master switch for xpp debug instrumentation.
 *
 * Controls all debug-only facilities: XPP_DEBUG_ASSERT, deadlock
 * detection, bounds-check diagnostics, etc.
 *
 * Default behaviour:
 *   - NDEBUG undefined (Debug build) → XPP_DEBUG = 1
 *   - NDEBUG defined (Release build) → XPP_DEBUG = 0
 *
 * Override from the command line to decouple from build type:
 *   -DXPP_DEBUG=1   Force enable in Release (diagnostics without rebuild)
 *   -DXPP_DEBUG=0   Force disable in Debug (benchmark without check overhead)
 */
#ifndef XPP_DEBUG
  #ifdef NDEBUG
    #define XPP_DEBUG 0
  #else
    #define XPP_DEBUG 1
  #endif
#endif

#endif // XPP_COMPILER_H
