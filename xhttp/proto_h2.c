/*
 * Copyright 2025 The xKit Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * proto_h2.c - HTTP/2 protocol handler implementation (nghttp2)
 */

#include "proto_h2.h"
#include "server_private.h"

#include <ctype.h>
#include <nghttp2/nghttp2.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ═══════════════════════════════════════════════════════════════════════════
 *  Internal state for HTTP/2 protocol handler
 * ═══════════════════════════════════════════════════════════════════════════ */

#define XHTTP_H2_MAX_PENDING_DISPATCH 16

typedef struct xHttpProtoH2_ {
  nghttp2_session *session;
  /* Streams pending dispatch (filled during mem_recv callbacks,
   * dispatched after mem_recv returns) */
  struct xHttpStream_ *pending_dispatch[XHTTP_H2_MAX_PENDING_DISPATCH];
  int                  pending_count;
} xHttpProtoH2;

/* Per-stream data stored as nghttp2 stream user data */
typedef struct xH2StreamData_ {
  struct xHttpStream_ *stream;
  char                *method;  /**< :method pseudo-header value */
} xH2StreamData;

/* ═══════════════════════════════════════════════════════════════════════════
 *  nghttp2 callbacks
 * ═══════════════════════════════════════════════════════════════════════════ */

/**
 * nghttp2 send callback: write frame data to conn->write_buf.
 */
static ssize_t h2_send_callback(nghttp2_session *session,
                                const uint8_t *data, size_t length,
                                int flags, void *user_data) {
  (void)session;
  (void)flags;
  struct xHttpConn_ *conn = (struct xHttpConn_ *)user_data;
  xIOBufferAppend(&conn->write_buf, (const char *)data, length);
  return (ssize_t)length;
}

/**
 * Called when nghttp2 begins receiving headers for a new stream.
 * Creates a new xHttpStream_ for this stream.
 */
static int h2_on_begin_headers_callback(nghttp2_session *session,
                                        const nghttp2_frame *frame,
                                        void *user_data) {
  struct xHttpConn_ *conn = (struct xHttpConn_ *)user_data;

  if (frame->hd.type != NGHTTP2_HEADERS ||
      frame->headers.cat != NGHTTP2_HCAT_REQUEST) {
    return 0;
  }

  /* Create a new stream for this request */
  struct xHttpStream_ *stream = xHttpStreamCreate(conn, frame->hd.stream_id);
  if (!stream) {
    return NGHTTP2_ERR_CALLBACK_FAILURE;
  }

  /* Create per-stream data */
  xH2StreamData *sd = (xH2StreamData *)calloc(1, sizeof(xH2StreamData));
  if (!sd) {
    xHttpStreamDestroy(stream);
    return NGHTTP2_ERR_CALLBACK_FAILURE;
  }
  sd->stream = stream;
  sd->method = NULL;

  /* Associate stream data with nghttp2 stream */
  nghttp2_session_set_stream_user_data(session, frame->hd.stream_id, sd);

  /* Set as current stream on connection (for dispatch) */
  conn->stream = stream;

  return 0;
}

/**
 * Called for each header name/value pair.
 * Pseudo-headers (:method, :path) are stored specially;
 * regular headers go into headers_raw.
 */
static int h2_on_header_callback(nghttp2_session *session,
                                 const nghttp2_frame *frame,
                                 const uint8_t *name, size_t namelen,
                                 const uint8_t *value, size_t valuelen,
                                 uint8_t flags, void *user_data) {
  (void)flags;
  (void)user_data;

  if (frame->hd.type != NGHTTP2_HEADERS ||
      frame->headers.cat != NGHTTP2_HCAT_REQUEST) {
    return 0;
  }

  xH2StreamData *sd = (xH2StreamData *)nghttp2_session_get_stream_user_data(
      session, frame->hd.stream_id);
  if (!sd) return 0;

  struct xHttpStream_ *stream = sd->stream;

  if (namelen > 0 && name[0] == ':') {
    /* Pseudo-header */
    if (namelen == 7 && memcmp(name, ":method", 7) == 0) {
      /* Store method string */
      free(sd->method);
      sd->method = strndup((const char *)value, valuelen);
    } else if (namelen == 5 && memcmp(name, ":path", 5) == 0) {
      /* Store URL */
      if (!stream->url) stream->url = xBufferCreate(256);
      if (!stream->url) return NGHTTP2_ERR_CALLBACK_FAILURE;
      xBufferReset(stream->url);
      if (xBufferAppend(&stream->url, (const char *)value, valuelen) != xErrno_Ok)
        return NGHTTP2_ERR_CALLBACK_FAILURE;
    }
    /* :scheme and :authority are ignored for now */
  } else {
    /* Regular header: append to headers_raw as "name: value\r\n" */
    if (!stream->headers_raw) stream->headers_raw = xBufferCreate(512);
    if (!stream->headers_raw) return NGHTTP2_ERR_CALLBACK_FAILURE;

    if (xBufferAppend(&stream->headers_raw, (const char *)name, namelen) != xErrno_Ok)
      return NGHTTP2_ERR_CALLBACK_FAILURE;
    if (xBufferAppend(&stream->headers_raw, ": ", 2) != xErrno_Ok)
      return NGHTTP2_ERR_CALLBACK_FAILURE;
    if (xBufferAppend(&stream->headers_raw, (const char *)value, valuelen) != xErrno_Ok)
      return NGHTTP2_ERR_CALLBACK_FAILURE;
    if (xBufferAppend(&stream->headers_raw, "\r\n", 2) != xErrno_Ok)
      return NGHTTP2_ERR_CALLBACK_FAILURE;
  }

  stream->header_bytes += namelen + valuelen;

  return 0;
}

/**
 * Called when a chunk of request body data is received.
 */
static int h2_on_data_chunk_recv_callback(nghttp2_session *session,
                                          uint8_t flags,
                                          int32_t stream_id,
                                          const uint8_t *data, size_t len,
                                          void *user_data) {
  (void)flags;
  (void)user_data;

  xH2StreamData *sd = (xH2StreamData *)nghttp2_session_get_stream_user_data(
      session, stream_id);
  if (!sd) return 0;

  struct xHttpStream_ *stream = sd->stream;

  if (!stream->body) stream->body = xBufferCreate(1024);
  if (!stream->body) return NGHTTP2_ERR_CALLBACK_FAILURE;
  if (xBufferAppend(&stream->body, (const char *)data, len) != xErrno_Ok)
    return NGHTTP2_ERR_CALLBACK_FAILURE;

  return 0;
}

/**
 * Called when a complete frame is received.
 * When HEADERS or DATA frame with END_STREAM flag is received,
 * the request is complete and should be dispatched.
 */
static int h2_on_frame_recv_callback(nghttp2_session *session,
                                     const nghttp2_frame *frame,
                                     void *user_data) {
  struct xHttpConn_ *conn = (struct xHttpConn_ *)user_data;
  xHttpProtoH2 *h2 = (xHttpProtoH2 *)conn->proto.state;

  switch (frame->hd.type) {
    case NGHTTP2_HEADERS:
    case NGHTTP2_DATA: {
      /* Check for END_STREAM flag */
      if (frame->hd.flags & NGHTTP2_FLAG_END_STREAM) {
        xH2StreamData *sd = (xH2StreamData *)nghttp2_session_get_stream_user_data(
            session, frame->hd.stream_id);
        if (sd && sd->stream) {
          sd->stream->request_complete = 1;
          /* Queue for dispatch after mem_recv returns (avoid re-entrancy) */
          if (h2->pending_count < XHTTP_H2_MAX_PENDING_DISPATCH) {
            h2->pending_dispatch[h2->pending_count++] = sd->stream;
          }
        }
      }
      break;
    }
    default:
      break;
  }

  return 0;
}

/**
 * Called when a stream is closed.
 * Frees the per-stream data.
 */
static int h2_on_stream_close_callback(nghttp2_session *session,
                                       int32_t stream_id,
                                       uint32_t error_code,
                                       void *user_data) {
  (void)error_code;
  (void)user_data;

  xH2StreamData *sd = (xH2StreamData *)nghttp2_session_get_stream_user_data(
      session, stream_id);
  if (sd) {
    if (sd->stream) {
      struct xHttpConn_ *conn = sd->stream->conn;
      /* If this is the current stream (being dispatched), don't destroy it
       * yet. The dispatch code will handle cleanup via conn_after_response.
       * Just clear the conn->stream pointer so conn_after_response knows
       * the stream is done. */
      if (conn->stream == sd->stream) {
        /* Stream is still being dispatched; defer destruction.
         * conn_after_response will detect conn->stream == NULL and skip
         * reset. The stream will be freed when the connection closes or
         * when we explicitly clean up here after dispatch. */
        xHttpStreamDestroy(sd->stream);
        conn->stream = NULL;
      } else {
        xHttpStreamDestroy(sd->stream);
      }
      sd->stream = NULL;
    }
    free(sd->method);
    free(sd);
    nghttp2_session_set_stream_user_data(session, stream_id, NULL);
  }

  return 0;
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  vtable method implementations
 * ═══════════════════════════════════════════════════════════════════════════ */

/**
 * Feed data to the HTTP/2 session.
 * Returns:  0 = processed (H2 handles dispatch internally via callbacks)
 *          -1 = error
 */
static int h2_on_data(struct xHttpConn_ *conn, const char *buf, size_t len) {
  xHttpProtoH2 *h2 = (xHttpProtoH2 *)conn->proto.state;

  /* Reset pending dispatch list */
  h2->pending_count = 0;

  ssize_t rv = nghttp2_session_mem_recv(h2->session, (const uint8_t *)buf, len);
  if (rv < 0) {
    fprintf(stderr, "xhttp h2: nghttp2_session_mem_recv error: %s\n",
            nghttp2_strerror((int)rv));
    return -1;
  }

  /* Send any pending frames (e.g. SETTINGS ACK) */
  rv = nghttp2_session_send(h2->session);
  if (rv != 0) {
    fprintf(stderr, "xhttp h2: nghttp2_session_send error: %s\n",
            nghttp2_strerror((int)rv));
    return -1;
  }

  /* Now dispatch all completed streams (safe: outside of mem_recv) */
  for (int i = 0; i < h2->pending_count; i++) {
    struct xHttpStream_ *stream = h2->pending_dispatch[i];
    if (stream) {
      conn->stream = stream;
      xHttpConnDispatchRequest(conn);
    }
  }
  h2->pending_count = 0;

  /* Send response frames submitted during dispatch */
  rv = nghttp2_session_send(h2->session);
  if (rv != 0) {
    fprintf(stderr, "xhttp h2: nghttp2_session_send (post-dispatch) error: %s\n",
            nghttp2_strerror((int)rv));
    return -1;
  }

  /* Try to flush the write buffer */
  xHttpConnTryFlush(conn);

  /* H2 handles dispatch internally,
   * so always return 0 (need more data) to the event loop */
  return 0;
}

/**
 * Reset the HTTP/2 session (not applicable for H2; no-op).
 */
static void h2_reset(struct xHttpConn_ *conn) {
  (void)conn;
  /* H2 doesn't reset per-request like H1; streams are independent */
}

/**
 * Destroy the HTTP/2 protocol handler.
 */
static void h2_destroy(struct xHttpConn_ *conn) {
  xHttpProtoH2 *h2 = (xHttpProtoH2 *)conn->proto.state;
  if (h2) {
    if (h2->session) {
      nghttp2_session_del(h2->session);
      h2->session = NULL;
    }
    free(h2);
    conn->proto.state = NULL;
  }
}

/**
 * Get the HTTP method string for the current H2 stream.
 */
static const char *h2_method(struct xHttpStream_ *stream) {
  if (!stream || !stream->conn) return "GET";

  xHttpProtoH2 *h2 = (xHttpProtoH2 *)stream->conn->proto.state;
  if (!h2 || !h2->session) return "GET";

  xH2StreamData *sd = (xH2StreamData *)nghttp2_session_get_stream_user_data(
      h2->session, stream->stream_id);
  if (sd && sd->method) {
    return sd->method;
  }
  return "GET";
}

/**
 * Check if the H2 connection should be kept alive.
 * H2 connections are always kept alive (multiplexed).
 */
static int h2_should_keep_alive(struct xHttpConn_ *conn) {
  (void)conn;
  return 1; /* H2 connections are persistent */
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  H2 response serialization
 * ═══════════════════════════════════════════════════════════════════════════ */

/* Body source for nghttp2 data provider */
typedef struct h2_body_source_ {
  const char *data;
  size_t      len;
  size_t      pos;
} h2_body_source;

/**
 * nghttp2 data provider read callback: feeds body data to nghttp2.
 */
static ssize_t h2_body_read_callback(nghttp2_session *session,
                                     int32_t stream_id,
                                     uint8_t *buf, size_t length,
                                     uint32_t *data_flags,
                                     nghttp2_data_source *source,
                                     void *user_data) {
  (void)session;
  (void)stream_id;
  (void)user_data;
  h2_body_source *src = (h2_body_source *)source->ptr;

  size_t remaining = src->len - src->pos;
  size_t to_copy = remaining < length ? remaining : length;

  if (to_copy > 0) {
    memcpy(buf, src->data + src->pos, to_copy);
    src->pos += to_copy;
  }

  if (src->pos >= src->len) {
    *data_flags |= NGHTTP2_DATA_FLAG_EOF;
  }

  return (ssize_t)to_copy;
}

/**
 * H2 send_response: submit response headers and body via nghttp2.
 */
static int h2_send_response(struct xHttpStream_ *stream, int status,
                            struct xHttpHeader_ *headers,
                            const char *body, size_t body_len) {
  struct xHttpConn_ *conn = stream->conn;
  xHttpProtoH2 *h2 = (xHttpProtoH2 *)conn->proto.state;

  /* Count headers: :status + user headers */
  int nheader = 1; /* :status */
  struct xHttpHeader_ *h = headers;
  while (h) {
    nheader++;
    h = h->next;
  }

  /* Build nghttp2 nv array */
  nghttp2_nv *nva = (nghttp2_nv *)calloc((size_t)nheader, sizeof(nghttp2_nv));
  if (!nva) return -1;

  /* :status pseudo-header */
  char status_str[16];
  int status_len = snprintf(status_str, sizeof(status_str), "%d", status);

  nva[0].name     = (uint8_t *)":status";
  nva[0].namelen  = 7;
  nva[0].value    = (uint8_t *)status_str;
  nva[0].valuelen = (size_t)status_len;
  nva[0].flags    = NGHTTP2_NV_FLAG_NO_COPY_NAME;

  /* User headers — HTTP/2 requires lowercase header names */
  int i = 1;
  h = headers;
  while (h) {
    /* Lowercase the header key in-place (h->key is strdup'd, safe to modify) */
    for (char *p = h->key; *p; p++) *p = (char)tolower((unsigned char)*p);
    nva[i].name     = (uint8_t *)h->key;
    nva[i].namelen  = strlen(h->key);
    nva[i].value    = (uint8_t *)h->value;
    nva[i].valuelen = strlen(h->value);
    nva[i].flags    = NGHTTP2_NV_FLAG_NO_COPY_NAME | NGHTTP2_NV_FLAG_NO_COPY_VALUE;
    i++;
    h = h->next;
  }

  int rv;
  if (body && body_len > 0) {
    /* Allocate body source on the heap (freed by nghttp2 after use) */
    h2_body_source *src = (h2_body_source *)malloc(sizeof(h2_body_source));
    if (!src) {
      free(nva);
      return -1;
    }
    src->data = body;
    src->len  = body_len;
    src->pos  = 0;

    nghttp2_data_provider data_prd;
    data_prd.source.ptr    = src;
    data_prd.read_callback = h2_body_read_callback;

    rv = nghttp2_submit_response(h2->session, stream->stream_id,
                                 nva, (size_t)nheader, &data_prd);
    free(nva);
    if (rv != 0) {
      free(src);
      return -1;
    }
  } else {
    /* No body: submit response (nghttp2 will set END_STREAM on HEADERS) */
    rv = nghttp2_submit_response(h2->session, stream->stream_id,
                                 nva, (size_t)nheader, NULL);
    free(nva);
    if (rv != 0) return -1;
  }

  /* Don't call nghttp2_session_send() here — we're likely inside
   * a mem_recv callback chain. The caller (h2_on_data) will call
   * session_send after all dispatches are done. */

  return 0;
}

/**
 * H2 write_data: submit DATA frame for streaming response.
 * (Simplified: not yet fully implemented for streaming)
 */
static int h2_write_data(struct xHttpStream_ *stream,
                         const char *data, size_t len) {
  (void)stream;
  (void)data;
  (void)len;
  /* TODO: Implement H2 streaming DATA frames */
  return 0;
}

/**
 * H2 end_stream: signal end of response stream.
 */
static int h2_end_stream(struct xHttpStream_ *stream) {
  stream->writer.sent = 1;
  return 0;
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  Initialization
 * ═══════════════════════════════════════════════════════════════════════════ */

int xHttpProtoH2Init(struct xHttpConn_ *conn) {
  xHttpProtoH2 *h2 = (xHttpProtoH2 *)calloc(1, sizeof(xHttpProtoH2));
  if (!h2) return -1;

  /* Set up nghttp2 callbacks */
  nghttp2_session_callbacks *callbacks;
  int rv = nghttp2_session_callbacks_new(&callbacks);
  if (rv != 0) {
    free(h2);
    return -1;
  }

  nghttp2_session_callbacks_set_send_callback(callbacks, h2_send_callback);
  nghttp2_session_callbacks_set_on_begin_headers_callback(
      callbacks, h2_on_begin_headers_callback);
  nghttp2_session_callbacks_set_on_header_callback(
      callbacks, h2_on_header_callback);
  nghttp2_session_callbacks_set_on_data_chunk_recv_callback(
      callbacks, h2_on_data_chunk_recv_callback);
  nghttp2_session_callbacks_set_on_frame_recv_callback(
      callbacks, h2_on_frame_recv_callback);
  nghttp2_session_callbacks_set_on_stream_close_callback(
      callbacks, h2_on_stream_close_callback);

  /* Create server session */
  rv = nghttp2_session_server_new(&h2->session, callbacks, conn);
  nghttp2_session_callbacks_del(callbacks);

  if (rv != 0) {
    free(h2);
    return -1;
  }

  /* Send server connection preface (SETTINGS frame) */
  nghttp2_settings_entry settings[] = {
    {NGHTTP2_SETTINGS_MAX_CONCURRENT_STREAMS, 100},
  };
  rv = nghttp2_submit_settings(h2->session, NGHTTP2_FLAG_NONE,
                               settings, 1);
  if (rv != 0) {
    nghttp2_session_del(h2->session);
    free(h2);
    return -1;
  }

  /* Flush the SETTINGS frame */
  rv = nghttp2_session_send(h2->session);
  if (rv != 0) {
    nghttp2_session_del(h2->session);
    free(h2);
    return -1;
  }

  /* Populate the vtable */
  conn->proto.on_data           = h2_on_data;
  conn->proto.reset             = h2_reset;
  conn->proto.destroy           = h2_destroy;
  conn->proto.method            = h2_method;
  conn->proto.should_keep_alive = h2_should_keep_alive;
  conn->proto.send_response     = h2_send_response;
  conn->proto.write_data        = h2_write_data;
  conn->proto.end_stream        = h2_end_stream;
  conn->proto.state             = h2;

  /* H2 doesn't create a default stream; streams are created on demand
   * via on_begin_headers_callback */
  conn->keep_alive = 1; /* H2 connections are persistent */

  return 0;
}
