/*
 * Copyright 2025 The xKit Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * server_private.h - Internal data structures for the HTTP server
 */

#ifndef XHTTP_SERVER_PRIVATE_H
#define XHTTP_SERVER_PRIVATE_H

#include <stddef.h>
#include <stdint.h>
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

/* ───────────────────── Route segment ───────────────────── */

/**
 * A single segment of a route pattern, e.g. for "/users/:id/posts":
 *   segments[0] = { .text = "users", .param = NULL  }
 *   segments[1] = { .text = NULL,    .param = "id"  }
 *   segments[2] = { .text = "posts", .param = NULL  }
 */
struct xHttpRouteSegment_ {
  const char *text;   /**< Static text, or NULL for a param segment  */
  const char *param;  /**< Param name (e.g. "id"), or NULL for static */
};

/* ───────────────────── Route entry ───────────────────── */

struct xHttpRoute_ {
  const char        *method;        /**< HTTP method, or NULL for any method */
  const char        *path;          /**< Original pattern string             */
  struct xHttpRouteSegment_ *segments; /**< Pre-parsed segments             */
  int                segment_count;   /**< Number of segments               */
  xHttpHandlerFunc   handler;       /**< Handler callback                    */
  void              *arg;           /**< User argument for handler           */
  struct xHttpRoute_ *next;         /**< Next route in the linked list       */
};

/* ───────────────────── Route param entry (matched) ───────────────────── */

#define XHTTP_MAX_PARAMS  8

struct xHttpParam_ {
  const char *name;   /**< Param name (points into route segment)  */
  const char *value;  /**< Param value (points into request URL)   */
  size_t      value_len; /**< Length of value (not NUL-terminated)  */
};

/* ───────────────────── Forward declarations ───────────────────── */

struct xHttpConn_;
struct xHttpStream_;

/* ───────────────────── Response header entry ───────────────────── */

struct xHttpHeader_ {
  char                *key;
  char                *value;
  struct xHttpHeader_ *next;
};

/* ───────────────────── Response writer ───────────────────── */

struct xHttpResponseWriter_ {
  int                   status_code;  /**< HTTP status code (default 200)  */
  struct xHttpHeader_  *headers;      /**< Response header linked list     */
  struct xHttpHeader_  *headers_tail; /**< Tail for O(1) append            */
  int                   sent;         /**< Whether response has been sent  */
  int                   streaming;    /**< Whether in streaming mode       */
  struct xHttpStream_  *stream;       /**< Back-pointer to the stream      */
};

/* ───────────────────── Protocol handler vtable ───────────────────── */

/**
 * Abstract protocol handler interface (vtable).
 * Allows transparent switching between HTTP/1.1 (llhttp) and HTTP/2 (nghttp2).
 */
typedef struct xHttpProto_ {
  /* Data ingestion */
  int          (*on_data)(struct xHttpConn_ *conn, const char *buf, size_t len);
  /* Connection lifecycle */
  void         (*reset)(struct xHttpConn_ *conn);
  void         (*destroy)(struct xHttpConn_ *conn);
  /* Request introspection (operates on stream) */
  const char  *(*method)(struct xHttpStream_ *stream);
  int          (*should_keep_alive)(struct xHttpConn_ *conn);
  /* Response serialization (protocol-specific) */
  int          (*send_response)(struct xHttpStream_ *stream, int status,
                                struct xHttpHeader_ *headers,
                                const char *body, size_t body_len);
  int          (*write_data)(struct xHttpStream_ *stream,
                             const char *data, size_t len);
  int          (*end_stream)(struct xHttpStream_ *stream);
  void         *state;  /**< Opaque protocol state (e.g. xHttpProtoH1*) */
} xHttpProto;

/* ───────────────────── Stream (per-request state) ───────────────────── */

/**
 * Represents a single HTTP request/response exchange.
 * HTTP/1.1: one implicit stream per connection (stream_id = 0).
 * HTTP/2:   multiple concurrent streams per connection.
 */
struct xHttpStream_ {
  struct xHttpConn_   *conn;         /**< Back-pointer to the connection   */
  int32_t              stream_id;    /**< Stream ID (0 for H1)             */

  /* Request parsing state (accumulated during parsing) */
  xBuffer              url;          /**< Parsed URL                       */
  xBuffer              header_field; /**< Current header field being parsed*/
  xBuffer              headers_raw;  /**< Accumulated raw headers          */
  xBuffer              body;         /**< Accumulated body                 */
  size_t               header_bytes; /**< Total header bytes received      */

  /* Response writer for this stream */
  struct xHttpResponseWriter_ writer;

  /* Stream state */
  int                  request_complete; /**< Request fully parsed          */
  int                  pending_error;    /**< Error status to send          */
  const char          *pending_error_reason; /**< Error reason string        */
};

/* ───────────────────── Connection ───────────────────── */

struct xHttpConn_ {
  struct xHttpServer_ *server;       /**< Back-pointer to the server      */
  xSocket              sock;         /**< Async socket handle              */
  xIOBuffer            read_buf;     /**< Read buffer                      */
  xIOBuffer            write_buf;    /**< Write buffer                     */

  /* Protocol handler (vtable) */
  xHttpProto           proto;        /**< Protocol handler interface       */

  /* Current stream (H1: single implicit stream; H2: active stream) */
  struct xHttpStream_ *stream;       /**< Current/implicit stream          */

  /* Connection state */
  int                  keep_alive;   /**< Whether to keep connection alive */
  int                  writing;      /**< Whether we are in write mode     */
  int                  proto_detected; /**< Whether protocol has been detected */

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

/* Stream lifecycle (server.c) */
struct xHttpStream_ *xHttpStreamCreate(struct xHttpConn_ *conn, int32_t stream_id);
void xHttpStreamDestroy(struct xHttpStream_ *stream);
void xHttpStreamReset(struct xHttpStream_ *stream);

/* Connection management (server.c) */
void xHttpConnClose(struct xHttpConn_ *conn);
void xHttpConnResetParser(struct xHttpConn_ *conn);
void xHttpConnDispatchRequest(struct xHttpConn_ *conn);

/* Response helpers (server.c) */
void xHttpConnSendError(struct xHttpConn_ *conn, int status_code,
                        const char *reason);
void xHttpConnFlushWrite(struct xHttpConn_ *conn);
void xHttpConnTryFlush(struct xHttpConn_ *conn);

/* HTTP status reason phrase lookup */
const char *xHttpStatusReason(int code);

#endif /* XHTTP_SERVER_PRIVATE_H */
