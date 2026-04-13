/*
 * Copyright 2025 The xKit Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * xfer_cache.c - Buffered write cache implementation
 *
 * Uses mmap for fast in-memory writes and batches msync calls to reduce
 * I/O overhead during high-throughput P2P transfers.
 *
 * Strategy:
 *   - mmap the entire file on creation (file must be pre-allocated)
 *   - on xTransferCachePut: memcpy to mmap region + update pending count
 *   - when batch threshold reached: msync(MS_ASYNC)
 *   - caller-driven flush on done: msync(MS_SYNC)
 */

#include <xfer/xfer_cache.h>

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

/* ───────────────────── Types ───────────────────── */

XDEF_STRUCT(xTransferCache_) {
  int      fd;
  void    *mmap_base;
  uint64_t mmap_len;
  uint32_t chunk_size;
  uint32_t batch;
  uint32_t pending_chunks;
  uint64_t pending_bytes;
  uint64_t pending_max_offset; /* highest (chunk_id*chunk_size + len) written */
  bool     use_cache;
};

/* ───────────────────── Flush ───────────────────── */

void xTransferCacheFlush(xTransferCache cache, bool sync) {
  xTransferCache_ *c = (xTransferCache_ *)cache;
  if (!c || !c->use_cache || c->pending_bytes == 0) return;

  if (msync(c->mmap_base, c->pending_max_offset,
            sync ? MS_SYNC : MS_ASYNC) != 0) {
    /* best-effort: log but don't propagate */
  }
  c->pending_chunks     = 0;
  c->pending_bytes      = 0;
  c->pending_max_offset = 0;
}

/* ───────────────────── Internal ───────────────────── */

static void cache_flush_batch(xTransferCache_ *c) {
  if (!c->use_cache || c->pending_bytes == 0) return;
  msync(c->mmap_base, c->pending_max_offset, MS_ASYNC);
  c->pending_chunks     = 0;
  c->pending_bytes      = 0;
  c->pending_max_offset = 0;
}

/* ───────────────────── Lifecycle ───────────────────── */

xTransferCache xTransferCacheCreate(int fd, uint64_t file_size, uint32_t chunk_size,
                          uint32_t batch, uint32_t flush_ms) {
  (void)flush_ms; /* reserved for future timer-based flush */

  xTransferCache_ *c = (xTransferCache_ *)calloc(1, sizeof(xTransferCache_));
  if (!c) return NULL;

  c->fd         = fd;
  c->chunk_size = chunk_size;
  c->batch     = batch > 0 ? batch : XFER_TRANSFER_CACHE_DEFAULT_BATCH;

  /* Disable caching for small files. */
  c->use_cache = (file_size >= XFER_TRANSFER_CACHE_MIN_FILE_SIZE);

  if (c->use_cache) {
    c->mmap_len = (size_t)file_size;
    if (c->mmap_len != file_size) {
      /* File too large to mmap on this platform. */
      c->use_cache = false;
    }
  }

  if (c->use_cache) {
    c->mmap_base = mmap(NULL, c->mmap_len, PROT_WRITE, MAP_SHARED, fd, 0);
    if (c->mmap_base == MAP_FAILED) {
      free(c);
      return NULL;
    }
  }

  return (xTransferCache)c;
}

void xTransferCacheDestroy(xTransferCache cache) {
  xTransferCache_ *c = (xTransferCache_ *)cache;
  if (!c) return;

  if (c->use_cache) {
    if (c->pending_bytes > 0) {
      msync(c->mmap_base, c->pending_max_offset, MS_SYNC);
    }
    munmap(c->mmap_base, c->mmap_len);
  }

  free(c);
}

/* ───────────────────── Write ───────────────────── */

xErrno xTransferCachePut(xTransferCache cache, uint32_t chunk_id,
                    const void *data, uint32_t len) {
  xTransferCache_ *c = (xTransferCache_ *)cache;
  if (!c) return xErrno_InvalidArg;

  if (len > c->chunk_size) return xErrno_InvalidArg;

  if (!c->use_cache) {
    off_t offset = (off_t)(uint64_t)chunk_id * c->chunk_size;
    if (lseek(c->fd, offset, SEEK_SET) < 0) return xErrno_SysError;
    ssize_t written = write(c->fd, data, len);
    if (written < 0) return xErrno_SysError;
    return xErrno_Ok;
  }

  uint64_t file_offset = (uint64_t)chunk_id * c->chunk_size;
  memcpy((uint8_t *)c->mmap_base + file_offset, data, len);

  c->pending_bytes += len;
  c->pending_chunks++;

  uint64_t chunk_end = file_offset + len;
  if (chunk_end > c->pending_max_offset)
    c->pending_max_offset = chunk_end;

  if (c->pending_chunks >= c->batch)
    cache_flush_batch(c);

  return xErrno_Ok;
}

/* ───────────────────── Query ───────────────────── */

uint64_t xTransferCachePendingBytes(xTransferCache cache) {
  xTransferCache_ *c = (xTransferCache_ *)cache;
  if (!c) return 0;
  return c->pending_bytes;
}
