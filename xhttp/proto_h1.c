/*
 * Copyright 2025 The xKit Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * proto_h1.c - HTTP/1.1 protocol handler implementation (llhttp)
 */

#include "proto_h1.h"
#include "server_private.h"

#include <llhttp.h>
#include <stdlib.h>
#include <string.h>

/* ═══════════════════════════════════════════════════════════════════════════
 *  Internal state for HTTP/1.1 protocol handler
 * ═══════════════════════════════════════════════════════════════════════════ */

typedef struct xHttpProtoH1_ {
  llhttp_t           parser;
  llhttp_settings_t  settings;
} xHttpProtoH1;

/* ═══════════════════════════════════════════════════════════════════════════
 *  llhttp callbacks
 * ═══════════════════════════════════════════════════════════════════════════ */

static int on_url(llhttp_t *parser, const char *at, size_t len) {
  struct xHttpConn_ *conn = (struct xHttpConn_ *)parser->data;
  conn->header_bytes += len;

  if (!conn->url) conn->url = xBufferCreate(256);
  if (!conn->url) return HPE_INTERNAL;
  if (xBufferAppend(&conn->url, at, len) != xErrno_Ok) return HPE_INTERNAL;
  return HPE_OK;
}

static int on_header_field(llhttp_t *parser, const char *at, size_t len) {
  struct xHttpConn_ *conn = (struct xHttpConn_ *)parser->data;
  conn->header_bytes += len;

  if (conn->header_bytes > conn->server->max_header_size) {
    conn->pending_error = 431;
    conn->pending_error_reason = "Request Header Fields Too Large";
    return HPE_USER;
  }

  /* Accumulate into raw headers */
  if (!conn->headers_raw) conn->headers_raw = xBufferCreate(512);
  if (!conn->headers_raw) return HPE_INTERNAL;
  if (xBufferAppend(&conn->headers_raw, at, len) != xErrno_Ok)
    return HPE_INTERNAL;

  /* Track current field for potential use */
  if (conn->header_field) {
    xBufferReset(conn->header_field);
  } else {
    conn->header_field = xBufferCreate(128);
    if (!conn->header_field) return HPE_INTERNAL;
  }
  if (xBufferAppend(&conn->header_field, at, len) != xErrno_Ok)
    return HPE_INTERNAL;

  return HPE_OK;
}

static int on_header_value(llhttp_t *parser, const char *at, size_t len) {
  struct xHttpConn_ *conn = (struct xHttpConn_ *)parser->data;
  conn->header_bytes += len;

  if (conn->header_bytes > conn->server->max_header_size) {
    conn->pending_error = 431;
    conn->pending_error_reason = "Request Header Fields Too Large";
    return HPE_USER;
  }

  /* Append ": " + value + "\r\n" to raw headers */
  if (xBufferAppend(&conn->headers_raw, ": ", 2) != xErrno_Ok)
    return HPE_INTERNAL;
  if (xBufferAppend(&conn->headers_raw, at, len) != xErrno_Ok)
    return HPE_INTERNAL;
  if (xBufferAppend(&conn->headers_raw, "\r\n", 2) != xErrno_Ok)
    return HPE_INTERNAL;

  return HPE_OK;
}

static int on_headers_complete(llhttp_t *parser) {
  struct xHttpConn_ *conn = (struct xHttpConn_ *)parser->data;

  /* Determine keep-alive from HTTP version and headers */
  conn->keep_alive = llhttp_should_keep_alive(parser);

  return HPE_OK;
}

static int on_body(llhttp_t *parser, const char *at, size_t len) {
  struct xHttpConn_ *conn = (struct xHttpConn_ *)parser->data;

  size_t cur_len = conn->body ? xBufferLen(conn->body) : 0;
  if (cur_len + len > conn->server->max_body_size) {
    conn->pending_error = 413;
    conn->pending_error_reason = "Content Too Large";
    return HPE_USER;
  }

  if (!conn->body) conn->body = xBufferCreate(1024);
  if (!conn->body) return HPE_INTERNAL;
  if (xBufferAppend(&conn->body, at, len) != xErrno_Ok) return HPE_INTERNAL;

  return HPE_OK;
}

static int on_message_complete(llhttp_t *parser) {
  struct xHttpConn_ *conn = (struct xHttpConn_ *)parser->data;

  /* Set flag to dispatch after llhttp_execute returns.
   * We must not call dispatch from within a callback because
   * the response flush may reset/close the parser. */
  conn->request_complete = 1;

  /* Pause the parser so we can process the response before parsing
   * the next pipelined request */
  llhttp_pause(parser);

  return HPE_OK;
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  vtable method implementations
 * ═══════════════════════════════════════════════════════════════════════════ */

/**
 * Feed data to the HTTP/1.1 parser.
 * Returns:  1 = request complete (dispatch needed)
 *           0 = need more data
 *          -1 = parse error
 */
static int h1_on_data(struct xHttpConn_ *conn, const char *buf, size_t len) {
  xHttpProtoH1 *h1 = (xHttpProtoH1 *)conn->proto.state;

  enum llhttp_errno err = llhttp_execute(&h1->parser, buf, len);

  /* Handle deferred error from llhttp callbacks */
  if (conn->pending_error) {
    return -1;
  }

  /* Handle completed request (deferred from on_message_complete) */
  if (conn->request_complete) {
    return 1;
  }

  if (err != HPE_OK && err != HPE_PAUSED) {
    return -1;
  }

  return 0;
}

/**
 * Reset the HTTP/1.1 parser for keep-alive reuse.
 */
static void h1_reset(struct xHttpConn_ *conn) {
  xHttpProtoH1 *h1 = (xHttpProtoH1 *)conn->proto.state;
  if (h1) {
    llhttp_reset(&h1->parser);
  }
}

/**
 * Destroy the HTTP/1.1 protocol handler, freeing heap memory.
 */
static void h1_destroy(struct xHttpConn_ *conn) {
  if (conn->proto.state) {
    free(conn->proto.state);
    conn->proto.state = NULL;
  }
}

/**
 * Get the HTTP method string for the current request.
 */
static const char *h1_method(struct xHttpConn_ *conn) {
  xHttpProtoH1 *h1 = (xHttpProtoH1 *)conn->proto.state;
  return llhttp_method_name((llhttp_method_t)h1->parser.method);
}

/**
 * Check if the connection should be kept alive.
 */
static int h1_should_keep_alive(struct xHttpConn_ *conn) {
  xHttpProtoH1 *h1 = (xHttpProtoH1 *)conn->proto.state;
  return llhttp_should_keep_alive(&h1->parser);
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  Initialization
 * ═══════════════════════════════════════════════════════════════════════════ */

int xHttpProtoH1Init(struct xHttpConn_ *conn) {
  xHttpProtoH1 *h1 = (xHttpProtoH1 *)calloc(1, sizeof(xHttpProtoH1));
  if (!h1) return -1;

  /* Set up llhttp settings (callbacks) */
  memset(&h1->settings, 0, sizeof(h1->settings));
  h1->settings.on_url              = on_url;
  h1->settings.on_header_field     = on_header_field;
  h1->settings.on_header_value     = on_header_value;
  h1->settings.on_headers_complete = on_headers_complete;
  h1->settings.on_body             = on_body;
  h1->settings.on_message_complete = on_message_complete;

  /* Initialize the parser */
  llhttp_init(&h1->parser, HTTP_REQUEST, &h1->settings);
  h1->parser.data = conn;

  /* Populate the vtable */
  conn->proto.on_data          = h1_on_data;
  conn->proto.reset            = h1_reset;
  conn->proto.destroy          = h1_destroy;
  conn->proto.method           = h1_method;
  conn->proto.should_keep_alive = h1_should_keep_alive;
  conn->proto.state            = h1;

  return 0;
}
