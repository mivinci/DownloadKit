/*
 * Copyright 2025 The xKit Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * ws_private.h - Internal data structures for WebSocket connections
 */

#ifndef XHTTP_WS_PRIVATE_H
#define XHTTP_WS_PRIVATE_H

#include "transport.h"
#include "ws_deflate.h"
#include "ws_frame.h"
#include <xbase/event.h>
#include <xbase/socket.h>
#include <xbuf/io.h>
#include <xhttp/server.h>
#include <xhttp/ws.h>

/* Forward declaration */
struct xHttpServer_;

/* ───────────────────── Close state machine ───────────────────── */

typedef enum {
  XWS_OPEN = 0,       /**< Normal operating state              */
  XWS_CLOSE_SENT,     /**< We sent Close, waiting for peer     */
  XWS_CLOSE_RECEIVED, /**< Peer sent Close, we replied         */
  XWS_CLOSED,         /**< Connection fully closed              */
} xWsCloseState;

/* ───────────────────── WebSocket connection ───────────────────── */

struct xWsConn_ {
  struct xHttpServer_ *server; /**< Back-pointer to the server       */
  xEventLoop           loop;   /**< Event loop                       */
  xSocket              sock;   /**< Async socket handle               */
  xIOBuffer            read_buf;  /**< Incoming data buffer           */
  xIOBuffer            write_buf; /**< Outgoing data buffer           */

  /* Transport layer (transferred from HTTP connection) */
  xHttpTransport transport;

  /* Frame parser */
  xWsFrameParser parser;

  /* Fragment reassembly */
  xIOBuffer frag_buf;     /**< Accumulated fragment payload      */
  uint8_t   frag_opcode;  /**< Opcode of the first fragment      */
  int       in_fragment;  /**< Whether we are mid-fragmented msg */
  int       frag_compressed; /**< RSV1 was set on first fragment */

  /* Close handshake state */
  xWsCloseState close_state;
  uint16_t      close_code;   /**< Close code to report            */
  char         *close_reason; /**< Close reason (heap, may be NULL)*/
  size_t        close_reason_len;

  /* Idle timeout */
  xEventTimer idle_timer;
  int         idle_timeout_ms;

  /* User callbacks */
  xWsCallbacks callbacks;
  void        *user_arg;

  /* Connection state */
  int is_client; /**< Non-zero for client-side connection */
  int writing;   /**< Whether we are flushing writes      */

#ifdef XHTTP_WS_DEFLATE
  /* permessage-deflate state */
  xWsDeflateParams deflate_params;
  xWsDeflateCtx   *deflate_ctx;
#endif

  /* Doubly-linked list of WS connections on the server */
  struct xWsConn_ *prev;
  struct xWsConn_ *next;
};

/* ───────────────────── Internal API ───────────────────── */

/**
 * Create a WebSocket connection from a hijacked HTTP connection.
 *
 * Takes ownership of the socket and transport. Registers read
 * events on the event loop.
 *
 * @param server      The HTTP server.
 * @param loop        Event loop.
 * @param sock        Socket handle (ownership transferred).
 * @param transport   Transport vtable (ownership transferred).
 * @param callbacks   User callbacks.
 * @param arg         User argument for callbacks.
 * @param timeout_ms  Idle timeout in milliseconds.
 * @return            New xWsConn, or NULL on failure.
 */
struct xWsConn_ *xWsConnCreate(struct xHttpServer_ *server,
                                xEventLoop loop,
                                xSocket sock,
                                xHttpTransport transport,
                                const xWsCallbacks *callbacks,
                                void *arg,
                                int timeout_ms);

/**
 * Destroy a WebSocket connection and free all resources.
 *
 * Removes from the server's WS connection list, cancels timers,
 * closes the socket, and frees all buffers.
 *
 * @param conn  Connection to destroy.
 */
void xWsConnDestroy(struct xWsConn_ *conn);

/**
 * Send a Close frame and begin the close handshake.
 *
 * @param conn    Connection.
 * @param code    Close status code.
 * @param reason  Optional reason string.
 * @param len     Length of reason.
 */
void xWsConnClose(struct xWsConn_ *conn, uint16_t code,
                  const char *reason, size_t len);

#endif /* XHTTP_WS_PRIVATE_H */
