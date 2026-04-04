/*
 * Copyright 2025 The xKit Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * server_private.h - Internal data structures for the HTTP server
 */

#ifndef XHTTP_SERVER_PRIVATE_H
#define XHTTP_SERVER_PRIVATE_H

#include <llhttp.h>
#include <xbase/event.h>
#include <xbase/socket.h>
#include <xbuf/buf.h>
#include <xbuf/io.h>
#include <xhttp/server.h>

/* ───────────────────── Default configuration ───────────────────── */

#define XHTTP_DEFAULT_IDLE_TIMEOUT_MS   60000
#define XHTTP_DEFAULT_MAX_HEADER_SIZE   8192
#define XHTTP_DEFAULT_MAX_BODY_SIZE     1048576

/* Maximum number of iovec entries for writev */
#define XHTTP_MAX_IOV  64

/* ───────────────────── Route entry ───────────────────── */

struct xHttpRoute_ {
  const char        *method;   /**< HTTP method, or NULL for any method   */
  const char        *path;     /**< URL path (exact match)                */
  xHttpHandlerFunc   handler;  /**< Handler callback                      */
  void              *arg;      /**< User argument for handler             */
  struct xHttpRoute_ *next;    /**< Next route in the linked list         */
};

/* ───────────────────── Response header entry ───────────────────── */

struct xHttpHeader_ {
  char                *key;
  char                *value;
  struct xHttpHeader_ *next;
};

/* ───────────────────── Response writer ───────────────────── */

struct xHttpResponseWriter_ {
  int                  status_code;  /**< HTTP status code (default 200)  */
  struct xHttpHeader_ *headers;      /**< Response header linked list     */
  struct xHttpHeader_ *headers_tail; /**< Tail for O(1) append            */
  int                  sent;         /**< Whether response has been sent  */
  int                  streaming;    /**< Whether in streaming mode       */
  struct xHttpConn_   *conn;         /**< Back-pointer to the connection  */
};

/* ───────────────────── Connection ───────────────────── */

struct xHttpConn_ {
  struct xHttpServer_ *server;       /**< Back-pointer to the server      */
  xSocket              sock;         /**< Async socket handle              */
  xIOBuffer            read_buf;     /**< Read buffer                      */
  xIOBuffer            write_buf;    /**< Write buffer                     */

  /* llhttp parser state */
  llhttp_t             parser;       /**< HTTP parser instance             */
  llhttp_settings_t    parser_settings; /**< Parser callbacks              */

  /* Request parsing state (accumulated during parsing) */
  xBuffer              url;          /**< Parsed URL                       */
  xBuffer              header_field; /**< Current header field being parsed*/
  xBuffer              headers_raw;  /**< Accumulated raw headers          */
  xBuffer              body;         /**< Accumulated body                 */
  size_t               header_bytes; /**< Total header bytes received      */

  /* Response writer for current request */
  struct xHttpResponseWriter_ writer;

  /* Connection state */
  int                  keep_alive;   /**< Whether to keep connection alive */
  int                  writing;      /**< Whether we are in write mode     */
  int                  request_complete; /**< Request fully parsed, dispatch needed */
  int                  pending_error;    /**< Error status to send after parse  */
  const char          *pending_error_reason; /**< Error reason string          */

  /* Linked list of active connections */
  struct xHttpConn_   *prev;
  struct xHttpConn_   *next;
};

/* ───────────────────── Server ───────────────────── */

struct xHttpServer_ {
  xEventLoop           loop;         /**< Event loop                       */
  xSocket              listen_sock;  /**< Listening socket                  */
  int                  listen_fd;    /**< Listening socket fd (raw)         */

  /* Routes */
  struct xHttpRoute_  *routes;       /**< Head of route linked list         */
  struct xHttpRoute_  *routes_tail;  /**< Tail for O(1) append              */

  /* Active connections (doubly-linked list) */
  struct xHttpConn_   *conns;        /**< Head of active connection list    */

  /* Configuration */
  int                  idle_timeout_ms;
  size_t               max_header_size;
  size_t               max_body_size;
};

/* ───────────────────── Internal functions ───────────────────── */

/* Connection management (server.c) */
void xHttpConnClose(struct xHttpConn_ *conn);
void xHttpConnResetParser(struct xHttpConn_ *conn);

/* Response helpers (server.c) */
void xHttpConnSendError(struct xHttpConn_ *conn, int status_code,
                        const char *reason);
void xHttpConnFlushWrite(struct xHttpConn_ *conn);

/* HTTP status reason phrase lookup */
const char *xHttpStatusReason(int code);

#endif /* XHTTP_SERVER_PRIVATE_H */
