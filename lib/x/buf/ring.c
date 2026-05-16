/*
 * Copyright 2025 The moo Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * ring.c - Fixed-size ring buffer implementation
 */

#include <x/buf/ring.h>

#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/uio.h>
#include <unistd.h>

/* ───────────────────── Types ───────────────────── */

XDEF_STRUCT(xRingBuffer_) {
  size_t cap;    /* Allocated capacity (power of two)    */
  size_t mask;   /* cap - 1, for fast modulo             */
  size_t head;   /* Write cursor (monotonic)             */
  size_t tail;   /* Read cursor  (monotonic)             */
  char   data[]; /* Inline data storage (flexible array member) */
};

/* ───────────────────── Internal ───────────────────── */

/** Round up to the next power of two (minimum 16). */
static size_t next_pow2(size_t v) {
  if (v < 16) v = 16;
  /* Guard against overflow: if v exceeds the largest representable
   * power of two, clamp to that value.  We cannot return SIZE_MAX
   * because the ring buffer relies on (cap - 1) as a bitmask, which
   * requires cap to be an exact power of two. */
  if (v > (SIZE_MAX >> 1) + 1) return (SIZE_MAX >> 1) + 1;
  v--;
  v |= v >> 1;
  v |= v >> 2;
  v |= v >> 4;
  v |= v >> 8;
  v |= v >> 16;
#if SIZE_MAX > 0xFFFFFFFFUL
  v |= v >> 32;
#endif
  return v + 1;
}

/* ───────────────────── Lifecycle ───────────────────── */

xRingBuffer xRingBufferCreate(size_t min_cap) {
  xRingBuffer_ *rb;
  size_t        cap;

  if (min_cap == 0) return NULL;

  cap = next_pow2(min_cap);

  rb = (xRingBuffer_ *)malloc(sizeof(xRingBuffer_) + cap);
  if (!rb) return NULL;

  rb->cap  = cap;
  rb->mask = cap - 1;
  rb->head = 0;
  rb->tail = 0;
  return (xRingBuffer)rb;
}

void xRingBufferDestroy(xRingBuffer rb) {
  free(rb);
}

void xRingBufferReset(xRingBuffer rb) {
  xRingBuffer_ *r = (xRingBuffer_ *)rb;
  if (!r) return;
  r->head = 0;
  r->tail = 0;
}

/* ───────────────────── Query ───────────────────── */

size_t xRingBufferLen(xRingBuffer rb) {
  xRingBuffer_ *r = (xRingBuffer_ *)rb;
  if (!r) return 0;
  return r->head - r->tail;
}

size_t xRingBufferCap(xRingBuffer rb) {
  xRingBuffer_ *r = (xRingBuffer_ *)rb;
  if (!r) return 0;
  return r->cap;
}

size_t xRingBufferWritable(xRingBuffer rb) {
  xRingBuffer_ *r = (xRingBuffer_ *)rb;
  if (!r) return 0;
  return r->cap - (r->head - r->tail);
}

bool xRingBufferEmpty(xRingBuffer rb) {
  xRingBuffer_ *r = (xRingBuffer_ *)rb;
  if (!r) return true;
  return r->head == r->tail;
}

bool xRingBufferFull(xRingBuffer rb) {
  xRingBuffer_ *r = (xRingBuffer_ *)rb;
  if (!r) return false;
  return (r->head - r->tail) == r->cap;
}

/* ───────────────────── Write ───────────────────── */

size_t xRingBufferWrite(xRingBuffer rb, const void *data, size_t len) {
  xRingBuffer_ *r = (xRingBuffer_ *)rb;
  size_t        writable, pos, first;

  if (!r) return 0;
  if (len == 0) return 0;

  writable = r->cap - (r->head - r->tail);
  if (len > writable) len = writable;
  if (len == 0) return 0;

  pos   = r->head & r->mask;
  first = r->cap - pos; /* bytes until wrap */

  if (len <= first) {
    memcpy(r->data + pos, data, len);
  } else {
    memcpy(r->data + pos, data, first);
    memcpy(r->data, (const char *)data + first, len - first);
  }

  r->head += len;
  return len;
}

/* ───────────────────── Read ───────────────────── */

/**
 * Internal: copy from ring to linear buffer, handling wrap-around.
 * Does NOT advance tail.
 */
static size_t ring_copy_out(const xRingBuffer_ *r, void *out, size_t len) {
  size_t readable, pos, first;

  readable = r->head - r->tail;
  if (len > readable) len = readable;
  if (len == 0) return 0;

  pos   = r->tail & r->mask;
  first = r->cap - pos;

  if (len <= first) {
    memcpy(out, r->data + pos, len);
  } else {
    memcpy(out, r->data + pos, first);
    memcpy((char *)out + first, r->data, len - first);
  }
  return len;
}

size_t xRingBufferRead(xRingBuffer rb, void *out, size_t len) {
  xRingBuffer_ *r = (xRingBuffer_ *)rb;
  size_t        n;
  if (!r) return 0;
  n = ring_copy_out(r, out, len);
  r->tail += n;
  return n;
}

size_t xRingBufferPeek(xRingBuffer rb, void *out, size_t len) {
  xRingBuffer_ *r = (xRingBuffer_ *)rb;
  if (!r) return 0;
  return ring_copy_out(r, out, len);
}

size_t xRingBufferDiscard(xRingBuffer rb, size_t n) {
  xRingBuffer_ *r = (xRingBuffer_ *)rb;
  size_t        readable;
  if (!r) return 0;
  readable = r->head - r->tail;
  if (n > readable) n = readable;
  r->tail += n;
  return n;
}

/* ───────────────────── I/O helpers ───────────────────── */

int xRingBufferReadIov(xRingBuffer rb, struct iovec iov[2]) {
  xRingBuffer_ *r = (xRingBuffer_ *)rb;
  size_t        readable, pos, first;

  if (!r) return 0;

  readable = r->head - r->tail;
  if (readable == 0) return 0;

  pos   = r->tail & r->mask;
  first = r->cap - pos;

  if (readable <= first) {
    iov[0].iov_base = (void *)(r->data + pos);
    iov[0].iov_len  = readable;
    return 1;
  }

  iov[0].iov_base = (void *)(r->data + pos);
  iov[0].iov_len  = first;
  iov[1].iov_base = (void *)r->data;
  iov[1].iov_len  = readable - first;
  return 2;
}

int xRingBufferWriteIov(xRingBuffer rb, struct iovec iov[2]) {
  xRingBuffer_ *r = (xRingBuffer_ *)rb;
  size_t        writable, pos, first;

  if (!r) return 0;

  writable = r->cap - (r->head - r->tail);
  if (writable == 0) return 0;

  pos   = r->head & r->mask;
  first = r->cap - pos;

  if (writable <= first) {
    iov[0].iov_base = (void *)(r->data + pos);
    iov[0].iov_len  = writable;
    return 1;
  }

  iov[0].iov_base = (void *)(r->data + pos);
  iov[0].iov_len  = first;
  iov[1].iov_base = (void *)r->data;
  iov[1].iov_len  = writable - first;
  return 2;
}

ssize_t xRingBufferReadFd(xRingBuffer rb, int fd) {
  xRingBuffer_ *r = (xRingBuffer_ *)rb;
  struct iovec  iov[2];
  ssize_t       n;
  int           cnt;

  if (!r) return -1;

  cnt = xRingBufferWriteIov(rb, iov);
  if (cnt == 0) return 0; /* full */

  do {
    n = readv(fd, iov, cnt);
  } while (n < 0 && errno == EINTR);
  if (n > 0) r->head += (size_t)n;
  return n;
}

ssize_t xRingBufferWriteFd(xRingBuffer rb, int fd) {
  xRingBuffer_ *r = (xRingBuffer_ *)rb;
  struct iovec  iov[2];
  ssize_t       n;
  int           cnt;

  if (!r) return -1;

  cnt = xRingBufferReadIov(rb, iov);
  if (cnt == 0) return 0; /* empty */

  do {
    n = writev(fd, iov, cnt);
  } while (n < 0 && errno == EINTR);
  if (n > 0) r->tail += (size_t)n;
  return n;
}
