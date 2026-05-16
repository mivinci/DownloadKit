/*
 * Copyright 2025 The moo Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * buf.h - Linear auto-growing byte buffer
 *
 * xBuffer is a simple, contiguous byte buffer that automatically grows
 * when more space is needed. It maintains a read position and a write
 * position, supporting efficient append and consume operations.
 *
 * The buffer header and data area are allocated in a single malloc()
 * call using a flexible array member, avoiding an extra indirection.
 *
 * Typical usage:
 *   xBuffer buf = xBufferCreate(1024);
 *   xBufferAppend(&buf, data, len);
 *   process(xBufferData(buf), xBufferLen(buf));
 *   xBufferConsume(buf, processed);
 *   xBufferDestroy(buf);
 */

#ifndef XBUF_BUF_H
#define XBUF_BUF_H

#include <x/base/base.h>
#include <x/base/error.h>

#include <stddef.h>
#include <sys/types.h>
#include <sys/uio.h>

/* ───────────────────── Types ───────────────────── */

/**
 * @brief Linear auto-growing byte buffer (opaque handle).
 *
 * The struct is allocated together with its data area via a flexible
 * array member.  Because realloc may relocate the whole object, APIs
 * that can trigger growth take an xBuffer* so the caller's handle
 * stays valid.
 *
 * Layout (single allocation):
 *   [xBuffer header | data ..........................]
 *                     ^          ^                   ^
 *                     data+rpos  data+wpos           data+cap
 */
XDEF_HANDLE(xBuffer);

/* ───────────────────── Lifecycle ───────────────────── */

/**
 * @brief Create a buffer with the given initial capacity.
 *
 * Header and data area are allocated in one malloc() call.
 *
 * @param initial_cap  Initial capacity in bytes (must be > 0).
 * @return A new buffer, or NULL on allocation failure.
 */
XCAPI(xBuffer) xBufferCreate(size_t initial_cap);

/**
 * @brief Release all memory owned by the buffer.
 *
 * @param buf  Buffer to destroy, or NULL (no-op).
 */
XCAPI(void) xBufferDestroy(xBuffer buf);

/**
 * @brief Discard all data but keep the allocated memory.
 *
 * @param buf  Buffer to reset (must not be NULL).
 */
XCAPI(void) xBufferReset(xBuffer buf);

/* ───────────────────── Write ───────────────────── */

/**
 * @brief Append raw bytes to the buffer, growing if necessary.
 *
 * Because growth may realloc the entire object, the caller must pass
 * a pointer to the handle so it can be updated.
 *
 * @param bufp  Pointer to the buffer handle (must not be NULL).
 * @param data  Source bytes (must not be NULL if len > 0).
 * @param len   Number of bytes to append.
 * @return xErrno_Ok on success, xErrno_NoMemory on allocation failure.
 */
XCAPI(xErrno) xBufferAppend(xBuffer *bufp, const void *data, size_t len);

/**
 * @brief Append a NUL-terminated C string (excluding the NUL).
 *
 * @param bufp  Pointer to the buffer handle (must not be NULL).
 * @param str   C string to append (must not be NULL).
 * @return xErrno_Ok on success, xErrno_NoMemory on allocation failure.
 */
XCAPI(xErrno) xBufferAppendStr(xBuffer *bufp, const char *str);

/**
 * @brief Ensure at least @p additional bytes of writable space.
 *
 * May trigger a realloc + compact. After a successful call,
 * xBufferWritable(*bufp) >= additional.
 *
 * @param bufp       Pointer to the buffer handle (must not be NULL).
 * @param additional Minimum writable bytes required.
 * @return xErrno_Ok on success, xErrno_NoMemory on allocation failure.
 */
XCAPI(xErrno) xBufferReserve(xBuffer *bufp, size_t additional);

/* ───────────────────── Read ───────────────────── */

/**
 * @brief Return a pointer to the start of readable data.
 *
 * The returned pointer is valid until the next mutating call.
 *
 * @param buf  Buffer, or NULL.
 * @return Pointer to readable bytes, or NULL if buf is NULL or empty.
 */
XCAPI(const void *) xBufferData(xBuffer buf);

/**
 * @brief Return the number of readable bytes.
 */
XCAPI(size_t) xBufferLen(xBuffer buf);

/**
 * @brief Return the total allocated capacity.
 */
XCAPI(size_t) xBufferCap(xBuffer buf);

/**
 * @brief Return the number of writable bytes (cap - wpos).
 */
XCAPI(size_t) xBufferWritable(xBuffer buf);

/**
 * @brief Advance the read position by @p n bytes (consume data).
 *
 * If n >= xBufferLen(buf), the buffer is fully consumed (equivalent to reset).
 *
 * @param buf  Buffer (must not be NULL).
 * @param n    Number of bytes to consume.
 */
XCAPI(void) xBufferConsume(xBuffer buf, size_t n);

/**
 * @brief Move unread data to the front of the buffer, reclaiming
 *        consumed space.
 *
 * After compact, rpos == 0 and writable space is maximized without
 * reallocation.
 *
 * @param buf  Buffer (must not be NULL).
 */
XCAPI(void) xBufferCompact(xBuffer buf);

/* ───────────────────── I/O helpers ───────────────────── */

/**
 * @brief Read from a file descriptor into the buffer.
 *
 * Performs a single read() syscall, growing the buffer if needed.
 * Returns the number of bytes read, 0 on EOF, or -1 on error (errno set).
 *
 * @param bufp  Pointer to the buffer handle (must not be NULL).
 * @param fd    File descriptor to read from.
 * @return Bytes read, 0 on EOF, -1 on error.
 */
XCAPI(ssize_t) xBufferReadFd(xBuffer *bufp, int fd);

/**
 * @brief Write readable data from the buffer to a file descriptor.
 *
 * Performs a single write() syscall and consumes the written bytes.
 * Returns the number of bytes written, or -1 on error (errno set).
 *
 * @param buf  Buffer (must not be NULL).
 * @param fd   File descriptor to write to.
 * @return Bytes written, or -1 on error.
 */
XCAPI(ssize_t) xBufferWriteFd(xBuffer buf, int fd);

#endif /* XBUF_BUF_H */
