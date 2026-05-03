/*
 * Copyright 2025 The xKit Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * io.h - Reference-counted block-chain I/O buffer (brpc IOBuf style)
 *
 * xIOBuffer is a non-contiguous byte buffer composed of a chain of
 * reference-counted memory blocks. It supports zero-copy split, append,
 * and scatter-gather I/O (readv/writev).
 *
 * Architecture:
 *
 *   xIOBuffer  = [ xIOBufferRef, xIOBufferRef, xIOBufferRef, ... ]
 *                  ↓            ↓            ↓
 *              xIOBlock_A   xIOBlock_B   xIOBlock_C   (ref-counted, shared)
 *
 *   xIOBlock:  fixed-size memory block (default 8KB), reference counted.
 *   xIOBufferRef: { block*, offset, length } — a slice into a block.
 *   xIOBuffer:    a dynamic array of xIOBufferRef, representing a logical byte
 * stream.
 *
 * Key properties:
 *   - Append two xIOBuffers: O(1) — just concatenate ref arrays, bump
 * refcounts.
 *   - Split/cut N bytes from front: O(k) where k = number of refs touched.
 *   - writev: each ref maps directly to an iovec entry.
 *   - Blocks are pooled in a global freelist for reuse.
 *
 * Typical usage:
 *   xIOBuffer io;
 *   xIOBufferInit(&io);
 *   xIOBufferAppend(&io, data, len);
 *   xIOBufferReadFd(&io, fd);
 *   xIOBufferCut(&io, &header_buf, header_len);
 *   xIOBufferWriteFd(&io, fd);
 *   xIOBufferDeinit(&io);
 */

#ifndef XBUF_IO_H
#define XBUF_IO_H

#include <xbase/base.h>
#include <xbase/error.h>
#include <xbase/uio.h>

#include <stddef.h>
#include <sys/types.h>

/* ───────────────────── Configuration ───────────────────── */

/** Default block size (8 KB). */
#ifndef XIOBUFFER_BLOCK_SIZE
#define XIOBUFFER_BLOCK_SIZE 8192
#endif

/** Initial inline capacity for the ref array (avoid malloc for small bufs). */
#ifndef XIOBUFFER_INLINE_REFS
#define XIOBUFFER_INLINE_REFS 8
#endif

/* ───────────────────── Block ───────────────────── */

/**
 * @brief Reference-counted memory block.
 *
 * Blocks are fixed-size and managed by a global freelist.
 * Users should not manipulate blocks directly.
 */
XDEF_STRUCT(xIOBlock) {
  size_t refs;                       /**< Reference count (atomic)     */
  size_t size;                       /**< Usable data size in bytes    */
  char   data[XIOBUFFER_BLOCK_SIZE]; /**< Inline data storage          */
};

/**
 * @brief Acquire a block from the global pool (or allocate a new one).
 * @return A block with refcount == 1, or NULL on OOM.
 */
XCAPI(xIOBlock *) xIOBlockAcquire(void);

/**
 * @brief Increment the reference count of a block.
 */
XCAPI(void) xIOBlockRetain(xIOBlock *blk);

/**
 * @brief Decrement the reference count; return to pool when it hits zero.
 */
XCAPI(void) xIOBlockRelease(xIOBlock *blk);

/* ───────────────────── BlockRef ───────────────────── */

/**
 * @brief A slice reference into an xIOBlock.
 */
XDEF_STRUCT(xIOBufferRef) {
  xIOBlock *block;  /**< The underlying block (ref-counted) */
  size_t    offset; /**< Start offset within block->data    */
  size_t    length; /**< Number of valid bytes from offset   */
};

/* ───────────────────── IOBuf ───────────────────── */

/**
 * @brief Block-chain I/O buffer.
 *
 * Maintains a dynamic array of xIOBufferRef. For small buffers the
 * inline array avoids heap allocation.
 */
XDEF_STRUCT(xIOBuffer) {
  xIOBufferRef  inlined[XIOBUFFER_INLINE_REFS]; /**< Inline ref storage       */
  xIOBufferRef *refs;   /**< Pointer to ref array (inlined or heap)        */
  size_t        nrefs;  /**< Number of active refs                         */
  size_t        cap;    /**< Capacity of the refs array                    */
  size_t        nbytes; /**< Total logical byte count (cached)             */
};

/* ───────────────────── Lifecycle ───────────────────── */

/**
 * @brief Initialize an empty IOBuf.
 *
 * @param io  IOBuf to initialize (must not be NULL).
 */
XCAPI(void) xIOBufferInit(xIOBuffer *io);

/**
 * @brief Release all block references and free the ref array.
 *
 * @param io  IOBuf to deinitialize, or NULL (no-op).
 */
XCAPI(void) xIOBufferDeinit(xIOBuffer *io);

/**
 * @brief Discard all data (release all refs) but keep the ref array.
 *
 * @param io  IOBuf (must not be NULL).
 */
XCAPI(void) xIOBufferReset(xIOBuffer *io);

/* ───────────────────── Query ───────────────────── */

/**
 * @brief Return the total number of readable bytes.
 */
XCAPI(size_t) xIOBufferLen(const xIOBuffer *io);

/**
 * @brief Return true if the IOBuf contains no data.
 */
XCAPI(bool) xIOBufferEmpty(const xIOBuffer *io);

/**
 * @brief Return the number of block refs (segments).
 */
XCAPI(size_t) xIOBufferRefCount(const xIOBuffer *io);

/* ───────────────────── Write (append) ───────────────────── */

/**
 * @brief Append raw bytes, allocating new blocks as needed.
 *
 * @param io    IOBuf (must not be NULL).
 * @param data  Source bytes.
 * @param len   Number of bytes to append.
 * @return xErrno_Ok on success, xErrno_NoMemory on failure.
 */
XCAPI(xErrno) xIOBufferAppend(xIOBuffer *io, const void *data, size_t len);

/**
 * @brief Append a NUL-terminated C string (excluding the NUL).
 *
 * @param io   IOBuf (must not be NULL).
 * @param str  C string to append.
 * @return xErrno_Ok on success, xErrno_NoMemory on failure.
 */
XCAPI(xErrno) xIOBufferAppendStr(xIOBuffer *io, const char *str);

/**
 * @brief Zero-copy append: move all refs from @p other into @p io.
 *
 * After this call, @p other is empty. Block refcounts are transferred,
 * not incremented.
 *
 * @param io     Destination IOBuf.
 * @param other  Source IOBuf (will be emptied).
 * @return xErrno_Ok on success.
 */
XCAPI(xErrno) xIOBufferAppendIOBuffer(xIOBuffer *io, xIOBuffer *other);

/* ───────────────────── Read (consume) ───────────────────── */

/**
 * @brief Copy up to @p len bytes from the front into @p out, then consume.
 *
 * @param io   IOBuf (must not be NULL).
 * @param out  Destination buffer.
 * @param len  Maximum bytes to read.
 * @return Number of bytes actually read.
 */
XCAPI(size_t) xIOBufferRead(xIOBuffer *io, void *out, size_t len);

/**
 * @brief Zero-copy cut: move the first @p n bytes into @p dst.
 *
 * Splits block refs at the cut boundary. Blocks that are fully
 * consumed have their refcount transferred; the boundary block
 * gets its refcount incremented (shared).
 *
 * @param io   Source IOBuf.
 * @param dst  Destination IOBuf (should be initialized/empty).
 * @param n    Number of bytes to cut.
 * @return Number of bytes actually moved (may be < n if io has fewer).
 */
XCAPI(size_t) xIOBufferCut(xIOBuffer *io, xIOBuffer *dst, size_t n);

/**
 * @brief Discard the first @p n bytes.
 *
 * @param io  IOBuf (must not be NULL).
 * @param n   Number of bytes to discard.
 * @return Number of bytes actually discarded.
 */
XCAPI(size_t) xIOBufferConsume(xIOBuffer *io, size_t n);

/* ───────────────────── Linearize ───────────────────── */

/**
 * @brief Copy the entire IOBuf content into a contiguous buffer.
 *
 * @param io   IOBuf (must not be NULL).
 * @param out  Destination buffer (must have at least xIOBufferLen(io) bytes).
 * @return Number of bytes copied.
 */
XCAPI(size_t) xIOBufferCopyTo(const xIOBuffer *io, void *out);

/* ───────────────────── Custom I/O function types ───────────────────── */

/**
 * @brief Custom read function type for xIOBufferReadWith.
 *
 * Semantics match read(2): returns bytes read, 0 on EOF, -1 on error.
 *
 * @param ctx  User-provided context (e.g. SSL*, fd wrapper).
 * @param buf  Destination buffer.
 * @param len  Maximum bytes to read.
 * @return Bytes read, 0 on EOF, -1 on error.
 */
typedef ssize_t (*xIOBufferReadFunc)(void *ctx, void *buf, size_t len);

/**
 * @brief Custom writev function type for xIOBufferWriteWith.
 *
 * Semantics match writev(2): returns bytes written, -1 on error.
 *
 * @param ctx     User-provided context.
 * @param iov     Array of iovec entries.
 * @param iovcnt  Number of iovec entries.
 * @return Bytes written, or -1 on error.
 */
typedef ssize_t (*xIOBufferWritevFunc)(void *ctx, const xiovec *iov,
                                       int iovcnt);

/* ───────────────────── I/O helpers ───────────────────── */

/**
 * @brief Fill an iovec array with readable segments for writev().
 *
 * @param io      IOBuf (must not be NULL).
 * @param iov     Output iovec array.
 * @param max_iov Maximum number of iovec entries to fill.
 * @return Number of iovec entries filled.
 */
XCAPI(int) xIOBufferReadIov(const xIOBuffer *io, xiovec *iov,
                            int max_iov);

/**
 * @brief Read from a file descriptor into the IOBuf.
 *
 * Allocates a new block (or reuses the tail block's remaining space)
 * and performs a single read() syscall.
 *
 * @param io  IOBuf (must not be NULL).
 * @param fd  File descriptor.
 * @return Bytes read, 0 on EOF, -1 on error.
 */
XCAPI(ssize_t) xIOBufferReadFd(xIOBuffer *io, int fd);

/**
 * @brief Write IOBuf data to a file descriptor using writev().
 *
 * Consumes the written bytes from the front.
 *
 * @param io  IOBuf (must not be NULL).
 * @param fd  File descriptor.
 * @return Bytes written, or -1 on error.
 */
XCAPI(ssize_t) xIOBufferWriteFd(xIOBuffer *io, int fd);

/**
 * @brief Read into the IOBuf using a custom read function.
 *
 * Same semantics as xIOBufferReadFd, but uses the provided callback
 * instead of read(2).
 *
 * @param io   IOBuf (must not be NULL).
 * @param fn   Custom read function.
 * @param ctx  Context passed to fn.
 * @return Bytes read, 0 on EOF, -1 on error.
 */
XCAPI(ssize_t) xIOBufferReadWith(xIOBuffer *io, xIOBufferReadFunc fn,
                                 void *ctx);

/**
 * @brief Write IOBuf data using a custom writev function.
 *
 * Same semantics as xIOBufferWriteFd, but uses the provided callback
 * instead of writev(2).
 *
 * @param io   IOBuf (must not be NULL).
 * @param fn   Custom writev function.
 * @param ctx  Context passed to fn.
 * @return Bytes written, or -1 on error.
 */
XCAPI(ssize_t) xIOBufferWriteWith(xIOBuffer *io, xIOBufferWritevFunc fn,
                                  void *ctx);

/* ───────────────────── Block pool ───────────────────── */

/**
 * @brief Pre-populate the global block pool with @p n blocks.
 *
 * Useful at startup to avoid allocation spikes.
 *
 * @param n  Number of blocks to pre-allocate.
 * @return xErrno_Ok on success, xErrno_NoMemory on failure.
 */
XCAPI(xErrno) xIOBlockPoolWarmup(size_t n);

/**
 * @brief Release all blocks in the global pool back to the OS.
 *
 * Typically called at shutdown for clean valgrind reports.
 */
XCAPI(void) xIOBlockPoolDrain(void);

#endif /* XBUF_IO_H */
