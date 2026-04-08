/*
 * Copyright 2025 The xKit Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * io.c - Abstract I/O interface implementations
 */

#include <xbase/io.h>

#include <errno.h>
#include <stdlib.h>
#include <string.h>

/* ═══════════════════════════════════════════════════════════════════
 *  Convenience functions
 * ═══════════════════════════════════════════════════════════════════
 */

ssize_t xRead(xReader r, void *buf, size_t len) {
  return r.read(r.ctx, buf, len);
}

ssize_t xWrite(xWriter w, const void *buf, size_t len) {
  struct iovec iov = {.iov_base = (void *)buf, .iov_len = len};
  return w.writev(w.ctx, &iov, 1);
}

ssize_t xWritev(xWriter w, const struct iovec *iov, int iovcnt) {
  return w.writev(w.ctx, iov, iovcnt);
}

off_t xSeek(xSeeker s, off_t offset, int whence) {
  return s.seek(s.ctx, offset, whence);
}

int xClose(xCloser c) { return c.close(c.ctx); }

/* ═══════════════════════════════════════════════════════════════════
 *  Advanced helper functions
 * ═══════════════════════════════════════════════════════════════════
 */

ssize_t xReadFull(xReader r, void *buf, size_t len) {
  size_t total = 0;
  while (total < len) {
    ssize_t n = r.read(r.ctx, (char *)buf + total, len - total);
    if (n > 0) {
      total += (size_t)n;
    } else if (n == 0) {
      /* EOF */
      break;
    } else {
      /* n == -1 */
      if (errno == EAGAIN || errno == EINTR) continue;
      return -1;
    }
  }
  return (ssize_t)total;
}

#define XREAD_ALL_INIT_CAP 4096

int xReadAll(xReader r, void **out, size_t *out_len) {
  size_t cap  = XREAD_ALL_INIT_CAP;
  size_t total = 0;
  char  *buf  = (char *)malloc(cap);
  if (!buf) goto fail;

  for (;;) {
    if (total == cap) {
      size_t new_cap = cap * 2;
      char  *new_buf = (char *)realloc(buf, new_cap);
      if (!new_buf) goto fail;
      buf = new_buf;
      cap = new_cap;
    }

    ssize_t n = r.read(r.ctx, buf + total, cap - total);
    if (n > 0) {
      total += (size_t)n;
    } else if (n == 0) {
      /* EOF — success */
      *out     = buf;
      *out_len = total;
      return 0;
    } else {
      /* n == -1 */
      if (errno == EAGAIN || errno == EINTR) continue;
      goto fail;
    }
  }

fail:
  free(buf);
  *out     = NULL;
  *out_len = 0;
  return -1;
}
