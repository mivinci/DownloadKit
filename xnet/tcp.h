/*
 * Copyright 2025 The xKit Authors. All rights reserved.
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
#include <xbase/base.h>
#include <xbase/error.h>
#include <xbase/event.h>
#include <xbase/socket.h>
#include <xnet/tls.h>
#include <xnet/transport.h>

/* ═══════════════════════════════════════════════════════════════════
 *  xTcpConn — connection resource wrapper
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

/* ═══════════════════════════════════════════════════════════════════
 *  xTcpConnect — async TCP connector
 * ═══════════════════════════════════════════════════════════════════
 */

/**
 * @brief Configuration for xTcpConnect.
 */
XDEF_STRUCT(xTcpConnectConf) {
  const xTlsClientConf *tls; /**< TLS config, or NULL for plain TCP     */
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
  void *tls_ctx;   /**< TLS context from xTlsCtxCreate(), or NULL       */
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
