/*
 * Copyright 2025 The xKit Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * io.c - Reference-counted block-chain I/O buffer implementation
 */

#include <xbase/atomic.h>
#include <xbuf/io.h>

#include <assert.h>
#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sys/uio.h>
#include <unistd.h>

/* ═══════════════════════════════════════════════════════
 *  Block pool — lock-free stack (Treiber stack)
 * ═══════════════════════════════════════════════════════ */

/*
 * PoolNode_ overlays xIOBlock to form a lock-free Treiber stack.
 * Only the first pointer-sized field (reused as `next`) is accessed in pool
 * state. When a block is in the pool, refs/size/data are stale and must not be
 * read.
 */
XDEF_STRUCT(PoolNode_) {
  PoolNode_ *next;
};

static PoolNode_ *volatile g_pool_head = NULL;

static void pool_push(xIOBlock *blk) {
  PoolNode_ *node = (PoolNode_ *)blk; /* reuse block memory as node */
  PoolNode_ *head;
  do {
    head       = xAtomicLoad(&g_pool_head, xAtomicAcquire);
    node->next = head;
  } while (!xAtomicCasWeak(&g_pool_head, &head, node, xAtomicRelease));
}

static xIOBlock *pool_pop(void) {
  PoolNode_ *head;
  PoolNode_ *next;
  do {
    head = xAtomicLoad(&g_pool_head, xAtomicAcquire);
    if (!head) return NULL;
    next = head->next;
  } while (!xAtomicCasWeak(&g_pool_head, &head, next, xAtomicRelease));
  return (xIOBlock *)head;
}

/* ═══════════════════════════════════════════════════════
 *  xIOBlock
 * ═══════════════════════════════════════════════════════ */

xIOBlock *xIOBlockAcquire(void) {
  xIOBlock *blk = pool_pop();
  if (!blk) {
    blk = (xIOBlock *)malloc(sizeof(xIOBlock));
    if (!blk) return NULL;
  }
  blk->refs = 1;
  blk->size = 0;
  return blk;
}

void xIOBlockRetain(xIOBlock *blk) {
  if (!blk) return;
  xAtomicAdd(&blk->refs, 1, xAtomicSeqCst);
}

void xIOBlockRelease(xIOBlock *blk) {
  if (!blk) return;
  if (xAtomicSub(&blk->refs, 1, xAtomicSeqCst) == 0) {
    pool_push(blk); /* return to pool instead of free */
  }
}

xErrno xIOBlockPoolWarmup(size_t n) {
  size_t i;
  for (i = 0; i < n; i++) {
    xIOBlock *blk = (xIOBlock *)malloc(sizeof(xIOBlock));
    if (!blk) return xErrno_NoMemory;
    blk->refs = 0;
    pool_push(blk);
  }
  return xErrno_Ok;
}

/**
 * Drain all pooled blocks back to the OS.
 *
 * Invariant: blocks enter the pool only via xIOBlockRelease (refs decremented
 * to zero) or xIOBlockPoolWarmup (never handed out).  Callers must ensure no
 * outstanding references exist before calling this function; otherwise the
 * application has a block-leak bug.
 *
 * Note: we cannot assert refs == 0 here because the PoolNode_ overlay reuses
 * the refs field as the `next` pointer while the block sits in the pool.
 */
void xIOBlockPoolDrain(void) {
  xIOBlock *blk;
  while ((blk = pool_pop()) != NULL) {
    free(blk);
  }
}

/* ═══════════════════════════════════════════════════════
 *  IOBuf ref array management
 * ═══════════════════════════════════════════════════════ */

static xErrno iobuf_grow_refs(xIOBuffer *io, size_t needed) {
  size_t        newcap;
  xIOBufferRef *newrefs;

  if (needed <= io->cap) return xErrno_Ok;

  newcap = io->cap ? io->cap * 2 : XIOBUFFER_INLINE_REFS;
  while (newcap < needed)
    newcap *= 2;

  if (io->refs == io->inlined) {
    /* Transition from inline to heap. */
    newrefs = (xIOBufferRef *)malloc(newcap * sizeof(xIOBufferRef));
    if (!newrefs) return xErrno_NoMemory;
    if (io->nrefs > 0)
      memcpy(newrefs, io->inlined, io->nrefs * sizeof(xIOBufferRef));
  } else {
    newrefs = (xIOBufferRef *)realloc(io->refs, newcap * sizeof(xIOBufferRef));
    if (!newrefs) return xErrno_NoMemory;
  }

  io->refs = newrefs;
  io->cap  = newcap;
  return xErrno_Ok;
}

/**
 * Append a ref to the ref array.
 * If `adjust_nbytes` is true, io->nbytes is incremented by `length`.
 * Callers that manage nbytes externally (e.g. xIOBufferCut) pass false.
 */
static xErrno iobuf_push_ref(xIOBuffer *io, xIOBlock *blk, size_t offset,
                             size_t length, bool adjust_nbytes) {
  xErrno err;

  if (length == 0) return xErrno_Ok;

  err = iobuf_grow_refs(io, io->nrefs + 1);
  if (err != xErrno_Ok) return err;

  io->refs[io->nrefs].block  = blk;
  io->refs[io->nrefs].offset = offset;
  io->refs[io->nrefs].length = length;
  io->nrefs++;
  if (adjust_nbytes) io->nbytes += length;
  return xErrno_Ok;
}

/** Remove the first `count` refs, shifting the rest down. */
static void iobuf_shift_refs(xIOBuffer *io, size_t count) {
  if (count == 0) return;
  if (count >= io->nrefs) {
    io->nrefs = 0;
    return;
  }
  memmove(io->refs, io->refs + count,
          (io->nrefs - count) * sizeof(xIOBufferRef));
  io->nrefs -= count;
}

/* ═══════════════════════════════════════════════════════
 *  Lifecycle
 * ═══════════════════════════════════════════════════════ */

void xIOBufferInit(xIOBuffer *io) {
  if (!io) return;
  io->refs   = io->inlined;
  io->nrefs  = 0;
  io->cap    = XIOBUFFER_INLINE_REFS;
  io->nbytes = 0;
}

void xIOBufferDeinit(xIOBuffer *io) {
  size_t i;
  if (!io) return;

  for (i = 0; i < io->nrefs; i++)
    xIOBlockRelease(io->refs[i].block);

  if (io->refs != io->inlined) free(io->refs);

  io->refs   = io->inlined;
  io->nrefs  = 0;
  io->cap    = XIOBUFFER_INLINE_REFS;
  io->nbytes = 0;
}

void xIOBufferReset(xIOBuffer *io) {
  size_t i;
  if (!io) return;

  for (i = 0; i < io->nrefs; i++)
    xIOBlockRelease(io->refs[i].block);

  io->nrefs  = 0;
  io->nbytes = 0;
}

/* ═══════════════════════════════════════════════════════
 *  Query
 * ═══════════════════════════════════════════════════════ */

size_t xIOBufferLen(const xIOBuffer *io) {
  return io ? io->nbytes : 0;
}

bool xIOBufferEmpty(const xIOBuffer *io) {
  return !io || io->nbytes == 0;
}

size_t xIOBufferRefCount(const xIOBuffer *io) {
  return io ? io->nrefs : 0;
}

/* ═══════════════════════════════════════════════════════
 *  Write (append)
 * ═══════════════════════════════════════════════════════ */

xErrno xIOBufferAppend(xIOBuffer *io, const void *data, size_t len) {
  const char   *src = (const char *)data;
  xIOBufferRef *tail;
  xIOBlock     *blk;
  size_t        avail, chunk;
  xErrno        err;

  if (!io) return xErrno_InvalidArg;
  if (len == 0) return xErrno_Ok;

  /* Try to fill the tail block's remaining space first. */
  if (io->nrefs > 0) {
    tail  = &io->refs[io->nrefs - 1];
    avail = XIOBUFFER_BLOCK_SIZE - (tail->offset + tail->length);
    if (avail > 0) {
      chunk = len < avail ? len : avail;
      memcpy(tail->block->data + tail->offset + tail->length, src, chunk);
      tail->length += chunk;
      io->nbytes += chunk;
      src += chunk;
      len -= chunk;
    }
  }

  /* Allocate new blocks for the remaining data. */
  while (len > 0) {
    blk = xIOBlockAcquire();
    if (!blk) return xErrno_NoMemory;

    chunk = len < XIOBUFFER_BLOCK_SIZE ? len : XIOBUFFER_BLOCK_SIZE;
    memcpy(blk->data, src, chunk);
    blk->size = chunk;

    err = iobuf_push_ref(io, blk, 0, chunk, true);
    if (err != xErrno_Ok) {
      xIOBlockRelease(blk);
      return err;
    }

    src += chunk;
    len -= chunk;
  }

  return xErrno_Ok;
}

xErrno xIOBufferAppendStr(xIOBuffer *io, const char *str) {
  if (!io || !str) return xErrno_InvalidArg;
  return xIOBufferAppend(io, str, strlen(str));
}

xErrno xIOBufferAppendIOBuffer(xIOBuffer *io, xIOBuffer *other) {
  xErrno err;
  size_t i;

  if (!io || !other) return xErrno_InvalidArg;
  if (other->nrefs == 0) return xErrno_Ok;

  err = iobuf_grow_refs(io, io->nrefs + other->nrefs);
  if (err != xErrno_Ok) return err;

  /* Transfer refs (no refcount bump — ownership moves). */
  for (i = 0; i < other->nrefs; i++)
    io->refs[io->nrefs + i] = other->refs[i];

  io->nrefs += other->nrefs;
  io->nbytes += other->nbytes;

  /* Clear other without releasing blocks (ownership transferred). */
  other->nrefs  = 0;
  other->nbytes = 0;

  return xErrno_Ok;
}

/* ═══════════════════════════════════════════════════════
 *  Read (consume)
 * ═══════════════════════════════════════════════════════ */

size_t xIOBufferRead(xIOBuffer *io, void *out, size_t len) {
  char         *dst   = (char *)out;
  size_t        total = 0;
  size_t        shift = 0;
  size_t        chunk;
  xIOBufferRef *ref;

  if (!io || !out || len == 0) return 0;

  if (len > io->nbytes) len = io->nbytes;

  while (total < len && shift < io->nrefs) {
    ref   = &io->refs[shift];
    chunk = ref->length;
    if (chunk > len - total) chunk = len - total;

    memcpy(dst + total, ref->block->data + ref->offset, chunk);
    total += chunk;

    if (chunk == ref->length) {
      /* Fully consumed this ref. */
      xIOBlockRelease(ref->block);
      shift++;
    } else {
      /* Partially consumed. */
      ref->offset += chunk;
      ref->length -= chunk;
    }
  }

  if (shift > 0) iobuf_shift_refs(io, shift);

  io->nbytes -= total;
  return total;
}

size_t xIOBufferCut(xIOBuffer *io, xIOBuffer *dst, size_t n) {
  size_t        total = 0;
  size_t        shift = 0;
  size_t        chunk;
  xIOBufferRef *ref;
  xErrno        err;

  if (!io || !dst || n == 0) return 0;

  if (n > io->nbytes) n = io->nbytes;

  while (total < n && shift < io->nrefs) {
    ref   = &io->refs[shift];
    chunk = ref->length;

    if (chunk <= n - total) {
      /* Move entire ref to dst (transfer ownership, no refcount change). */
      err = iobuf_push_ref(dst, ref->block, ref->offset, ref->length, false);
      if (err != xErrno_Ok) break;
      total += chunk;
      shift++;
    } else {
      /* Split: partial ref. Need to share the block. */
      chunk = n - total;
      xIOBlockRetain(ref->block);
      err = iobuf_push_ref(dst, ref->block, ref->offset, chunk, false);
      if (err != xErrno_Ok) {
        xIOBlockRelease(ref->block);
        break;
      }
      ref->offset += chunk;
      ref->length -= chunk;
      total += chunk;
    }
  }

  /* Shift consumed refs out of io. */
  if (shift > 0) iobuf_shift_refs(io, shift);

  io->nbytes -= total;
  dst->nbytes += total;
  return total;
}

size_t xIOBufferConsume(xIOBuffer *io, size_t n) {
  size_t        total = 0;
  size_t        shift = 0;
  size_t        chunk;
  xIOBufferRef *ref;

  if (!io || n == 0) return 0;

  if (n > io->nbytes) n = io->nbytes;

  while (total < n && shift < io->nrefs) {
    ref   = &io->refs[shift];
    chunk = ref->length;

    if (chunk <= n - total) {
      xIOBlockRelease(ref->block);
      total += chunk;
      shift++;
    } else {
      chunk = n - total;
      ref->offset += chunk;
      ref->length -= chunk;
      total += chunk;
    }
  }

  if (shift > 0) iobuf_shift_refs(io, shift);

  io->nbytes -= total;
  return total;
}

/* ═══════════════════════════════════════════════════════
 *  Linearize
 * ═══════════════════════════════════════════════════════ */

size_t xIOBufferCopyTo(const xIOBuffer *io, void *out) {
  char  *dst   = (char *)out;
  size_t total = 0;
  size_t i;

  if (!io || !out) return 0;

  for (i = 0; i < io->nrefs; i++) {
    const xIOBufferRef *ref = &io->refs[i];
    memcpy(dst + total, ref->block->data + ref->offset, ref->length);
    total += ref->length;
  }
  return total;
}

/* ═══════════════════════════════════════════════════════
 *  I/O helpers
 * ═══════════════════════════════════════════════════════ */

int xIOBufferReadIov(const xIOBuffer *io, struct iovec *iov, int max_iov) {
  int    cnt = 0;
  size_t i;

  if (!io || !iov || max_iov <= 0) return 0;

  for (i = 0; i < io->nrefs && cnt < max_iov; i++) {
    const xIOBufferRef *ref = &io->refs[i];
    if (ref->length == 0) continue;
    iov[cnt].iov_base = ref->block->data + ref->offset;
    iov[cnt].iov_len  = ref->length;
    cnt++;
  }
  return cnt;
}

ssize_t xIOBufferReadFd(xIOBuffer *io, int fd) {
  xIOBlock     *blk;
  xIOBufferRef *tail;
  size_t        avail;
  ssize_t       n;
  xErrno        err;

  if (!io) return -1;

  /* Try to read into the tail block's remaining space. */
  if (io->nrefs > 0) {
    tail  = &io->refs[io->nrefs - 1];
    avail = XIOBUFFER_BLOCK_SIZE - (tail->offset + tail->length);
    if (avail > 0) {
      do {
        n = read(fd, tail->block->data + tail->offset + tail->length, avail);
      } while (n < 0 && errno == EINTR);
      if (n > 0) {
        tail->length += (size_t)n;
        io->nbytes += (size_t)n;
      }
      return n;
    }
  }

  /* Allocate a new block. */
  blk = xIOBlockAcquire();
  if (!blk) return -1;

  do {
    n = read(fd, blk->data, XIOBUFFER_BLOCK_SIZE);
  } while (n < 0 && errno == EINTR);
  if (n <= 0) {
    xIOBlockRelease(blk);
    return n;
  }

  blk->size = (size_t)n;
  err       = iobuf_push_ref(io, blk, 0, (size_t)n, true);
  if (err != xErrno_Ok) {
    xIOBlockRelease(blk);
    return -1;
  }

  return n;
}

ssize_t xIOBufferWriteFd(xIOBuffer *io, int fd) {
#ifndef IOV_MAX
#define IOV_MAX 1024
#endif
  struct iovec iov[IOV_MAX < 64 ? IOV_MAX : 64];
  int          cnt;
  ssize_t      n;

  if (!io) return -1;

  cnt = xIOBufferReadIov(io, iov, (int)(sizeof(iov) / sizeof(iov[0])));
  if (cnt == 0) return 0;

  do {
    n = writev(fd, iov, cnt);
  } while (n < 0 && errno == EINTR);
  if (n > 0) xIOBufferConsume(io, (size_t)n);
  return n;
}
