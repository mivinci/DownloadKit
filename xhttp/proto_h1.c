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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ═══════════════════════════════════════════════════════════════════════════
 *  Internal state for HTTP/1.1 protocol handler
 * ═══════════════════════════════════════════════════════════════════════════
 */

XDEF_STRUCT(xHttpProtoH1) {
  llhttp_t          parser;
  llhttp_settings_t settings;
};

/* ═══════════════════════════════════════════════════════════════════════════
 *  llhttp callbacks
 * ═══════════════════════════════════════════════════════════════════════════
 */

static int on_url(llhttp_t *parser, const char *at, size_t len) {
  struct xHttpConn_   *conn   = (struct xHttpConn_ *)parser->data;
  struct xHttpStream_ *stream = conn->stream;
  stream->header_bytes += len;

  if (!stream->url) stream->url = xBufferCreate(256);
  if (!stream->url) return HPE_INTERNAL;
  if (xBufferAppend(&stream->url, at, len) != xErrno_Ok) return HPE_INTERNAL;
  return HPE_OK;
}

static int on_header_field(llhttp_t *parser, const char *at, size_t len) {
  struct xHttpConn_   *conn   = (struct xHttpConn_ *)parser->data;
  struct xHttpStream_ *stream = conn->stream;
  stream->header_bytes += len;

  if (stream->header_bytes > conn->server->max_header_size) {
    stream->pending_error        = 431;
    stream->pending_error_reason = "Request Header Fields Too Large";
    return HPE_USER;
  }

  /* Accumulate into raw headers */
  if (!stream->headers_raw) stream->headers_raw = xBufferCreate(512);
  if (!stream->headers_raw) return HPE_INTERNAL;
  if (xBufferAppend(&stream->headers_raw, at, len) != xErrno_Ok)
    return HPE_INTERNAL;

  /* Track current field for potential use */
  if (stream->header_field) {
    xBufferReset(stream->header_field);
  } else {
    stream->header_field = xBufferCreate(128);
    if (!stream->header_field) return HPE_INTERNAL;
  }
  if (xBufferAppend(&stream->header_field, at, len) != xErrno_Ok)
    return HPE_INTERNAL;

  return HPE_OK;
}

static int on_header_value(llhttp_t *parser, const char *at, size_t len) {
  struct xHttpConn_   *conn   = (struct xHttpConn_ *)parser->data;
  struct xHttpStream_ *stream = conn->stream;
  stream->header_bytes += len;

  if (stream->header_bytes > conn->server->max_header_size) {
    stream->pending_error        = 431;
    stream->pending_error_reason = "Request Header Fields Too Large";
    return HPE_USER;
  }

  /* Append ": " + value + "\r\n" to raw headers */
  if (xBufferAppend(&stream->headers_raw, ": ", 2) != xErrno_Ok)
    return HPE_INTERNAL;
  if (xBufferAppend(&stream->headers_raw, at, len) != xErrno_Ok)
    return HPE_INTERNAL;
  if (xBufferAppend(&stream->headers_raw, "\r\n", 2) != xErrno_Ok)
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
  struct xHttpConn_   *conn   = (struct xHttpConn_ *)parser->data;
  struct xHttpStream_ *stream = conn->stream;

  size_t cur_len = stream->body ? xBufferLen(stream->body) : 0;
  if (cur_len + len > conn->server->max_body_size) {
    stream->pending_error        = 413;
    stream->pending_error_reason = "Content Too Large";
    return HPE_USER;
  }

  if (!stream->body) stream->body = xBufferCreate(1024);
  if (!stream->body) return HPE_INTERNAL;
  if (xBufferAppend(&stream->body, at, len) != xErrno_Ok) return HPE_INTERNAL;

  return HPE_OK;
}

static int on_message_complete(llhttp_t *parser) {
  struct xHttpConn_ *conn = (struct xHttpConn_ *)parser->data;

  /* Set flag to dispatch after llhttp_execute returns.
   * We must not call dispatch from within a callback because
   * the response flush may reset/close the parser. */
  conn->stream->request_complete = 1;

  /* Pause the parser so we can process the response before parsing
   * the next pipelined request */
  llhttp_pause(parser);

  return HPE_OK;
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  vtable method implementations
 * ═══════════════════════════════════════════════════════════════════════════
 */

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
  if (conn->stream->pending_error) {
    return -1;
  }

  /* Handle completed request (deferred from on_message_complete) */
  if (conn->stream->request_complete) {
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
static const char *h1_method(struct xHttpStream_ *stream) {
  xHttpProtoH1 *h1 = (xHttpProtoH1 *)stream->conn->proto.state;
  return llhttp_method_name((llhttp_method_t)h1->parser.method);
}

/**
 * Check if the connection should be kept alive.
 */
static int h1_should_keep_alive(struct xHttpConn_ *conn) {
  xHttpProtoH1 *h1 = (xHttpProtoH1 *)conn->proto.state;
  return llhttp_should_keep_alive(&h1->parser);
}

/**
 * H1 send_response: serialize HTTP/1.1 status line + headers + body.
 */
static int h1_send_response(struct xHttpStream_ *stream, int status,
                            struct xHttpHeader_ *headers, const char *body,
                            size_t body_len) {
  struct xHttpConn_ *conn = stream->conn;
  xIOBuffer         *wb   = &conn->write_buf;

  /* Status line: "HTTP/1.1 <code> <reason>\r\n" */
  char status_line[64];
  int  slen = snprintf(status_line, sizeof(status_line), "HTTP/1.1 %d %s\r\n",
                       status, xHttpStatusReason(status));
  xIOBufferAppend(wb, status_line, (size_t)slen);

  /* Content-Length header */
  char cl_buf[48];
  int  cl_len =
    snprintf(cl_buf, sizeof(cl_buf), "Content-Length: %zu\r\n", body_len);
  xIOBufferAppend(wb, cl_buf, (size_t)cl_len);

  /* Connection header */
  if (conn->keep_alive) {
    xIOBufferAppendStr(wb, "Connection: keep-alive\r\n");
  } else {
    xIOBufferAppendStr(wb, "Connection: close\r\n");
  }

  /* User-set headers */
  struct xHttpHeader_ *h = headers;
  while (h) {
    xIOBufferAppendStr(wb, h->key);
    xIOBufferAppendStr(wb, ": ");
    xIOBufferAppendStr(wb, h->value);
    xIOBufferAppendStr(wb, "\r\n");
    h = h->next;
  }

  /* End of headers */
  xIOBufferAppendStr(wb, "\r\n");

  /* Body */
  if (body && body_len > 0) {
    xIOBufferAppend(wb, body, body_len);
  }

  return 0;
}

/**
 * H1 write_data: append data to write buffer (streaming mode).
 * On first call, flushes headers with Connection: close.
 */
static int h1_write_data(struct xHttpStream_ *stream, const char *data,
                         size_t len) {
  struct xHttpConn_           *conn = stream->conn;
  struct xHttpResponseWriter_ *w    = &stream->writer;

  /* First call: flush headers and enter streaming mode */
  if (!w->streaming) {
    w->streaming     = 1;
    conn->keep_alive = 0; /* streaming responses always close */

    xIOBuffer *wb = &conn->write_buf;

    /* Status line */
    char status_line[64];
    int  slen = snprintf(status_line, sizeof(status_line), "HTTP/1.1 %d %s\r\n",
                         w->status_code, xHttpStatusReason(w->status_code));
    xIOBufferAppend(wb, status_line, (size_t)slen);

    /* Connection: close (no Content-Length in streaming) */
    xIOBufferAppendStr(wb, "Connection: close\r\n");

    /* User-set headers */
    struct xHttpHeader_ *h = w->headers;
    while (h) {
      xIOBufferAppendStr(wb, h->key);
      xIOBufferAppendStr(wb, ": ");
      xIOBufferAppendStr(wb, h->value);
      xIOBufferAppendStr(wb, "\r\n");
      h = h->next;
    }

    /* End of headers */
    xIOBufferAppendStr(wb, "\r\n");
  }

  /* Append data */
  if (data && len > 0) {
    xIOBufferAppend(&conn->write_buf, data, len);
  }

  return 0;
}

/**
 * H1 end_stream: mark stream as complete.
 */
static int h1_end_stream(struct xHttpStream_ *stream) {
  stream->writer.sent = 1;
  return 0;
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  Initialization
 * ═══════════════════════════════════════════════════════════════════════════
 */

int xHttpProtoH1Init(struct xHttpConn_ *conn) {
  xHttpProtoH1 *h1 = (xHttpProtoH1 *)calloc(1, sizeof(xHttpProtoH1));
  if (!h1) return -1;

  /* Create the implicit H1 stream (stream_id = 0) */
  conn->stream = xHttpStreamCreate(conn, 0);
  if (!conn->stream) {
    free(h1);
    return -1;
  }

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
  conn->proto.on_data           = h1_on_data;
  conn->proto.reset             = h1_reset;
  conn->proto.destroy           = h1_destroy;
  conn->proto.method            = h1_method;
  conn->proto.should_keep_alive = h1_should_keep_alive;
  conn->proto.send_response     = h1_send_response;
  conn->proto.write_data        = h1_write_data;
  conn->proto.end_stream        = h1_end_stream;
  conn->proto.state             = h1;

  return 0;
}
