/*
 * Copyright 2025 The moo Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * transport.h - Transport abstraction layer for TCP connections
 *
 * Provides a vtable-based abstraction over the underlying I/O transport,
 * allowing transparent switching between Plain TCP and TLS.
 */

#ifndef XNET_TRANSPORT_H
#define XNET_TRANSPORT_H

#include <xbase/base.h>
#include <xnet/tls.h>

#include <stddef.h>
#include <sys/types.h>
#include <sys/uio.h>

/* ───────────────────── Transport handshake result ───────────────────── */

/**
 * Return values for the handshake callback.
 */
XDEF_ENUM(xTransportResult){
  xTransportResult_Done      = 0, /**< Handshake complete              */
  xTransportResult_WantRead  = 1, /**< Need more data (register read)  */
  xTransportResult_WantWrite = 2, /**< Need to write (register write)  */
  xTransportResult_Error     = -1 /**< Handshake failed                */
};

/* ───────────────────── Transport vtable ───────────────────── */

/**
 * Abstract transport interface (vtable).
 *
 * Plain TCP: read/writev map directly to read(2)/writev(2).
 *            handshake and alpn are NULL.
 * TLS:      read/writev use SSL_read/SSL_write (or mbedtls equivalents).
 *            handshake performs async TLS handshake.
 *            alpn returns the negotiated ALPN protocol string.
 */
XDEF_STRUCT(xTransport) {
  /**
   * Read decrypted data from the transport.
   * Semantics match read(2): returns bytes read, 0 on EOF, -1 on error.
   * For TLS, WANT_READ/WANT_WRITE set errno to EAGAIN.
   */
  ssize_t (*read)(void *ctx, void *buf, size_t len);

  /**
   * Write data through the transport using scatter-gather I/O.
   * Semantics match writev(2): returns bytes written, -1 on error.
   * For TLS, data is encrypted before sending.
   */
  ssize_t (*writev)(void *ctx, const struct iovec *iov, int iovcnt);

  /**
   * Perform (or continue) an async handshake.
   * Returns xTransportResult_Done, WantRead, WantWrite, or Error.
   * NULL for Plain TCP (no handshake needed).
   */
  int (*handshake)(void *ctx);

  /**
   * Get the ALPN-negotiated protocol string after handshake.
   * Returns a pointer to a static string (e.g. "h2", "http/1.1"),
   * or NULL if no ALPN was negotiated.
   * NULL for Plain TCP.
   */
  const char *(*alpn)(void *ctx);

  /**
   * Destroy transport-specific state (e.g. SSL object).
   * Called when the connection is closed.
   * NULL means no cleanup needed.
   */
  void (*destroy)(void *ctx);

  void *ctx; /**< Opaque transport state (e.g. SSL*, fd wrapper) */
};

#endif /* XNET_TRANSPORT_H */
