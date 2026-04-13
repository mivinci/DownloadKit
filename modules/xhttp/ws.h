/*
 * Copyright 2025 The xKit Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * ws.h - WebSocket API (server + client)
 *
 * Provides a callback-driven WebSocket interface.
 *
 * Server: Call xWsServe() for a one-line WebSocket-only server,
 * or call xWsUpgrade() inside a regular HTTP handler to perform
 * the WebSocket upgrade handshake alongside other HTTP routes.
 *
 * Client: Call xWsConnect() with a ws:// or wss:// URL to
 * initiate an asynchronous WebSocket connection.
 *
 * All callbacks are dispatched on the event loop thread.
 */

#ifndef XHTTP_WS_H
#define XHTTP_WS_H

#include <stddef.h>
#include <stdint.h>
#include <xbase/base.h>
#include <xbase/error.h>
#include <xbase/event.h>
#include <xnet/tls.h>

/* Forward declarations (avoid circular include with server.h) */
XDEF_HANDLE(xHttpResponseWriter);
XDEF_HANDLE(xHttpServer);
typedef struct xHttpRequest xHttpRequest;

/* ── Types ─────────────────────────────────────────────────────────────── */

/**
 * @brief Opaque handle to a WebSocket connection.
 *
 * Valid from the on_open callback until after the on_close callback
 * returns. The handle may be stored and used to call xWsSend() or
 * xWsClose() at any time (from the event loop thread).
 */
XDEF_HANDLE(xWsConn);

/**
 * @brief WebSocket message type.
 */
XDEF_ENUM(xWsOpcode){
  xWsOpcode_Text   = 0x1, /**< UTF-8 text message   */
  xWsOpcode_Binary = 0x2, /**< Binary message        */
};

/* ── Callbacks ─────────────────────────────────────────────────────────── */

/**
 * @brief Called when a WebSocket connection is established.
 *
 * @param conn  The new WebSocket connection handle.
 * @param arg   User-provided argument from xWsUpgrade().
 */
typedef void (*xWsOnOpenFunc)(xWsConn conn, void *arg);

/**
 * @brief Called when a complete message is received.
 *
 * For fragmented messages, the library reassembles all fragments
 * before invoking this callback.
 *
 * @param conn     The WebSocket connection.
 * @param opcode   Message type (Text or Binary).
 * @param payload  Message payload (valid only during callback).
 * @param len      Payload length in bytes.
 * @param arg      User-provided argument.
 */
typedef void (*xWsOnMessageFunc)(xWsConn conn, xWsOpcode opcode,
                                 const void *payload, size_t len,
                                 void *arg);

/**
 * @brief Called when a WebSocket connection is closed.
 *
 * Invoked for both clean closes (Close handshake) and abnormal
 * disconnects (I/O error, timeout). After this callback returns,
 * the xWsConn handle is invalid.
 *
 * @param conn    The WebSocket connection.
 * @param code    Close status code (0 if unavailable).
 * @param reason  Close reason string (may be NULL).
 * @param len     Length of reason in bytes.
 * @param arg     User-provided argument.
 */
typedef void (*xWsOnCloseFunc)(xWsConn conn, uint16_t code,
                               const char *reason, size_t len,
                               void *arg);

/**
 * @brief WebSocket event callbacks.
 *
 * All fields are optional (may be NULL). Passed to
 * xWsUpgrade() to define behavior for a WebSocket connection.
 */
XDEF_STRUCT(xWsCallbacks) {
  xWsOnOpenFunc    on_open;    /**< Connection opened (optional) */
  xWsOnMessageFunc on_message; /**< Message received (optional)  */
  xWsOnCloseFunc   on_close;   /**< Connection closed (optional) */
};

/* ── Send / Close ──────────────────────────────────────────────────────── */

/**
 * @brief Send a message over a WebSocket connection.
 *
 * The payload is framed and queued for asynchronous transmission.
 * This function may be called from any callback on the event loop
 * thread.
 *
 * @param conn     WebSocket connection (must not be NULL).
 * @param opcode   Message type (xWsOpcode_Text or xWsOpcode_Binary).
 * @param payload  Message data.
 * @param len      Length of payload in bytes.
 * @return         xErrno_Ok on success, or an error code.
 */
XCAPI(xErrno) xWsSend(xWsConn conn, xWsOpcode opcode,
                      const void *payload, size_t len);

/**
 * @brief Initiate a graceful close of a WebSocket connection.
 *
 * Sends a Close frame with the given status code. The connection
 * remains open until the peer's Close frame is received (or a
 * timeout expires), after which on_close is invoked.
 *
 * @param conn  WebSocket connection (must not be NULL).
 * @param code  Close status code (e.g. 1000 for normal closure).
 * @return      xErrno_Ok on success, or an error code.
 */
XCAPI(xErrno) xWsClose(xWsConn conn, uint16_t code);

/* ── Upgrade ────────────────────────────────────────────────────────────── */

/**
 * @brief Upgrade an HTTP connection to WebSocket.
 *
 * Call this inside a regular xHttpHandlerFunc to perform the
 * WebSocket upgrade handshake (RFC 6455). On success the HTTP
 * connection is hijacked and a new xWsConn is created; the
 * handler must return immediately after a successful upgrade.
 *
 * The function validates the request headers (Upgrade, Connection,
 * Sec-WebSocket-Key, Sec-WebSocket-Version) and sends the 101
 * Switching Protocols response automatically.
 *
 * On failure (missing headers, wrong version, etc.) an appropriate
 * HTTP error response (400/405) is sent and the function returns
 * a non-Ok error code. The handler may then return normally.
 *
 * @param writer     The response writer from the handler.
 * @param req        The HTTP request from the handler.
 * @param callbacks  WebSocket event callbacks (must not be NULL).
 * @param arg        User argument forwarded to callbacks.
 * @return           xErrno_Ok on success, or an error code.
 */
XCAPI(xErrno) xWsUpgrade(xHttpResponseWriter writer,
                         const xHttpRequest *req,
                         const xWsCallbacks *callbacks,
                         void *arg);

/* ── Client Connect ─────────────────────────────────────────────────── */

/**
 * @brief Configuration for xWsConnect().
 *
 * Zero-initialize and set only the fields you need.
 * At minimum, @c url must be set.
 */
XDEF_STRUCT(xWsConnectConf) {
  /** WebSocket URL (ws:// or wss://). Required. */
  const char *url;

  /**
   * TLS configuration for wss:// connections.
   * NULL = use defaults (system CA, verify enabled).
   * Ignored for ws:// URLs.
   */
  const xTlsConf *tls;

  /**
   * Pre-created TLS context for wss:// connections.
   * Takes priority over @c tls. When set, the context is
   * shared (not owned) — the caller must keep it alive for
   * the lifetime of the connection.
   * NULL = create from @c tls (or use defaults).
   */
  xTlsCtx tls_ctx;

  /**
   * Extra HTTP headers for the Upgrade request.
   * Format: "Key: Value\r\nKey2: Value2\r\n".
   * NULL = no extra headers.
   */
  const char *headers;

  /**
   * Connection timeout in milliseconds.
   * 0 = use default (10000 ms).
   */
  int timeout_ms;
};

/**
 * @brief Initiate an asynchronous WebSocket client connection.
 *
 * Parses the URL, resolves DNS, connects via TCP, optionally
 * performs a TLS handshake (for wss://), sends the HTTP Upgrade
 * request, and validates the 101 response — all asynchronously.
 *
 * On success, @c callbacks->on_open is invoked with a usable
 * xWsConn handle. On failure at any stage, @c callbacks->on_close
 * is invoked with an appropriate error code (on_open is NOT
 * called).
 *
 * @param loop       Event loop to run the connection on.
 * @param conf       Connection configuration (must not be NULL).
 * @param callbacks  WebSocket event callbacks (must not be NULL).
 * @param arg        User argument forwarded to callbacks.
 * @return           xErrno_Ok if the async process started,
 *                   or xErrno_InvalidArg for bad parameters.
 */
XCAPI(xErrno) xWsConnect(xEventLoop loop,
                         const xWsConnectConf *conf,
                         const xWsCallbacks *callbacks,
                         void *arg);

/* ── Convenience: WebSocket-only server ─────────────────────────────── */

/**
 * @brief Create a WebSocket-only HTTP server.
 *
 * Convenience function that creates an HTTP server, registers a
 * catch-all route that upgrades every incoming request to WebSocket,
 * and starts listening on the given address and port.
 *
 * The returned handle can be used with xHttpServerDestroy() for
 * cleanup, or with xHttpServerRoute() to add extra HTTP endpoints
 * (e.g. a health-check page).
 *
 * @param loop       Event loop (must not be NULL).
 * @param host       Bind address (e.g. "0.0.0.0"), or NULL.
 * @param port       Port number to listen on.
 * @param callbacks  WebSocket event callbacks (must not be NULL).
 * @param arg        User argument forwarded to all callbacks.
 * @return           The server handle, or NULL on failure.
 */
XCAPI(xHttpServer) xWsServe(xEventLoop loop,
                             const char *host,
                             uint16_t port,
                             const xWsCallbacks *callbacks,
                             void *arg);

#endif /* XHTTP_WS_H */
