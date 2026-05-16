/*
 * Copyright 2025 The libx Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * tcp.h - TCP connection, connector, and listener abstractions
 *
 * Provides xTcpConn (connection resource wrapper), xTcpConnect (async
 * TCP connector with optional TLS), and xTcpListener (async TCP listener
 * with optional TLS).
 */

#ifndef XNET_TCP_H
#define XNET_TCP_H

#include <stdint.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <x/base/base.h>
#include <x/base/error.h>
#include <x/base/event.h>
#include <x/base/io.h>
#include <x/base/socket.h>
#include <x/net/tls.h>
#include <x/net/transport.h>

/* ═══════════════════════════════════════════════════════════════════
 *  xTcpConn — connection resource wrapper
 *
 *  Design note: unlike xWsCallbacks, we intentionally do NOT provide
 *  a callback-based API (e.g. on_data / on_close) at the TCP layer.
 *  WebSocket callbacks work well because the protocol defines message
 *  boundaries, close handshakes, and ping/pong — the library does
 *  real work before invoking user code. Raw TCP is a byte stream with
 *  no framing; an on_data callback would still deliver arbitrary
 *  fragments, leaving the user to reassemble and parse — no better
 *  than calling xTcpConnRecv directly. Instead, we keep xTcpConn as
 *  a thin resource wrapper and provide Recv / Send / SendIov helpers
 *  so users can drive I/O from their own event callbacks.
 * ═══════════════════════════════════════════════════════════════════
 */

/**
 * @brief Opaque handle to a TCP connection (xSocket + xTransport).
 */
XDEF_HANDLE(xTcpConn);

/**
 * @brief Get the transport interface for read/write operations.
 *
 * @param conn  Connection handle (must not be NULL).
 * @return      Pointer to the internal xTransport.
 */
XCAPI(xTransport *) xTcpConnTransport(xTcpConn conn);

/**
 * @brief Get the underlying xSocket.
 *
 * @param conn  Connection handle, or NULL.
 * @return      The xSocket, or NULL if conn is NULL.
 */
XCAPI(xSocket) xTcpConnSocket(xTcpConn conn);

/**
 * @brief Receive data from a TCP connection.
 *
 * Reads up to @p len bytes into @p buf. Semantics match read(2):
 * returns bytes read, 0 on EOF (peer closed), -1 on error.
 * For TLS connections, decryption is handled transparently.
 *
 * @param conn  Connection handle (must not be NULL).
 * @param buf   Buffer to read into.
 * @param len   Maximum number of bytes to read.
 * @return      Bytes read, 0 on EOF, or -1 on error (errno set).
 */
XCAPI(ssize_t) xTcpConnRecv(xTcpConn conn, void *buf, size_t len);

/**
 * @brief Send data over a TCP connection.
 *
 * Writes up to @p len bytes from @p buf. Semantics match write(2):
 * returns bytes written, or -1 on error.
 * For TLS connections, encryption is handled transparently.
 *
 * @param conn  Connection handle (must not be NULL).
 * @param buf   Data to send.
 * @param len   Number of bytes to send.
 * @return      Bytes written, or -1 on error (errno set).
 */
XCAPI(ssize_t) xTcpConnSend(xTcpConn conn, const char *buf, size_t len);

/**
 * @brief Send scattered data over a TCP connection.
 *
 * Writes data from multiple buffers using scatter-gather I/O.
 * Semantics match writev(2): returns total bytes written, or -1 on error.
 * For TLS connections, encryption is handled transparently.
 *
 * @param conn    Connection handle (must not be NULL).
 * @param iov     Array of I/O vectors.
 * @param iovcnt  Number of vectors in @p iov.
 * @return        Total bytes written, or -1 on error (errno set).
 */
XCAPI(ssize_t) xTcpConnSendIov(xTcpConn conn, const struct iovec *iov,
                               int iovcnt);

/**
 * @brief Close a TCP connection and release all resources.
 *
 * Destroys resources in the correct order: transport.destroy →
 * xSocketDestroy → free(conn). Safe to call with NULL (no-op).
 *
 * @param loop  The event loop.
 * @param conn  Connection to close, or NULL.
 */
XCAPI(void) xTcpConnClose(xEventLoop loop, xTcpConn conn);

/**
 * @brief Take ownership of the xSocket from the connection.
 *
 * Returns the internal xSocket and sets it to NULL inside the conn.
 * Subsequent xTcpConnClose will not close this socket.
 *
 * @param conn  Connection handle, or NULL.
 * @return      The xSocket, or NULL if conn is NULL.
 */
XCAPI(xSocket) xTcpConnTakeSocket(xTcpConn conn);

/**
 * @brief Take ownership of the xTransport from the connection.
 *
 * Returns a copy of the internal xTransport and zeros it inside the conn.
 * Subsequent xTcpConnClose will not destroy this transport.
 *
 * @param conn  Connection handle, or NULL.
 * @return      Copy of the xTransport (all fields zero if conn is NULL).
 */
XCAPI(xTransport) xTcpConnTakeTransport(xTcpConn conn);

/**
 * @brief Get an xReader adapter for the connection.
 *
 * Returns an xReader whose read function and context are taken from the
 * connection's internal xTransport. Reading through the returned xReader
 * is equivalent to calling xTcpConnRecv.
 *
 * @param conn  Connection handle (must not be NULL).
 * @return      An xReader bound to the connection's transport.
 */
XCAPI(xReader) xTcpConnReader(xTcpConn conn);

/**
 * @brief Get an xWriter adapter for the connection.
 *
 * Returns an xWriter whose writev function and context are taken from the
 * connection's internal xTransport. Writing through the returned xWriter
 * is equivalent to calling xTcpConnSendIov.
 *
 * @param conn  Connection handle (must not be NULL).
 * @return      An xWriter bound to the connection's transport.
 */
XCAPI(xWriter) xTcpConnWriter(xTcpConn conn);

/* ═══════════════════════════════════════════════════════════════════
 *  xTcpConnect — async TCP connector
 * ═══════════════════════════════════════════════════════════════════
 */

/**
 * @brief Configuration for xTcpConnect.
 */
XDEF_STRUCT(xTcpConnectConf) {
  xTlsCtx         tls_ctx;   /**< Shared TLS context (preferred), or NULL */
  const xTlsConf *tls;       /**< TLS config for auto-created ctx, or NULL */
  int timeout_ms;            /**< Connect timeout in ms (0 = default 10s) */
  int nodelay;               /**< Set TCP_NODELAY if non-zero             */
  int keepalive;             /**< Set SO_KEEPALIVE if non-zero            */
};

/**
 * @brief Callback invoked when an async TCP connection completes.
 *
 * On success, `conn` is a valid xTcpConn and `err` is xErrno_Ok.
 * On failure, `conn` is NULL and `err` indicates the error.
 *
 * @param conn  The established connection, or NULL on failure.
 * @param err   xErrno_Ok on success, or an error code.
 * @param arg   User-provided argument from xTcpConnect().
 */
typedef void (*xTcpConnectFunc)(xTcpConn conn, xErrno err, void *arg);

/**
 * @brief Initiate an async TCP connection.
 *
 * Performs DNS resolution → socket creation → non-blocking connect →
 * optional TLS handshake → callback notification, all asynchronously.
 *
 * @param loop      Event loop (must not be NULL).
 * @param host      Hostname or IP address (must not be NULL).
 * @param port      Port number.
 * @param conf      Connection configuration, or NULL for defaults.
 * @param callback  Completion callback (must not be NULL).
 * @param arg       User argument forwarded to callback.
 * @return          xErrno_Ok on success, or an error code.
 */
XCAPI(xErrno) xTcpConnect(xEventLoop loop, const char *host, uint16_t port,
                          const xTcpConnectConf *conf, xTcpConnectFunc callback,
                          void *arg);

/* ═══════════════════════════════════════════════════════════════════
 *  xTcpListener — async TCP listener
 * ═══════════════════════════════════════════════════════════════════
 */

/**
 * @brief Opaque handle to a TCP listener.
 */
XDEF_HANDLE(xTcpListener);

/**
 * @brief Configuration for xTcpListener.
 */
XDEF_STRUCT(xTcpListenerConf) {
  xTlsCtx tls_ctx; /**< TLS context from xTlsCtxCreate(), or NULL       */
  int   backlog;   /**< listen() backlog (0 = default 128)               */
  int   reuseport; /**< Set SO_REUSEPORT if non-zero                    */
};

/**
 * @brief Callback invoked when a new connection is accepted.
 *
 * @param listener  The listener that accepted the connection.
 * @param conn      The new connection (caller takes ownership).
 * @param addr      Peer address.
 * @param addrlen   Length of peer address.
 * @param arg       User-provided argument from xTcpListenerCreate().
 */
typedef void (*xTcpListenerFunc)(xTcpListener listener, xTcpConn conn,
                                 const struct sockaddr *addr, socklen_t addrlen,
                                 void *arg);

/**
 * @brief Create a TCP listener.
 *
 * Creates a socket, sets SO_REUSEADDR, binds, listens, and registers
 * with the event loop for accept events.
 *
 * @param loop      Event loop (must not be NULL).
 * @param host      Bind address, or NULL for INADDR_ANY.
 * @param port      Port number.
 * @param conf      Listener configuration, or NULL for defaults.
 * @param callback  Accept callback (must not be NULL).
 * @param arg       User argument forwarded to callback.
 * @return          A listener handle, or NULL on failure.
 */
XCAPI(xTcpListener) xTcpListenerCreate(xEventLoop loop, const char *host,
                                       uint16_t                port,
                                       const xTcpListenerConf *conf,
                                       xTcpListenerFunc callback, void *arg);

/**
 * @brief Destroy a TCP listener.
 *
 * Closes the listening socket and stops accepting connections.
 * Already-established connections are not affected.
 * Safe to call with NULL (no-op).
 *
 * @param listener  Listener to destroy, or NULL.
 */
XCAPI(void) xTcpListenerDestroy(xTcpListener listener);

#endif /* XNET_TCP_H */
