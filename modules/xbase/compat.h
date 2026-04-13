/*
 * Copyright 2025 The xKit Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * compat.h - Platform compatibility helpers
 */

#ifndef XBASE_COMPAT_H
#define XBASE_COMPAT_H

#include <string.h>

/* ─────────────────── memmem polyfill ─────────────────── */

/*
 * memmem() searches for a byte sequence within another byte sequence.
 * It is a GNU extension (_GNU_SOURCE on glibc) and is natively
 * available on macOS/BSD, but missing on some platforms (e.g. Windows,
 * older glibc without _GNU_SOURCE).
 *
 * Provide a portable inline fallback when the system lacks it.
 */
#if !defined(__APPLE__) && !defined(_GNU_SOURCE)

static inline void *xcompat_memmem(const void *haystack, size_t hlen,
                               const void *needle, size_t nlen) {
  if (nlen == 0) return (void *)haystack;
  if (nlen > hlen) return NULL;

  const unsigned char *h = (const unsigned char *)haystack;
  const unsigned char *n = (const unsigned char *)needle;
  const unsigned char *end = h + hlen - nlen;

  for (; h <= end; h++) {
    if (h[0] == n[0] && memcmp(h, n, nlen) == 0)
      return (void *)h;
  }
  return NULL;
}

#define memmem xcompat_memmem

#endif /* !__APPLE__ && !_GNU_SOURCE */

#endif /* XBASE_COMPAT_H */
