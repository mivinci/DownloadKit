/*
 * Copyright 2025 The xKit Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * str.c - SDS-style dynamic string implementation
 */

#include <xbase/compat.h>
#include <xbase/str.h>

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ───────────────────── Internal header ───────────────────── */

XDEF_STRUCT(xStrHeader) {
  size_t len; /**< Used bytes (not counting NUL)   */
  size_t cap; /**< Allocated bytes (not counting header + NUL) */
};

#define XSTR_HEADER_SIZE (sizeof(xStrHeader))

/** Access the header from the user-facing char* pointer. */
static inline xStrHeader *xstr_hdr(const xStr s) {
  return (xStrHeader *)(s - XSTR_HEADER_SIZE);
}

static inline const xStrHeader *xstr_hdr_const(const xStr s) {
  return (const xStrHeader *)(s - XSTR_HEADER_SIZE);
}

/** Minimum capacity so we don't realloc on every tiny append. */
#define XSTR_MIN_CAP 64

/** Allocate header + cap + 1 (for NUL), return the data pointer. */
static xStr xstr_alloc(size_t cap) {
  if (cap < XSTR_MIN_CAP) cap = XSTR_MIN_CAP;
  xStrHeader *hdr = (xStrHeader *)malloc(XSTR_HEADER_SIZE + cap + 1);
  if (!hdr) return NULL;
  hdr->len = 0;
  hdr->cap = cap;
  xStr s   = (xStr)(hdr + 1);
  s[0]     = '\0';
  return s;
}

/** Grow capacity to fit at least needed bytes (len + add_len + 1 for NUL). */
static xStr xstr_ensure(xStr s, size_t add_len) {
  xStrHeader *hdr    = xstr_hdr(s);
  size_t      needed = hdr->len + add_len + 1;
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

  xStrHeader *new_hdr =
    (xStrHeader *)realloc(hdr, XSTR_HEADER_SIZE + new_cap + 1);
  if (!new_hdr) return NULL;
  new_hdr->cap = new_cap;
  return (xStr)(new_hdr + 1);
}

/* ───────────────────── Lifecycle ───────────────────── */

xStr xStrCreate(const char *init) {
  size_t len = init ? strlen(init) : 0;
  xStr   s   = xstr_alloc(len);
  if (!s) return NULL;
  if (len > 0) {
    memcpy(s, init, len);
    xstr_hdr(s)->len = len;
    s[len]           = '\0';
  }
  return s;
}

xStr xStrCreateLen(const void *init, size_t len) {
  xStr s = xstr_alloc(len);
  if (!s) return NULL;
  if (len > 0 && init) {
    memcpy(s, init, len);
  }
  xstr_hdr(s)->len = len;
  s[len]           = '\0';
  return s;
}

void xStrDestroy(xStr s) {
  if (!s) return;
  free(xstr_hdr(s));
}

xStr xStrDup(const xStr s) {
  if (!s) return NULL;
  return xStrCreateLen(s, xstr_hdr_const(s)->len);
}

/* ───────────────────── Append ───────────────────── */

xStr xStrAppend(xStr s, const char *append) {
  if (!s || !append) return s;
  size_t alen = strlen(append);
  return xStrAppendLen(s, append, alen);
}

xStr xStrAppendLen(xStr s, const void *append, size_t len) {
  if (!s || len == 0) return s;
  xStr ns = xstr_ensure(s, len);
  if (!ns) return NULL; /* original s still valid */
  if (append) {
    memcpy(ns + xstr_hdr(ns)->len, append, len);
  }
  xstr_hdr(ns)->len += len;
  ns[xstr_hdr(ns)->len] = '\0';
  return ns;
}

xStr xStrAppendFormat(xStr s, const char *fmt, ...) {
  if (!s || !fmt) return s;

  va_list ap;
  va_start(ap, fmt);

  /* First, try to write directly into available space. */
  xStrHeader *hdr   = xstr_hdr(s);
  size_t      avail = hdr->cap - hdr->len;
  va_list     ap2;
  va_copy(ap2, ap);
  int n = vsnprintf(s + hdr->len, avail + 1, fmt, ap2);
  va_end(ap2);

  if (n < 0) {
    va_end(ap);
    return s; /* encoding error */
  }

  size_t needed = (size_t)n;
  if (needed <= avail) {
    /* Fit in available space. */
    hdr->len += needed;
    va_end(ap);
    return s;
  }

  /* Need more space — grow and retry. */
  xStr ns = xstr_ensure(s, needed);
  if (!ns) {
    va_end(ap);
    return NULL;
  }
  hdr = xstr_hdr(ns);
  n   = vsnprintf(ns + hdr->len, needed + 1, fmt, ap);
  va_end(ap);
  if (n < 0) return ns; /* shouldn't happen */
  hdr->len += (size_t)n;
  return ns;
}

/* ───────────────────── Truncate / Clear ───────────────────── */

void xStrTruncate(xStr s, size_t new_len) {
  if (!s) return;
  xStrHeader *hdr = xstr_hdr(s);
  if (new_len > hdr->len) return;
  hdr->len   = new_len;
  s[new_len] = '\0';
}

void xStrClear(xStr s) {
  if (!s) return;
  xstr_hdr(s)->len = 0;
  s[0]             = '\0';
}

/* ───────────────────── Accessors ───────────────────── */

size_t xStrLen(const xStr s) {
  if (!s) return 0;
  return xstr_hdr_const(s)->len;
}

size_t xStrCap(const xStr s) {
  if (!s) return 0;
  return xstr_hdr_const(s)->cap;
}

size_t xStrAvail(const xStr s) {
  if (!s) return 0;
  const xStrHeader *hdr = xstr_hdr_const(s);
  return hdr->cap - hdr->len;
}

/* ───────────────────── Memory control ───────────────────── */

xStr xStrGrow(xStr s, size_t add_len) {
  if (!s) return NULL;
  return xstr_ensure(s, add_len);
}

xStr xStrShrinkToFit(xStr s) {
  if (!s) return NULL;
  xStrHeader *hdr = xstr_hdr(s);
  if (hdr->cap == hdr->len) return s;

  size_t      new_size = XSTR_HEADER_SIZE + hdr->len + 1;
  size_t      old_len  = hdr->len;
  xStrHeader *new_hdr  = (xStrHeader *)realloc(hdr, new_size);
  if (!new_hdr) return s; /* keep original on failure */
  new_hdr->cap = old_len;
  return (xStr)(new_hdr + 1);
}

/* ───────────────────── Search ───────────────────── */

/** Pattern length threshold: below → naive memcmp, >= → memmem. */
#define XSTR_FIND_THRESHOLD 32

size_t xStrFind(const xStr haystack, const char *needle, size_t needle_len) {
  if (!haystack) return XSTR_NONE;
  if (needle_len == 0) return 0;

  size_t hlen = xstr_hdr_const(haystack)->len;
  if (needle_len > hlen) return XSTR_NONE;

  if (needle_len < XSTR_FIND_THRESHOLD) {
    /* Naive scan — avoids memmem call overhead for short patterns. */
    size_t last = hlen - needle_len;
    for (size_t i = 0; i <= last; i++) {
      if (memcmp(haystack + i, needle, needle_len) == 0)
        return i;
    }
    return XSTR_NONE;
  }

  /* memmem — leverages platform-optimized search (Two-Way on glibc). */
  const char *found =
    (const char *)memmem(haystack, hlen, needle, needle_len);
  return found ? (size_t)(found - haystack) : XSTR_NONE;
}

size_t xStrFindStr(const xStr haystack, const char *needle) {
  if (!needle) return XSTR_NONE;
  return xStrFind(haystack, needle, strlen(needle));
}

/* ───────────────────── Comparison ───────────────────── */

int xStrCmp(const xStr s1, const xStr s2) {
  if (!s1 && !s2) return 0;
  if (!s1) return -1;
  if (!s2) return 1;

  size_t l1   = xstr_hdr_const(s1)->len;
  size_t l2   = xstr_hdr_const(s2)->len;
  size_t minl = l1 < l2 ? l1 : l2;
  int    cmp  = memcmp(s1, s2, minl);
  if (cmp != 0) return cmp;
  if (l1 < l2) return -1;
  if (l1 > l2) return 1;
  return 0;
}

int xStrEq(const xStr s1, const xStr s2) {
  return xStrCmp(s1, s2) == 0;
}
