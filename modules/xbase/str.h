/*
 * Copyright 2025 The xKit Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * str.h - SDS-style dynamic string
 *
 * xStr is a NUL-terminated auto-growing byte string, compatible with
 * all C string functions (printf %s, strcmp, …). The header (length +
 * capacity) is hidden before the user pointer, so xStr IS a char*.
 *
 * Inspired by Redis SDS (Simple Dynamic Strings).
 */

#ifndef XBASE_STR_H
#define XBASE_STR_H

#include <stdarg.h>
#include <stddef.h>
#include <xbase/base.h>

/* ───────────────────── Type ───────────────────── */

/**
 * @brief SDS-style dynamic string — just a char*.
 *
 * The header lives at (s - sizeof(xStrHeader)), so every xStr can be
 * passed directly to C string APIs. It is always NUL-terminated.
 */
typedef char *xStr;

/** Sentinel value returned by xStrFind / xStrFindStr when not found. */
#define XSTR_NONE ((size_t)-1)

/* ───────────────────── Lifecycle ───────────────────── */

/**
 * @brief Create an xStr from a C string.
 *
 * @param init  C string to copy (NULL → empty string "").
 * @return New xStr, or NULL on allocation failure.
 */
XCAPI(xStr) xStrCreate(const char *init);

/**
 * @brief Create an xStr from raw memory (binary-safe).
 *
 * @param init  Pointer to data (NULL → empty string "").
 * @param len   Number of bytes to copy from @p init.
 * @return New xStr, or NULL on allocation failure.
 */
XCAPI(xStr) xStrCreateLen(const void *init, size_t len);

/**
 * @brief Free an xStr (NULL-safe).
 */
XCAPI(void) xStrDestroy(xStr s);

/**
 * @brief Deep-copy an xStr.
 *
 * @param s  Source string (NULL → NULL).
 * @return Cloned xStr, or NULL on allocation failure.
 */
XCAPI(xStr) xStrDup(const xStr s);

/* ───────────────────── Append ───────────────────── */

/**
 * @brief Append a C string.
 *
 * May reallocate; caller must use the return value:
 *   s = xStrAppend(s, "hello");
 *
 * @param s       Existing xStr (must not be NULL).
 * @param append  C string to append (must not be NULL).
 * @return Updated xStr (may differ from @p s), or NULL on failure
 *         (original @p s is still valid).
 */
XCAPI(xStr) xStrAppend(xStr s, const char *append);

/**
 * @brief Append raw bytes (binary-safe).
 *
 * @param s       Existing xStr (must not be NULL).
 * @param append  Data to append (must not be NULL if len > 0).
 * @param len     Number of bytes to append.
 * @return Updated xStr, or NULL on failure (original still valid).
 */
XCAPI(xStr) xStrAppendLen(xStr s, const void *append, size_t len);

/**
 * @brief Append a printf-style formatted string.
 *
 * @param s     Existing xStr (must not be NULL).
 * @param fmt   printf format string.
 * @param ...   Format arguments.
 * @return Updated xStr, or NULL on failure (original still valid).
 */
XCAPI(xStr) xStrAppendFormat(xStr s, const char *fmt, ...)
  __attribute__((format(printf, 2, 3)));

/* ───────────────────── Truncate / Clear ───────────────────── */

/**
 * @brief Truncate to @p new_len bytes (lazy — does not shrink allocation).
 *
 * @param s        xStr (must not be NULL).
 * @param new_len  Must be <= xStrLen(s).
 */
XCAPI(void) xStrTruncate(xStr s, size_t new_len);

/**
 * @brief Clear to empty string "" (lazy — does not shrink allocation).
 */
XCAPI(void) xStrClear(xStr s);

/* ───────────────────── Accessors ───────────────────── */

/**
 * @brief Return the string length in O(1) (NULL → 0).
 */
XCAPI(size_t) xStrLen(const xStr s);

/**
 * @brief Return allocated capacity (NULL → 0).
 */
XCAPI(size_t) xStrCap(const xStr s);

/**
 * @brief Return available space = cap - len (NULL → 0).
 */
XCAPI(size_t) xStrAvail(const xStr s);

/* ───────────────────── Memory control ───────────────────── */

/**
 * @brief Pre-allocate space for at least @p add_len more bytes.
 *
 * Does not change the string length.
 *
 * @param s        xStr (must not be NULL).
 * @param add_len  Extra bytes needed beyond current length.
 * @return Updated xStr, or NULL on failure (original still valid).
 */
XCAPI(xStr) xStrGrow(xStr s, size_t add_len);

/**
 * @brief Shrink allocation to fit the current content exactly.
 *
 * @param s  xStr (must not be NULL).
 * @return Updated xStr (may differ), or NULL on failure (original still valid).
 */
XCAPI(xStr) xStrShrinkToFit(xStr s);

/* ───────────────────── Search ───────────────────── */

/**
 * @brief Find first occurrence of @p needle in @p haystack (binary-safe).
 *
 * Uses naive memcmp for short patterns and memmem for longer ones.
 *
 * @param haystack    xStr to search in (must not be NULL).
 * @param needle      Data to search for (must not be NULL if needle_len > 0).
 * @param needle_len  Length of @p needle in bytes.
 * @return Byte index of first match, or XSTR_NONE if not found.
 */
XCAPI(size_t) xStrFind(const xStr haystack, const char *needle,
                       size_t needle_len);

/**
 * @brief Find first occurrence of a C string in @p haystack.
 *
 * Equivalent to xStrFind(haystack, needle, strlen(needle)).
 *
 * @return Byte index of first match, or XSTR_NONE if not found.
 */
XCAPI(size_t) xStrFindStr(const xStr haystack, const char *needle);

/* ───────────────────── Comparison ───────────────────── */

/**
 * @brief Binary-safe comparison.
 *
 * @return <0, 0, >0 like memcmp. NULL sorts before non-NULL.
 */
XCAPI(int) xStrCmp(const xStr s1, const xStr s2);

/**
 * @brief Return non-zero if equal (NULL == NULL is true).
 */
XCAPI(int) xStrEq(const xStr s1, const xStr s2);

#endif /* XBASE_STR_H */
