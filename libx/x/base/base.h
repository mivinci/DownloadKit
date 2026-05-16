/*
 * Copyright 2025 The libx Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * base.h - Base definitions and macros
 */

#ifndef XBASE_BASE_H
#define XBASE_BASE_H

/**
 * XCAPI(T) — declares a C-linkage symbol of type T.
 *
 * For function declarations, T is the return type.
 *
 * For variable declarations, the macro always adds `extern`, so the
 * declaration does NOT become a tentative definition (required to
 * compile cleanly under GCC 10+ which defaults to `-fno-common`, and
 * to avoid multiple-definition errors when the header is included by
 * several translation units).
 *
 * Usage (variable):   XCAPI(const xFoo) gFoo;   // extern const xFoo gFoo;
 * Usage (function):   XCAPI(int) xFooBar(void);
 */
#ifdef __cplusplus
#define XCAPI(T) extern "C" T
#else
#define XCAPI(T) extern T
#endif

#ifdef __cplusplus
#define XCAPI_INLINE(T) extern "C" inline T
#else
#define XCAPI_INLINE(T) static inline T
#endif

#define XDEF_STRUCT(T) \
  typedef struct T T;  \
  struct T

#define XDEF_ENUM(T) \
  typedef int T;     \
  enum

#define XDEF_HANDLE(T)          typedef void *T
#define XDEF_HANDLE_EXPLICIT(T) typedef struct T *T

#include <stddef.h>

/**
 * @brief Obtain a pointer to the enclosing struct from a pointer to a member.
 * @param ptr    Pointer to the member field.
 * @param type   Type of the enclosing struct.
 * @param member Name of the member field inside @p type.
 */
#define xContainerOf(ptr, type, member) \
  ((type *)((char *)(ptr) - offsetof(type, member)))

#ifndef __cplusplus
#ifdef __STDC_VERSION__ /* C99 and later */
#include <stdbool.h>
#elif !defined(bool)
#define bool  _Bool
#define true  1
#define false 0
#endif
#endif

#endif // XBASE_BASE_H
