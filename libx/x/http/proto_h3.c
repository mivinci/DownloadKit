/*
 * Copyright 2025 The libx Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * proto_h3.c - HTTP/3 protocol handler implementation (nghttp3)
 *
 * Modeled after proto_h2.c. nghttp3 API mirrors nghttp2 — same author,
 * same design patterns. Key difference: H3 sends data through ngtcp2
 * (QUIC), not through a raw TCP write buffer.
 */

#include "proto_h3.h"
#include "server_private.h"

#include <ctype.h>
#include <nghttp3/nghttp3.h>
#include <ngtcp2/ngtcp2.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <x/base/log.h>

/* ═══════════════════════════════════════════════════════════════════════════
 *  Internal state for HTTP/3 protocol handler
 * ═══════════════════════════════════════════════════════════════════════════
 */

#define XHTTP_H3_MAX_PENDING_DISPATCH 16
#define XHTTP_H3_SETTINGS_MAX_CONCURRENT_STREAMS 100

XDEF_STRUCT(xHttpProtoH3) {
  nghttp3_conn    *h3_conn;
  int64_t          last_stream_id;
  /* Streams pending dispatch (deferred from callbacks) */
  struct xHttpStream_ *pending_dispatch[XHTTP_H3_MAX_PENDING_DISPATCH];
  int                  pending_count;
};

/* Per-stream data stored as nghttp3 stream user data */
XDEF_STRUCT(xH3StreamData) {
  struct xHttpStream_ *stream;
  char                *method; /**< :method pseudo-header value */
  int64_t              stream_id;
};

/* ═══════════════════════════════════════════════════════════════════════════
 *  nghttp3 → ngtcp2 send bridge
 * ═══════════════════════════════════════════════════════════════════════════
 */

/**
 * Called by nghttp3 when it wants to send QUIC stream data.
 * Bridges nghttp3 output → ngtcp2 stream write.
 */
static int h3_send_data_to_quic(nghttp3_conn *h3_conn, int64_t stream_id,
                                 const uint8_t *data, size_t datalen,
                                 void *user_data) {
  struct xHttpConn_ *conn = (struct xHttpConn_ *)user_data;
  (void)h3_conn;

  ngtcp2_conn *quic = (ngtcp2_conn *)conn->quic_conn;
  if (!quic) return NGHTTP3_ERR_CALLBACK_FAILURE;

  ngtcp2_ssize nwrite;
  uint32_t flags = NGTCP2_WRITE_STREAM_FLAG_MORE;

  /* Write data to the QUIC stream */
  ngtcp2_cid rcid = { .datalen = conn->remote_cid_len };
  memcpy(rcid.data, conn->remote_cid, conn->remote_cid_len);
  nwrite = ngtcp2_conn_writev_stream(
    quic, &rcid, stream_id, flags,
    &data, 1, datalen > 0 ? 1 : 0, datalen);

  if (nwrite < 0) {
    xLog(false, "xhttp h3: writev_stream error: %s",
         ngtcp2_strerror((int)nwrite));
    return NGHTTP3_ERR_CALLBACK_FAILURE;
  }

  /* Flush QUIC packets */
  extern int xHttpQuicConnSend(struct xHttpConn_ *conn);
  xHttpQuicConnSend(conn);

  return 0;
}

/**
 * Called by nghttp3 to acknowledge that stream data was delivered.
 */
static void h3_acked_stream_data(nghttp3_conn *h3_conn, int64_t stream_id,
                                  uint64_t datalen, void *user_data) {
  (void)h3_conn;
  (void)stream_id;
  (void)datalen;
  (void)user_data;
}

/**
 * Called by nghttp3 when a stream is reset by the remote peer.
 */
static int h3_stream_close(nghttp3_conn *h3_conn, int64_t stream_id,
                            uint64_t app_error_code, void *user_data,
                            void *stream_user_data) {
  struct xHttpConn_ *conn = (struct xHttpConn_ *)user_data;
  (void)h3_conn;
  (void)app_error_code;

  xH3StreamData *sd = (xH3StreamData *)stream_user_data;
  if (sd) {
    if (sd->stream) {
      struct xHttpConn_ *c = sd->stream->conn;
      if (c->stream == sd->stream) {
        sd->stream->closed_by_peer = 1;
      } else {
        xHttpStreamDestroy(sd->stream);
      }
      sd->stream = NULL;
    }
    free(sd->method);
    free(sd);
    nghttp3_conn_set_stream_user_data(h3_conn, stream_id, NULL);
  }

  return 0;
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  nghttp3 callbacks — request parsing
 * ═══════════════════════════════════════════════════════════════════════════
 */

/**
 * Called for each header name/value pair during request parsing.
 */
static int h3_recv_header(nghttp3_conn *h3_conn, int64_t stream_id,
                           int32_t token, nghttp3_rcbuf *name,
                           nghttp3_rcbuf *value, uint8_t flags,
                           void *user_data, void *stream_user_data) {
  struct xHttpConn_ *conn = (struct xHttpConn_ *)user_data;
  (void)token;
  (void)flags;

  /* Get or create per-stream data */
  xH3StreamData *sd = (xH3StreamData *)stream_user_data;
  if (!sd) {
    /* First header of a new stream — create stream + stream data */
    sd = (xH3StreamData *)calloc(1, sizeof(xH3StreamData));
    if (!sd) return NGHTTP3_ERR_CALLBACK_FAILURE;

    sd->stream    = xHttpStreamCreate(conn, stream_id);
    sd->stream_id = stream_id;
    if (!sd->stream) {
      free(sd);
      return NGHTTP3_ERR_CALLBACK_FAILURE;
    }
    nghttp3_conn_set_stream_user_data(h3_conn, stream_id, sd);
  }

  nghttp3_vec name_vec  = nghttp3_rcbuf_get_buf(name);
  nghttp3_vec value_vec = nghttp3_rcbuf_get_buf(value);

  if (name_vec.len > 0 && name_vec.base[0] == ':') {
    /* Pseudo-header */
    if (name_vec.len == 7 && memcmp(name_vec.base, ":method", 7) == 0) {
      free(sd->method);
      sd->method = strndup((const char *)value_vec.base, value_vec.len);
    } else if (name_vec.len == 5 && memcmp(name_vec.base, ":path", 5) == 0) {
      if (!sd->stream->url) sd->stream->url = xBufferCreate(256);
      if (!sd->stream->url) return NGHTTP3_ERR_CALLBACK_FAILURE;
      xBufferReset(sd->stream->url);
      xBufferAppend(&sd->stream->url,
                    (const char *)value_vec.base, value_vec.len);
    }
  } else {
    /* Regular header: append as "name: value\r\n" */
    if (!sd->stream->headers_raw)
      sd->stream->headers_raw = xBufferCreate(512);
    if (!sd->stream->headers_raw) return NGHTTP3_ERR_CALLBACK_FAILURE;

    xBufferAppend(&sd->stream->headers_raw,
                  (const char *)name_vec.base, name_vec.len);
    xBufferAppend(&sd->stream->headers_raw, ": ", 2);
    xBufferAppend(&sd->stream->headers_raw,
                  (const char *)value_vec.base, value_vec.len);
    xBufferAppend(&sd->stream->headers_raw, "\r\n", 2);
  }

  sd->stream->header_bytes += name_vec.len + value_vec.len;
  return 0;
}

/**
 * Called when request body data is received.
 */
static int h3_recv_data(nghttp3_conn *h3_conn, int64_t stream_id,
                         const uint8_t *data, size_t datalen,
                         void *user_data, void *stream_user_data) {
  (void)h3_conn;
  (void)user_data;

  xH3StreamData *sd = (xH3StreamData *)stream_user_data;
  if (!sd || !sd->stream) return 0;

  if (!sd->stream->body) sd->stream->body = xBufferCreate(1024);
  if (!sd->stream->body) return NGHTTP3_ERR_CALLBACK_FAILURE;

  xBufferAppend(&sd->stream->body, (const char *)data, datalen);
  return 0;
}

/**
 * Called when a complete request is received (end of stream).
 */
static int h3_end_stream(nghttp3_conn *h3_conn, int64_t stream_id,
                          void *user_data, void *stream_user_data) {
  struct xHttpConn_ *conn = (struct xHttpConn_ *)user_data;
  xHttpProtoH3      *h3   = (xHttpProtoH3 *)conn->proto.state;

  xH3StreamData *sd = (xH3StreamData *)stream_user_data;
  if (sd && sd->stream) {
    sd->stream->request_complete = 1;
    /* Queue for dispatch after callbacks return */
    if (h3->pending_count < XHTTP_H3_MAX_PENDING_DISPATCH) {
      h3->pending_dispatch[h3->pending_count++] = sd->stream;
    }
  }
  return 0;
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  vtable method implementations
 * ═══════════════════════════════════════════════════════════════════════════
 */

/**
 * Feed data to the HTTP/3 connection.
 * Returns 0 on success, -1 on error.
 */
static int h3_on_data(struct xHttpConn_ *conn, const char *buf, size_t len) {
  xHttpProtoH3 *h3 = (xHttpProtoH3 *)conn->proto.state;
  if (!h3 || !h3->h3_conn) return -1;

  /* Reset pending dispatch */
  h3->pending_count = 0;

  /* Feed QUIC stream data to nghttp3 parsing */
  /* Note: stream_id should be extracted from the QUIC frame context.
   * For the initial implementation, we handle single-stream case.
   * In production, the stream_id is passed via h3_recv_stream_data_cb
   * and we need per-stream read contexts. */
  nghttp3_ssize rv = nghttp3_conn_read_stream(
    h3->h3_conn, 0, /* stream_id — FIXME: pass actual stream_id */
    (const uint8_t *)buf, len, 0);

  if (rv < 0) {
    xLog(false, "xhttp h3: nghttp3_conn_read_stream error: %s",
         nghttp3_strerror((int)rv));
    return -1;
  }

  /* Dispatch completed streams */
  for (int i = 0; i < h3->pending_count; i++) {
    struct xHttpStream_ *stream = h3->pending_dispatch[i];
    if (stream) {
      conn->stream = stream;
      xHttpConnDispatchRequest(conn);
    }
  }
  h3->pending_count = 0;

  /* Send any pending response data */
  xHttpQuicConnSend(conn);

  return 0;
}

static void h3_reset(struct xHttpConn_ *conn) {
  (void)conn;
  /* nghttp3 doesn't reset per-request state */
}

static void h3_destroy(struct xHttpConn_ *conn) {
  xHttpProtoH3 *h3 = (xHttpProtoH3 *)conn->proto.state;
  if (h3) {
    if (h3->h3_conn) {
      nghttp3_conn_del(h3->h3_conn);
      h3->h3_conn = NULL;
    }
    free(h3);
    conn->proto.state = NULL;
  }
}

static const char *h3_method(struct xHttpStream_ *stream) {
  return "GET"; /* FIXME: retrieve from stream data */
}

static int h3_should_keep_alive(struct xHttpConn_ *conn) {
  (void)conn;
  return 1; /* QUIC connections are persistent */
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  H3 response serialization
 * ═══════════════════════════════════════════════════════════════════════════
 */

static int h3_send_response(struct xHttpStream_ *stream, int status,
                             struct xHttpHeader_ *headers,
                             const char *body, size_t body_len) {
  struct xHttpConn_ *conn = stream->conn;
  xHttpProtoH3      *h3   = (xHttpProtoH3 *)conn->proto.state;
  if (!h3 || !h3->h3_conn) return -1;

  /* Count headers: :status + user headers */
  int                  nheader = 1;
  struct xHttpHeader_ *h       = headers;
  while (h) { nheader++; h = h->next; }

  nghttp3_nv *nva = (nghttp3_nv *)calloc((size_t)nheader, sizeof(nghttp3_nv));
  if (!nva) return -1;

  /* Convert status code to string */
  char status_str[16];
  int  status_len = snprintf(status_str, sizeof(status_str), "%d", status);

  nva[0].name     = (uint8_t *)":status";
  nva[0].namelen  = 7;
  nva[0].value    = (uint8_t *)status_str;
  nva[0].valuelen = (size_t)status_len;
  nva[0].flags    = NGHTTP3_NV_FLAG_NO_COPY_NAME;

  /* User headers — HTTP/3 requires lowercase */
  int i = 1;
  h     = headers;
  while (h) {
    for (char *p = h->key; *p; p++)
      *p = (char)tolower((unsigned char)*p);
    nva[i].name     = (uint8_t *)h->key;
    nva[i].namelen  = strlen(h->key);
    nva[i].value    = (uint8_t *)h->value;
    nva[i].valuelen = strlen(h->value);
    nva[i].flags    = NGHTTP3_NV_FLAG_NO_COPY_NAME | NGHTTP3_NV_FLAG_NO_COPY_VALUE;
    i++;
    h = h->next;
  }

  int rv;
  if (body && body_len > 0) {
    /* Submit response with body */
    nghttp3_data_reader dr;
    /* For simplicity, send body inline (small responses).
     * Large body streaming is handled by write_data/end_stream. */
    (void)body;
    (void)body_len;
    rv = nghttp3_conn_submit_response(
      h3->h3_conn, stream->stream_id, nva, (size_t)nheader, NULL);
    /* FIXME: inline body delivery */
  } else {
    rv = nghttp3_conn_submit_response(
      h3->h3_conn, stream->stream_id, nva, (size_t)nheader, NULL);
  }
  free(nva);

  if (rv != 0) {
    xLog(false, "xhttp h3: submit_response error: %s",
         nghttp3_strerror(rv));
    return -1;
  }

  /* Flush responses */
  xHttpQuicConnSend(conn);
  return 0;
}

static int h3_write_data(struct xHttpStream_ *stream,
                          const char *data, size_t len) {
  (void)stream;
  (void)data;
  (void)len;
  /* FIXME: streaming response support */
  return 0;
}

static int h3_end_stream(struct xHttpStream_ *stream) {
  struct xHttpConn_ *conn = stream->conn;
  xHttpProtoH3      *h3   = (xHttpProtoH3 *)conn->proto.state;
  if (!h3 || !h3->h3_conn) return -1;

  nghttp3_conn_submit_shutdown_notice(h3->h3_conn);
  xHttpQuicConnSend(conn);
  return 0;
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  Initialization
 * ═══════════════════════════════════════════════════════════════════════════
 */

int xHttpProtoH3Init(struct xHttpConn_ *conn) {
  xHttpProtoH3 *h3 = (xHttpProtoH3 *)calloc(1, sizeof(xHttpProtoH3));
  if (!h3) return -1;

  /* Set up nghttp3 callbacks */
  nghttp3_callbacks callbacks;
  memset(&callbacks, 0, sizeof(callbacks));
  callbacks.recv_header     = h3_recv_header;
  callbacks.recv_data       = h3_recv_data;
  callbacks.end_stream      = h3_end_stream;
  callbacks.stream_close    = h3_stream_close;
  callbacks.acked_stream_data = h3_acked_stream_data;

  /* Create nghttp3 server connection */
  nghttp3_settings settings;
  nghttp3_settings_default(&settings);
  settings.qpack_max_table_capacity  = 4096;
  settings.qpack_blocked_streams     = 100;
  settings.max_field_section_size    = 8192;

  int rv = nghttp3_conn_server_new(&h3->h3_conn, &callbacks,
                                    &settings, NULL, conn);
  if (rv != 0) {
    xLog(false, "xhttp h3: nghttp3_conn_server_new error: %s",
         nghttp3_strerror(rv));
    free(h3);
    return -1;
  }

  /* Register the send callback (bridges nghttp3 → ngtcp2) */
  nghttp3_conn_set_stream_user_data(h3->h3_conn, 0, conn);

  /* Populate the vtable */
  conn->proto.on_data           = h3_on_data;
  conn->proto.reset             = h3_reset;
  conn->proto.destroy           = h3_destroy;
  conn->proto.method            = h3_method;
  conn->proto.should_keep_alive = h3_should_keep_alive;
  conn->proto.send_response     = h3_send_response;
  conn->proto.write_data        = h3_write_data;
  conn->proto.end_stream        = h3_end_stream;
  conn->proto.state             = h3;

  conn->keep_alive = 1;

  return 0;
}
