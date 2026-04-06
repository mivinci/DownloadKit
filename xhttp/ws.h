/*
 * Copyright 2025 The xKit Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * ws.h - WebSocket server API
 *
 * Provides a callback-driven WebSocket interface integrated with
 * the xhttp server. Call xWsUpgrade() inside a regular HTTP
 * handler to perform the WebSocket upgrade handshake; the library
 * then handles the frame codec, ping/pong, and close negotiation
 * automatically.
 *
 * All callbacks are dispatched on the event loop thread.
 */

#ifndef XHTTP_WS_H
#define XHTTP_WS_H

#include <stddef.h>
#include <stdint.h>
#include <xbase/base.h>
#include <xbase/error.h>

/* Forward declarations (avoid circular include with server.h) */
XDEF_HANDLE(xHttpResponseWriter);
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

#endif /* XHTTP_WS_H */
