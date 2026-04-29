/*
 * Copyright 2025 The xKit Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * string.h - SDS-style dynamic string
 *
 * xString is a NUL-terminated auto-growing byte string, compatible with
 * all C string functions (printf %s, strcmp, …). The header (length +
 * capacity) is hidden before the user pointer, so xString IS a char*.
 *
 * Inspired by Redis SDS (Simple Dynamic Strings).
 */

#ifndef XBASE_STRING_H
#define XBASE_STRING_H

#include <stdarg.h>
#include <stddef.h>
#include <xbase/base.h>
#include <xbase/error.h>

/* ───────────────────── Type ───────────────────── */

/**
 * @brief SDS-style dynamic string — just a char*.
 *
 * The header lives at (s - sizeof(xStringHeader)), so every xString can be
 * passed directly to C string APIs. It is always NUL-terminated.
 */
typedef char *xString;

/** Sentinel value returned by xStringFind / xStringFindStr when not found. */
#define XSTRING_NONE ((size_t)-1)

/* ───────────────────── Lifecycle ───────────────────── */

/**
 * @brief Create an xString from a C string.
 *
 * @param init  C string to copy (NULL → empty string "").
 * @return New xString, or NULL on allocation failure.
 */
XCAPI(xString) xStringCreate(const char *init);

/**
 * @brief Create an xString from raw memory (binary-safe).
 *
 * @param init  Pointer to data (NULL → empty string "").
 * @param len   Number of bytes to copy from @p init.
 * @return New xString, or NULL on allocation failure.
 */
XCAPI(xString) xStringCreateLen(const void *init, size_t len);

/**
 * @brief Free an xString (NULL-safe).
 */
XCAPI(void) xStringDestroy(xString s);

/**
 * @brief Deep-copy an xString.
 *
 * @param s  Source string (NULL → NULL).
 * @return Cloned xString, or NULL on allocation failure.
 */
XCAPI(xString) xStringDup(const xString s);

/* ───────────────────── Append ───────────────────── */

/**
 * @brief Append a C string.
 *
 * May reallocate; the pointer is updated in-place:
 *   xStringAppend(&s, "hello");
 *
 * @param s       Pointer to existing xString (must not be NULL, *s must not be NULL).
 * @param append  C string to append (must not be NULL).
 * @return Number of bytes appended on success, or negative xErrno on failure
 *         (*s is still valid on failure).
 */
XCAPI(int) xStringAppend(xString *s, const char *append);

/**
 * @brief Append raw bytes (binary-safe).
 *
 * @param s       Pointer to existing xString (must not be NULL, *s must not be NULL).
 * @param append  Data to append (must not be NULL if len > 0).
 * @param len     Number of bytes to append.
 * @return Number of bytes appended on success, or negative xErrno on failure
 *         (*s is still valid on failure).
 */
XCAPI(int) xStringAppendLen(xString *s, const void *append, size_t len);

/**
 * @brief Append a printf-style formatted string.
 *
 * @param s     Pointer to existing xString (must not be NULL, *s must not be NULL).
 * @param fmt   printf format string.
 * @param ...   Format arguments.
 * @return Number of bytes appended on success, or negative xErrno on failure
 *         (*s is still valid on failure).
 */
XCAPI(int) xStringAppendFormat(xString *s, const char *fmt, ...)
  __attribute__((format(printf, 2, 3)));

/* ───────────────────── Truncate / Clear ───────────────────── */

/**
 * @brief Truncate to @p new_len bytes (lazy — does not shrink allocation).
 *
 * @param s        xString (must not be NULL).
 * @param new_len  Must be <= xStringLen(s).
 */
XCAPI(void) xStringTruncate(xString s, size_t new_len);

/**
 * @brief Clear to empty string "" (lazy — does not shrink allocation).
 */
XCAPI(void) xStringClear(xString s);

/* ───────────────────── Accessors ───────────────────── */

/**
 * @brief Return the string length in O(1) (NULL → 0).
 */
XCAPI(size_t) xStringLen(const xString s);

/**
 * @brief Return allocated capacity (NULL → 0).
 */
XCAPI(size_t) xStringCap(const xString s);

/**
 * @brief Return available space = cap - len (NULL → 0).
 */
XCAPI(size_t) xStringAvail(const xString s);

/* ───────────────────── Memory control ───────────────────── */

/**
 * @brief Pre-allocate space for at least @p add_len more bytes.
 *
 * Does not change the string length.
 *
 * @param s        xString (must not be NULL).
 * @param add_len  Extra bytes needed beyond current length.
 * @return Updated xString, or NULL on failure (original still valid).
 */
XCAPI(xString) xStringGrow(xString s, size_t add_len);

/**
 * @brief Shrink allocation to fit the current content exactly.
 *
 * @param s  xString (must not be NULL).
 * @return Updated xString (may differ), or NULL on failure (original still valid).
 */
XCAPI(xString) xStringShrinkToFit(xString s);

/* ───────────────────── Search ───────────────────── */

/**
 * @brief Find first occurrence of @p needle in @p haystack (binary-safe).
 *
 * Uses naive memcmp for short patterns and memmem for longer ones.
 *
 * @param haystack    xString to search in (must not be NULL).
 * @param needle      Data to search for (must not be NULL if needle_len > 0).
 * @param needle_len  Length of @p needle in bytes.
 * @return Byte index of first match, or XSTRING_NONE if not found.
 */
XCAPI(size_t) xStringFind(const xString haystack, const char *needle,
                          size_t needle_len);

/**
 * @brief Find first occurrence of a C string in @p haystack.
 *
 * Equivalent to xStringFind(haystack, needle, strlen(needle)).
 *
 * @return Byte index of first match, or XSTRING_NONE if not found.
 */
XCAPI(size_t) xStringFindStr(const xString haystack, const char *needle);

/* ───────────────────── Comparison ───────────────────── */

/**
 * @brief Binary-safe comparison.
 *
 * @return <0, 0, >0 like memcmp. NULL sorts before non-NULL.
 */
XCAPI(int) xStringCmp(const xString s1, const xString s2);

/**
 * @brief Return non-zero if equal (NULL == NULL is true).
 */
XCAPI(int) xStringEq(const xString s1, const xString s2);

#endif /* XBASE_STRING_H */
