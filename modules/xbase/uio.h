/*
 * Copyright 2025 The xKit Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * uio.h - Cross-platform iovec / readv / writev compatibility
 *
 * On POSIX, this simply includes <sys/uio.h>.
 * On Windows, it provides a compatible xiovec struct and xwritev/xreadv
 * helpers backed by recv()/send().
 */

#ifndef XBASE_UIO_H
#define XBASE_UIO_H

#include <stddef.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>

#include <xbase/base.h> /* ssize_t */

/**
 * Windows-compatible iovec structure.
 */
typedef struct xiovec {
  void  *iov_base;
  size_t iov_len;
} xiovec;

/**
 * Scatter-gather write using send() on each buffer.
 */
static inline ssize_t xwritev(int fd, const xiovec *iov, int iovcnt) {
  ssize_t total = 0;
  for (int i = 0; i < iovcnt; i++) {
    if (iov[i].iov_len == 0) continue;
    int n =
      send((SOCKET)fd, (const char *)iov[i].iov_base, (int)iov[i].iov_len, 0);
    if (n < 0) {
      if (total > 0) return total;
      return -1;
    }
    total += n;
    if ((size_t)n < iov[i].iov_len) break;
  }
  return total;
}

/**
 * Scatter-gather read using recv() into each buffer.
 */
static inline ssize_t xreadv(int fd, const xiovec *iov, int iovcnt) {
  ssize_t total = 0;
  for (int i = 0; i < iovcnt; i++) {
    if (iov[i].iov_len == 0) continue;
    int n = recv((SOCKET)fd, (char *)iov[i].iov_base, (int)iov[i].iov_len, 0);
    if (n < 0) {
      if (total > 0) return total;
      return -1;
    }
    total += n;
    if ((size_t)n < iov[i].iov_len) break;
    if (n == 0) break; /* EOF */
  }
  return total;
}

#else /* POSIX */

#include <sys/uio.h>

typedef struct iovec xiovec;
#define xwritev(fd, iov, iovcnt) writev(fd, iov, iovcnt)
#define xreadv(fd, iov, iovcnt)  readv(fd, iov, iovcnt)

#endif

#endif /* XBASE_UIO_H */
