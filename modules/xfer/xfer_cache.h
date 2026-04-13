/*
 * Copyright 2025 The xKit Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * xfer_cache.h - Buffered write cache for P2P file transfer
 *
 * Buffers incoming file chunks in memory (mmap-backed) and flushes
 * to disk in batches to reduce I/O overhead during high-throughput
 * transfers.
 *
 * This module is used by the xfer receiver to accumulate chunks before
 * writing to disk, avoiding a per-chunk fwrite+flush on the hot path.
 *
 * Usage:
 *   xTransferCache c = xTransferCacheCreate(fd, file_size, chunk_size,
 *                                 batch_flush, flush_ms);
 *   // on chunk arrival:
 *   xTransferCachePut(c, chunk_id, data, data_len);
 *   // on transfer done:
 *   xTransferCacheFlush(c, sync);  // sync=true: msync(MS_SYNC)
 *   xTransferCacheDestroy(c);
 */

#ifndef XFER_TRANSFER_CACHE_H
#define XFER_TRANSFER_CACHE_H

#include <xbase/error.h>

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

/* ───────────────────── Handle ───────────────────── */

/**
 * @brief Buffered write cache (opaque handle).
 *
 * Internally backed by mmap for fast in-memory writes, with a
 * configurable batch flush policy.
 */
XDEF_HANDLE(xTransferCache);

/* ───────────────────── Configuration ───────────────────── */

/**
 * @brief Default number of chunks to accumulate before auto-flushing.
 */
#define XFER_TRANSFER_CACHE_DEFAULT_BATCH 16

/**
 * @brief Default flush interval in milliseconds (0 = disabled).
 */
#define XFER_TRANSFER_CACHE_DEFAULT_FLUSH_MS 500

/**
 * @brief Minimum file size to enable caching (bytes).
 * Files smaller than this are written immediately without caching.
 */
#define XFER_TRANSFER_CACHE_MIN_FILE_SIZE (64 * 1024) /* 64 KB */

/* ───────────────────── Lifecycle ───────────────────── */

/**
 * @brief Create a buffered write cache for the given file.
 *
 * The file must already exist and be pre-allocated to @p file_size
 * (e.g. via ftruncate or sparse file seek+write).  This function
 * mmaps the entire file for direct memory access.
 *
 * @param fd         Open file descriptor (must be writable, O_RDWR).
 *                    The caller retains ownership; the cache does not
 *                    close @p fd on destroy.
 * @param file_size  Total size of the file in bytes.
 * @param chunk_size Chunk size in bytes (used to compute file offsets).
 * @param batch      Number of chunks to accumulate before auto-flushing.
 *                   Pass 0 to use XFER_TRANSFER_CACHE_DEFAULT_BATCH.
 * @param flush_ms   Auto-flush interval in milliseconds. Pass 0 to disable.
 * @return A new cache, or NULL on mmap failure.
 */
XCAPI(xTransferCache)
xTransferCacheCreate(int fd, uint64_t file_size, uint32_t chunk_size,
                uint32_t batch, uint32_t flush_ms);

/**
 * @brief Destroy the cache and release all resources.
 *
 * Automatically performs a sync flush before munmap if there are
 * pending bytes.  Does NOT close the underlying fd.
 *
 * @param cache  Cache to destroy, or NULL (no-op).
 */
XCAPI(void) xTransferCacheDestroy(xTransferCache cache);

/* ───────────────────── Write ───────────────────── */

/**
 * @brief Write a chunk to the cache.
 *
 * Copies the chunk data directly into the mmap'd region at the
 * correct file offset (chunk_id * chunk_size).  If the internal
 * batch threshold is reached, an async flush is triggered automatically.
 *
 * @param cache     Cache, or NULL (no-op).
 * @param chunk_id Zero-based chunk index.
 * @param data     Chunk payload (must not be NULL).
 * @param len      Number of bytes in @p data (must not exceed chunk_size).
 * @return xErrno_Ok on success, xErrno_InvalidArg if @p len exceeds
 *         the configured chunk_size.
 */
XCAPI(xErrno) xTransferCachePut(xTransferCache cache, uint32_t chunk_id,
                            const void *data, uint32_t len);

/* ───────────────────── Flush ───────────────────── */

/**
 * @brief Flush all pending writes to disk.
 *
 * Calls msync(MS_ASYNC) to enqueue the pages for writing by the kernel.
 * If @p sync is true, also calls msync(MS_SYNC) to wait for completion
 * before returning, ensuring data is on disk.
 *
 * @param cache  Cache, or NULL (no-op).
 * @param sync  If true, wait for the kernel to finish writing (MS_SYNC).
 *               If false, return immediately after enqueueing (MS_ASYNC).
 */
XCAPI(void) xTransferCacheFlush(xTransferCache cache, bool sync);

/**
 * @brief Return the number of bytes currently held in the cache
 *        (written but not yet flushed).
 *
 * @param cache  Cache, or NULL.
 * @return Pending byte count, or 0 if cache is NULL.
 */
XCAPI(uint64_t) xTransferCachePendingBytes(xTransferCache cache);

#endif /* XFER_TRANSFER_CACHE_H */
