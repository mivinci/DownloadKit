/*
 * Copyright 2025 The xKit Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * string.c - SDS-style dynamic string implementation
 */

#include <xbase/compat.h>
#include <xbase/string.h>

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ───────────────────── Internal header ───────────────────── */

XDEF_STRUCT(xStringHeader) {
  size_t len; /**< Used bytes (not counting NUL)   */
  size_t cap; /**< Allocated bytes (not counting header + NUL) */
};

#define XSTRING_HEADER_SIZE (sizeof(xStringHeader))

/** Access the header from the user-facing char* pointer. */
static inline xStringHeader *xstring_hdr(const xString s) {
  return (xStringHeader *)(s - XSTRING_HEADER_SIZE);
}

static inline const xStringHeader *xstring_hdr_const(const xString s) {
  return (const xStringHeader *)(s - XSTRING_HEADER_SIZE);
}

/** Minimum capacity so we don't realloc on every tiny append. */
#define XSTRING_MIN_CAP 64

/** Allocate header + cap + 1 (for NUL), return the data pointer. */
static xString xstring_alloc(size_t cap) {
  if (cap < XSTRING_MIN_CAP) cap = XSTRING_MIN_CAP;
  xStringHeader *hdr = (xStringHeader *)malloc(XSTRING_HEADER_SIZE + cap + 1);
  if (!hdr) return NULL;
  hdr->len = 0;
  hdr->cap = cap;
  xString s  = (xString)(hdr + 1);
  s[0]       = '\0';
  return s;
}

/** Grow capacity to fit at least needed bytes (len + add_len + 1 for NUL). */
static xString xstring_ensure(xString s, size_t add_len) {
  xStringHeader *hdr    = xstring_hdr(s);
  size_t         needed = hdr->len + add_len + 1;
  if (hdr->cap >= needed) return s;

  /* Grow: <1MB → double, >=1MB → +1MB */
  size_t new_cap = hdr->cap;
  while (new_cap < needed) {
    if (new_cap < 1024 * 1024) {
      new_cap *= 2;
    } else {
      new_cap += 1024 * 1024;
      if (new_cap < needed) new_cap = needed; /* overflow guard */
    }
  }

  xStringHeader *new_hdr =
    (xStringHeader *)realloc(hdr, XSTRING_HEADER_SIZE + new_cap + 1);
  if (!new_hdr) return NULL;
  new_hdr->cap = new_cap;
  return (xString)(new_hdr + 1);
}

/* ───────────────────── Lifecycle ───────────────────── */

xString xStringCreate(const char *init) {
  size_t   len = init ? strlen(init) : 0;
  xString  s   = xstring_alloc(len);
  if (!s) return NULL;
  if (len > 0) {
    memcpy(s, init, len);
    xstring_hdr(s)->len = len;
    s[len]              = '\0';
  }
  return s;
}

xString xStringCreateLen(const void *init, size_t len) {
  xString s = xstring_alloc(len);
  if (!s) return NULL;
  if (len > 0 && init) {
    memcpy(s, init, len);
  }
  xstring_hdr(s)->len = len;
  s[len]              = '\0';
  return s;
}

void xStringDestroy(xString s) {
  if (!s) return;
  free(xstring_hdr(s));
}

xString xStringDup(const xString s) {
  if (!s) return NULL;
  return xStringCreateLen(s, xstring_hdr_const(s)->len);
}

/* ───────────────────── Append ───────────────────── */

int xStringAppend(xString *s, const char *append) {
  if (!s || !*s) return -xErrno_InvalidArg;
  if (!append) return -xErrno_InvalidArg;
  size_t alen = strlen(append);
  return xStringAppendLen(s, append, alen);
}

int xStringAppendLen(xString *s, const void *append, size_t len) {
  if (!s || !*s) return -xErrno_InvalidArg;
  if (len == 0) return 0;
  xString ns = xstring_ensure(*s, len);
  if (!ns) return -xErrno_NoMemory; /* original *s still valid */
  if (append) {
    memcpy(ns + xstring_hdr(ns)->len, append, len);
  }
  xstring_hdr(ns)->len += len;
  ns[xstring_hdr(ns)->len] = '\0';
  *s = ns;
  return (int)len;
}

int xStringAppendFormat(xString *s, const char *fmt, ...) {
  if (!s || !*s) return -xErrno_InvalidArg;
  if (!fmt) return -xErrno_InvalidArg;

  va_list ap;
  va_start(ap, fmt);

  /* First, try to write directly into available space. */
  xStringHeader *hdr   = xstring_hdr(*s);
  size_t         avail = hdr->cap - hdr->len;
  va_list        ap2;
  va_copy(ap2, ap);
  int n = vsnprintf(*s + hdr->len, avail + 1, fmt, ap2);
  va_end(ap2);

  if (n < 0) {
    va_end(ap);
    return 0; /* encoding error — string unchanged */
  }

  size_t needed = (size_t)n;
  if (needed <= avail) {
    /* Fit in available space. */
    hdr->len += needed;
    va_end(ap);
    return n;
  }

  /* Need more space — grow and retry. */
  xString ns = xstring_ensure(*s, needed);
  if (!ns) {
    va_end(ap);
    return -xErrno_NoMemory;
  }
  hdr = xstring_hdr(ns);
  n   = vsnprintf(ns + hdr->len, needed + 1, fmt, ap);
  va_end(ap);
  if (n < 0) { *s = ns; return 0; } /* shouldn't happen */
  hdr->len += (size_t)n;
  *s = ns;
  return n;
}

/* ───────────────────── Truncate / Clear ───────────────────── */

void xStringTruncate(xString s, size_t new_len) {
  if (!s) return;
  xStringHeader *hdr = xstring_hdr(s);
  if (new_len > hdr->len) return;
  hdr->len    = new_len;
  s[new_len]  = '\0';
}

void xStringClear(xString s) {
  if (!s) return;
  xstring_hdr(s)->len = 0;
  s[0]                = '\0';
}

/* ───────────────────── Accessors ───────────────────── */

size_t xStringLen(const xString s) {
  if (!s) return 0;
  return xstring_hdr_const(s)->len;
}

size_t xStringCap(const xString s) {
  if (!s) return 0;
  return xstring_hdr_const(s)->cap;
}

size_t xStringAvail(const xString s) {
  if (!s) return 0;
  const xStringHeader *hdr = xstring_hdr_const(s);
  return hdr->cap - hdr->len;
}

/* ───────────────────── Memory control ───────────────────── */

xString xStringGrow(xString s, size_t add_len) {
  if (!s) return NULL;
  return xstring_ensure(s, add_len);
}

xString xStringShrinkToFit(xString s) {
  if (!s) return NULL;
  xStringHeader *hdr = xstring_hdr(s);
  if (hdr->cap == hdr->len) return s;

  size_t         new_size = XSTRING_HEADER_SIZE + hdr->len + 1;
  size_t         old_len  = hdr->len;
  xStringHeader *new_hdr  = (xStringHeader *)realloc(hdr, new_size);
  if (!new_hdr) return s; /* keep original on failure */
  new_hdr->cap = old_len;
  return (xString)(new_hdr + 1);
}

/* ───────────────────── Search ───────────────────── */

/** Pattern length threshold: below → naive memcmp, >= → memmem. */
#define XSTRING_FIND_THRESHOLD 32

size_t xStringFind(const xString haystack, const char *needle, size_t needle_len) {
  if (!haystack) return XSTRING_NONE;
  if (needle_len == 0) return 0;

  size_t hlen = xstring_hdr_const(haystack)->len;
  if (needle_len > hlen) return XSTRING_NONE;

  if (needle_len < XSTRING_FIND_THRESHOLD) {
    /* Naive scan — avoids memmem call overhead for short patterns. */
    size_t last = hlen - needle_len;
    for (size_t i = 0; i <= last; i++) {
      if (memcmp(haystack + i, needle, needle_len) == 0)
        return i;
    }
    return XSTRING_NONE;
  }

  /* memmem — leverages platform-optimized search (Two-Way on glibc). */
  const char *found =
    (const char *)memmem(haystack, hlen, needle, needle_len);
  return found ? (size_t)(found - haystack) : XSTRING_NONE;
}

size_t xStringFindStr(const xString haystack, const char *needle) {
  if (!needle) return XSTRING_NONE;
  return xStringFind(haystack, needle, strlen(needle));
}

/* ───────────────────── Comparison ───────────────────── */

int xStringCmp(const xString s1, const xString s2) {
  if (!s1 && !s2) return 0;
  if (!s1) return -1;
  if (!s2) return 1;

  size_t l1   = xstring_hdr_const(s1)->len;
  size_t l2   = xstring_hdr_const(s2)->len;
  size_t minl = l1 < l2 ? l1 : l2;
  int    cmp  = memcmp(s1, s2, minl);
  if (cmp != 0) return cmp;
  if (l1 < l2) return -1;
  if (l1 > l2) return 1;
  return 0;
}

int xStringEq(const xString s1, const xString s2) {
  return xStringCmp(s1, s2) == 0;
}
