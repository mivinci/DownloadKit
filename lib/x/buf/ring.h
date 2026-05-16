/*
 * Copyright 2025 The moo Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * ring.h - Fixed-size ring (circular) buffer
 *
 * xRingBuffer is a fixed-capacity circular buffer. It never reallocates;
 * writes that would exceed capacity return xErrno_NoMemory (or a
 * partial-write variant for I/O helpers).
 *
 * The capacity is rounded up to the next power of two internally so
 * that index masking can replace modulo operations.
 *
 * The buffer header and data area are allocated in a single malloc()
 * call using a flexible array member, avoiding an extra indirection.
 *
 * Typical usage:
 *   xRingBuffer rb = xRingBufferCreate(8192);
 *   xRingBufferWrite(rb, data, len);
 *   xRingBufferRead(rb, out, sizeof(out));
 *   xRingBufferDestroy(rb);
 */

#ifndef XBUF_RING_H
#define XBUF_RING_H

#include <x/base/base.h>
#include <x/base/error.h>

#include <stddef.h>
#include <sys/types.h>
#include <sys/uio.h>

/* ───────────────────── Types ───────────────────── */

/**
 * @brief Fixed-size ring buffer (opaque handle).
 *
 * Internally uses a power-of-two capacity with bitmask indexing.
 * `head` is the next write position, `tail` is the next read position.
 * Both grow monotonically; the actual index is obtained via & mask.
 *
 * The struct is allocated together with its data area via a flexible
 * array member (single malloc).
 */
XDEF_HANDLE(xRingBuffer);

/* ───────────────────── Lifecycle ───────────────────── */

/**
 * @brief Create a ring buffer with at least @p min_cap bytes.
 *
 * The actual capacity is rounded up to the next power of two.
 * Header and data area are allocated in one malloc() call.
 *
 * @param min_cap  Minimum requested capacity (must be > 0).
 * @return A new ring buffer, or NULL on allocation failure or invalid arg.
 */
XCAPI(xRingBuffer) xRingBufferCreate(size_t min_cap);

/**
 * @brief Release all memory owned by the ring buffer.
 *
 * @param rb  Ring buffer to destroy, or NULL (no-op).
 */
XCAPI(void) xRingBufferDestroy(xRingBuffer rb);

/**
 * @brief Discard all data but keep the allocated memory.
 *
 * @param rb  Ring buffer (must not be NULL).
 */
XCAPI(void) xRingBufferReset(xRingBuffer rb);

/* ───────────────────── Query ───────────────────── */

/**
 * @brief Return the number of readable bytes.
 */
XCAPI(size_t) xRingBufferLen(xRingBuffer rb);

/**
 * @brief Return the total capacity.
 */
XCAPI(size_t) xRingBufferCap(xRingBuffer rb);

/**
 * @brief Return the number of writable bytes.
 */
XCAPI(size_t) xRingBufferWritable(xRingBuffer rb);

/**
 * @brief Return true if the ring buffer is empty.
 */
XCAPI(bool) xRingBufferEmpty(xRingBuffer rb);

/**
 * @brief Return true if the ring buffer is full.
 */
XCAPI(bool) xRingBufferFull(xRingBuffer rb);

/* ───────────────────── Write ───────────────────── */

/**
 * @brief Write bytes into the ring buffer.
 *
 * Writes as many bytes as possible. If the buffer has insufficient
 * space, a partial write is performed.
 *
 * @param rb    Ring buffer (must not be NULL).
 * @param data  Source bytes.
 * @param len   Number of bytes to write.
 * @return Number of bytes actually written (may be less than @p len).
 */
XCAPI(size_t) xRingBufferWrite(xRingBuffer rb, const void *data, size_t len);

/* ───────────────────── Read ───────────────────── */

/**
 * @brief Read and consume bytes from the ring buffer.
 *
 * Copies up to @p len bytes into @p out and advances the read cursor.
 *
 * @param rb   Ring buffer (must not be NULL).
 * @param out  Destination buffer.
 * @param len  Maximum bytes to read.
 * @return Number of bytes actually read.
 */
XCAPI(size_t) xRingBufferRead(xRingBuffer rb, void *out, size_t len);

/**
 * @brief Peek at readable data without consuming it.
 *
 * Copies up to @p len bytes into @p out without advancing the cursor.
 *
 * @param rb   Ring buffer (must not be NULL).
 * @param out  Destination buffer.
 * @param len  Maximum bytes to peek.
 * @return Number of bytes actually copied.
 */
XCAPI(size_t) xRingBufferPeek(xRingBuffer rb, void *out, size_t len);

/**
 * @brief Discard up to @p n readable bytes.
 *
 * @param rb  Ring buffer (must not be NULL).
 * @param n   Number of bytes to discard.
 * @return Number of bytes actually discarded.
 */
XCAPI(size_t) xRingBufferDiscard(xRingBuffer rb, size_t n);

/* ───────────────────── I/O helpers ───────────────────── */

/**
 * @brief Fill iovec array for writev() with readable data.
 *
 * Because the readable region may wrap around, up to 2 iovec entries
 * are needed.
 *
 * @param rb   Ring buffer (must not be NULL).
 * @param iov  Array of at least 2 iovec structs.
 * @return Number of iovec entries filled (0, 1, or 2).
 */
XCAPI(int) xRingBufferReadIov(xRingBuffer rb, struct iovec iov[2]);

/**
 * @brief Fill iovec array for readv() with writable regions.
 *
 * @param rb   Ring buffer (must not be NULL).
 * @param iov  Array of at least 2 iovec structs.
 * @return Number of iovec entries filled (0, 1, or 2).
 */
XCAPI(int) xRingBufferWriteIov(xRingBuffer rb, struct iovec iov[2]);

/**
 * @brief Read from a file descriptor into the ring buffer.
 *
 * Uses readv() to handle wrap-around efficiently.
 *
 * @param rb  Ring buffer (must not be NULL).
 * @param fd  File descriptor.
 * @return Bytes read, 0 on EOF, -1 on error.
 */
XCAPI(ssize_t) xRingBufferReadFd(xRingBuffer rb, int fd);

/**
 * @brief Write readable data from the ring buffer to a file descriptor.
 *
 * Uses writev() to handle wrap-around efficiently.
 *
 * @param rb  Ring buffer (must not be NULL).
 * @param fd  File descriptor.
 * @return Bytes written, or -1 on error.
 */
XCAPI(ssize_t) xRingBufferWriteFd(xRingBuffer rb, int fd);

#endif /* XBUF_RING_H */
