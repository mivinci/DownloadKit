/*
 * Copyright 2025 The moo Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * io.h - Abstract I/O interfaces (Reader, Writer, Seeker, Closer)
 *
 * Provides generic I/O abstractions similar to Go's io.Reader / io.Writer /
 * io.Seeker / io.Closer. Any object that implements these interfaces can use
 * the unified helper functions (xRead, xReadFull, xReadAll, xWrite, xWritev,
 * xSeek, xClose) for I/O operations.
 *
 * ── Interfaces ──────────────────────────────────────────────────────
 *
 *   xReader  — wraps  ssize_t read(ctx, buf, len)       (read(2) semantics)
 *   xWriter  — wraps  ssize_t writev(ctx, iov, iovcnt)  (writev(2) semantics)
 *   xSeeker  — wraps  off_t   seek(ctx, offset, whence) (lseek(2) semantics)
 *   xCloser  — wraps  int     close(ctx)                (close(2) semantics)
 *
 * Each interface is a small struct { fn_ptr, void *ctx } that can be
 * constructed from any object providing the matching function signature.
 * A zero-initialized struct (all NULL) is treated as "invalid / not set";
 * convenience functions check the function pointer before calling.
 *
 * ── Convenience functions ───────────────────────────────────────────
 *
 *   xRead(r, buf, len)          — single read, returns bytes / 0 / -1
 *   xReadFull(r, buf, len)      — loop until len bytes or EOF, retries EAGAIN
 *   xReadAll(r, &out, &out_len) — read until EOF into malloc'd buffer
 *   xWrite(w, buf, len)         — write a single contiguous buffer
 *   xWritev(w, iov, iovcnt)     — scatter-gather write
 *   xSeek(s, offset, whence)    — reposition read/write offset
 *   xClose(c)                   — release the underlying resource
 *
 * ── Integration with xTcpConn ───────────────────────────────────────
 *
 * xTcpConn (declared in <xnet/tcp.h>) provides two adapter functions
 * that return lightweight value-type interfaces bound to the connection's
 * internal xTransport:
 *
 *   xReader r = xTcpConnReader(conn);   // r.read  → transport.read
 *   xWriter w = xTcpConnWriter(conn);   // w.writev → transport.writev
 *
 * These adapters are zero-allocation — they simply copy the function
 * pointer and context from the transport. Once obtained, they can be
 * passed to any generic I/O helper:
 *
 *   // Read exactly 1024 bytes from a TCP connection
 *   xReader r = xTcpConnReader(conn);
 *   ssize_t n = xReadFull(r, buf, 1024);
 *
 *   // Read all data until the peer closes the connection
 *   void  *data;
 *   size_t data_len;
 *   xReadAll(r, &data, &data_len);
 *
 *   // Write a response through the generic writer
 *   xWriter w = xTcpConnWriter(conn);
 *   xWrite(w, "HTTP/1.1 200 OK\r\n\r\n", 19);
 *
 * If conn is NULL, both adapters return a zero-initialized struct
 * (function pointers are NULL), so subsequent convenience calls
 * safely return -1 without crashing.
 *
 * Note: xTcpConn does NOT provide an xCloser adapter because
 * xTcpConnClose() requires an xEventLoop parameter, which does not
 * fit the int (*close)(void *ctx) signature.
 */

#ifndef XBASE_IO_H
#define XBASE_IO_H

#include <xbase/base.h>

#include <stddef.h>

#ifdef _WIN32
typedef long xSsize;
typedef __int64 xOff;
#else
#include <sys/types.h>
#include <sys/uio.h>
typedef ssize_t xSsize;
typedef off_t xOff;
#endif

/**
 * @brief Portable I/O vector structure.
 *
 * On POSIX this is struct iovec; on Windows we define an equivalent.
 */
#ifdef _WIN32
XDEF_STRUCT(xIovec) {
  void  *iov_base;
  size_t iov_len;
};
#else
#define xIovec  struct iovec
#endif

/* ═══════════════════════════════════════════════════════════════════
 *  Core I/O interfaces
 * ═══════════════════════════════════════════════════════════════════
 */

/**
 * @brief Abstract reader interface.
 *
 * Wraps a read function pointer and an opaque context. The read function
 * follows read(2) semantics: returns bytes read, 0 on EOF, -1 on error.
 *
 * The function signature is compatible with xTransport.read and
 * xIOBufferReadFunc.
 */
XDEF_STRUCT(xReader) {
  xSsize (*read)(void *ctx, void *buf, size_t len);
  void *ctx;
};

/**
 * @brief Abstract writer interface.
 *
 * Wraps a writev function pointer and an opaque context. The writev function
 * follows writev(2) semantics: returns bytes written, -1 on error.
 *
 * The function signature is compatible with xTransport.writev and
 * xIOBufferWritevFunc.
 */
XDEF_STRUCT(xWriter) {
  xSsize (*writev)(void *ctx, const xIovec *iov, int iovcnt);
  void *ctx;
};

/**
 * @brief Abstract seeker interface.
 *
 * Wraps a seek function pointer and an opaque context. The seek function
 * follows lseek(2) semantics: returns the resulting offset, -1 on error.
 */
XDEF_STRUCT(xSeeker) {
  xOff (*seek)(void *ctx, xOff offset, int whence);
  void *ctx;
};

/**
 * @brief Abstract closer interface.
 *
 * Wraps a close function pointer and an opaque context. The close function
 * returns 0 on success, -1 on failure.
 */
XDEF_STRUCT(xCloser) {
  int (*close)(void *ctx);
  void *ctx;
};

/* ═══════════════════════════════════════════════════════════════════
 *  Convenience functions
 * ═══════════════════════════════════════════════════════════════════
 */

/**
 * @brief Read from a reader.
 *
 * Forwards to r.read(r.ctx, buf, len).
 *
 * @param r    The reader.
 * @param buf  Destination buffer.
 * @param len  Maximum bytes to read.
 * @return     Bytes read, 0 on EOF, -1 on error.
 */
XCAPI(xSsize) xRead(xReader r, void *buf, size_t len);

/**
 * @brief Write a single buffer through a writer.
 *
 * Wraps buf into a single iovec and calls w.writev(w.ctx, &iov, 1).
 *
 * @param w    The writer.
 * @param buf  Data to write.
 * @param len  Number of bytes to write.
 * @return     Bytes written, or -1 on error.
 */
XCAPI(xSsize) xWrite(xWriter w, const void *buf, size_t len);

/**
 * @brief Write scattered data through a writer.
 *
 * Forwards to w.writev(w.ctx, iov, iovcnt).
 *
 * @param w       The writer.
 * @param iov     Array of I/O vectors.
 * @param iovcnt  Number of vectors.
 * @return        Total bytes written, or -1 on error.
 */
XCAPI(xSsize) xWritev(xWriter w, const xIovec *iov, int iovcnt);

/**
 * @brief Seek to a position.
 *
 * Forwards to s.seek(s.ctx, offset, whence).
 *
 * @param s       The seeker.
 * @param offset  Offset value.
 * @param whence  SEEK_SET, SEEK_CUR, or SEEK_END.
 * @return        Resulting offset, or -1 on error.
 */
XCAPI(xOff) xSeek(xSeeker s, xOff offset, int whence);

/**
 * @brief Close a resource.
 *
 * Forwards to c.close(c.ctx).
 *
 * @param c  The closer.
 * @return   0 on success, -1 on failure.
 */
XCAPI(int) xClose(xCloser c);

/* ═══════════════════════════════════════════════════════════════════
 *  Advanced helper functions
 * ═══════════════════════════════════════════════════════════════════
 */

/**
 * @brief Read exactly len bytes, retrying on partial reads.
 *
 * Loops calling r.read until len bytes are read, EOF is reached, or an
 * unrecoverable error occurs. EAGAIN/EINTR are retried automatically.
 *
 * @param r    The reader.
 * @param buf  Destination buffer (must hold at least len bytes).
 * @param len  Number of bytes to read.
 * @return     Bytes actually read (may be < len on EOF), or -1 on error.
 */
XCAPI(xSsize) xReadFull(xReader r, void *buf, size_t len);

/**
 * @brief Read all data until EOF into a dynamically allocated buffer.
 *
 * Loops calling r.read until EOF, growing the buffer as needed (initial
 * size 4096, doubling on each expansion). On success, *out points to the
 * allocated buffer and *out_len contains the total bytes read. The caller
 * is responsible for freeing *out with free().
 *
 * On error, any allocated memory is freed, *out is set to NULL,
 * *out_len is set to 0, and -1 is returned.
 *
 * @param r        The reader.
 * @param[out] out      Pointer to receive the allocated buffer.
 * @param[out] out_len  Pointer to receive the total bytes read.
 * @return         0 on success, -1 on error.
 */
XCAPI(int) xReadAll(xReader r, void **out, size_t *out_len);

#endif /* XBASE_IO_H */
