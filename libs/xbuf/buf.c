/*
 * Copyright 2025 The moo Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * buf.c - Linear auto-growing byte buffer implementation
 */

#include <xbuf/buf.h>

#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* ───────────────────── Types ───────────────────── */

XDEF_STRUCT(xBuffer_) {
  size_t rpos;   /* Read position (start of unread)    */
  size_t wpos;   /* Write position (end of unread)     */
  size_t cap;    /* Total allocated data capacity      */
  char   data[]; /* Inline data storage (flexible array member) */
};

/* ───────────────────── Internal ───────────────────── */

/** Minimum allocation size to avoid tiny reallocs. */
#define BUF_MIN_CAP 64

/** Growth factor: double the capacity each time. */
static size_t buf_next_cap(size_t current, size_t needed) {
  size_t cap = current ? current : BUF_MIN_CAP;
  if (needed > SIZE_MAX / 2) return SIZE_MAX;
  while (cap < needed)
    cap *= 2;
  return cap;
}

/**
 * Ensure the buffer has at least `needed` total bytes of data capacity.
 * Compacts first if that alone would suffice, otherwise reallocs the
 * entire object (header + data).
 *
 * Because realloc may relocate, the caller's pointer is updated via bufp.
 */
static xErrno buf_grow(xBuffer_ **bufp, size_t needed) {
  xBuffer_ *buf = *bufp;
  xBuffer_ *newbuf;
  size_t    newcap;

  /* Already enough room? */
  if (needed <= buf->cap) return xErrno_Ok;

  /* Try compact first: slide unread data to the front.  If the unread
   * portion plus the requested total fits within the existing capacity,
   * we can avoid a realloc entirely. */
  if (buf->rpos > 0) {
    size_t unread = buf->wpos - buf->rpos;
    if (unread + needed <= buf->cap) {
      memmove(buf->data, buf->data + buf->rpos, unread);
      buf->rpos = 0;
      buf->wpos = unread;
      return xErrno_Ok;
    }
  }

  newcap = buf_next_cap(buf->cap, needed);
  newbuf = (xBuffer_ *)realloc(buf, sizeof(xBuffer_) + newcap);
  if (!newbuf) return xErrno_NoMemory;

  newbuf->cap = newcap;
  *bufp       = newbuf;
  return xErrno_Ok;
}

/* ───────────────────── Lifecycle ───────────────────── */

xBuffer xBufferCreate(size_t initial_cap) {
  xBuffer_ *buf;

  if (initial_cap == 0) initial_cap = BUF_MIN_CAP;

  buf = (xBuffer_ *)malloc(sizeof(xBuffer_) + initial_cap);
  if (!buf) return NULL;

  buf->rpos = 0;
  buf->wpos = 0;
  buf->cap  = initial_cap;
  return (xBuffer)buf;
}

void xBufferDestroy(xBuffer buf) {
  free(buf);
}

void xBufferReset(xBuffer buf) {
  xBuffer_ *b = (xBuffer_ *)buf;
  if (!b) return;
  b->rpos = 0;
  b->wpos = 0;
}

/* ───────────────────── Write ───────────────────── */

xErrno xBufferAppend(xBuffer *bufp, const void *data, size_t len) {
  xBuffer_ *b;
  xErrno    err;

  if (!bufp || !*bufp) return xErrno_InvalidArg;
  if (len == 0) return xErrno_Ok;

  b   = (xBuffer_ *)*bufp;
  err = buf_grow(&b, b->wpos + len);
  if (err != xErrno_Ok) return err;

  memcpy(b->data + b->wpos, data, len);
  b->wpos += len;
  *bufp = (xBuffer)b;
  return xErrno_Ok;
}

xErrno xBufferAppendStr(xBuffer *bufp, const char *str) {
  if (!bufp || !*bufp || !str) return xErrno_InvalidArg;
  return xBufferAppend(bufp, str, strlen(str));
}

xErrno xBufferReserve(xBuffer *bufp, size_t additional) {
  xBuffer_ *b;
  xErrno    err;

  if (!bufp || !*bufp) return xErrno_InvalidArg;

  b = (xBuffer_ *)*bufp;

  /* Compact first to maximize writable space. */
  if (b->rpos > 0) {
    size_t unread = b->wpos - b->rpos;
    if (unread > 0) memmove(b->data, b->data + b->rpos, unread);
    b->rpos = 0;
    b->wpos = unread;
  }

  err   = buf_grow(&b, b->wpos + additional);
  *bufp = (xBuffer)b;
  return err;
}

/* ───────────────────── Read ───────────────────── */

const void *xBufferData(xBuffer buf) {
  xBuffer_ *b = (xBuffer_ *)buf;
  if (!b || b->rpos >= b->wpos) return NULL;
  return b->data + b->rpos;
}

size_t xBufferLen(xBuffer buf) {
  xBuffer_ *b = (xBuffer_ *)buf;
  if (!b) return 0;
  return b->wpos - b->rpos;
}

size_t xBufferCap(xBuffer buf) {
  xBuffer_ *b = (xBuffer_ *)buf;
  if (!b) return 0;
  return b->cap;
}

size_t xBufferWritable(xBuffer buf) {
  xBuffer_ *b = (xBuffer_ *)buf;
  if (!b) return 0;
  return b->cap - b->wpos;
}

void xBufferConsume(xBuffer buf, size_t n) {
  xBuffer_ *b = (xBuffer_ *)buf;
  if (!b) return;
  if (n >= b->wpos - b->rpos) {
    b->rpos = 0;
    b->wpos = 0;
  } else {
    b->rpos += n;
  }
}

void xBufferCompact(xBuffer buf) {
  xBuffer_ *b = (xBuffer_ *)buf;
  size_t    unread;

  if (!b || b->rpos == 0) return;

  unread = b->wpos - b->rpos;
  if (unread > 0) memmove(b->data, b->data + b->rpos, unread);
  b->rpos = 0;
  b->wpos = unread;
}

/* ───────────────────── I/O helpers ───────────────────── */

ssize_t xBufferReadFd(xBuffer *bufp, int fd) {
  xBuffer_ *b;
  ssize_t   n;
  xErrno    err;

  if (!bufp || !*bufp) return -1;

  /* Ensure at least 4KB of writable space for the read. */
  if (xBufferWritable(*bufp) < 4096) {
    err = xBufferReserve(bufp, 4096);
    if (err != xErrno_Ok) return -1;
  }

  b = (xBuffer_ *)*bufp;
  do {
    n = read(fd, b->data + b->wpos, b->cap - b->wpos);
  } while (n < 0 && errno == EINTR);
  if (n > 0) b->wpos += (size_t)n;
  return n;
}

ssize_t xBufferWriteFd(xBuffer buf, int fd) {
  xBuffer_ *b = (xBuffer_ *)buf;
  ssize_t   n;
  size_t    readable;

  if (!b) return -1;

  readable = b->wpos - b->rpos;
  if (readable == 0) return 0;

  do {
    n = write(fd, b->data + b->rpos, readable);
  } while (n < 0 && errno == EINTR);
  if (n > 0) xBufferConsume(buf, (size_t)n);
  return n;
}
