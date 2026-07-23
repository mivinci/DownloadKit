/*
 * Copyright 2025 The libx Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * proto_h3.c - HTTP/3 protocol handler implementation (nghttp3)
 *
 * Modeled after proto_h2.c. Unlike nghttp2 (which has a send callback),
 * nghttp3 requires explicit serialization: after submitting a response,
 * call nghttp3_conn_writev_stream() → ngtcp2_conn_writev_stream().
 */

#include "proto_h3.h"
#include "server_private.h"

#include <ctype.h>
#include <errno.h>
#include <time.h>
#include <nghttp3/nghttp3.h>
#include <ngtcp2/ngtcp2.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <x/base/log.h>

/* ═══════════════════════════════════════════════════════════════════════════
 *  Configuration
 * ═══════════════════════════════════════════════════════════════════════════
 */

#define XHTTP_H3_MAX_PENDING_DISPATCH 16
#define XHTTP_H3_FLUSH_BUF_SIZE       65536

/* Get monotonic time in nanoseconds (for ngtcp2) */
static ngtcp2_tstamp h3_timestamp(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (ngtcp2_tstamp)ts.tv_sec * 1000000000ULL + (ngtcp2_tstamp)ts.tv_nsec;
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  Internal state
 * ═══════════════════════════════════════════════════════════════════════════
 */

XDEF_STRUCT(xHttpProtoH3) {
  nghttp3_conn    *h3_conn;
  struct xHttpStream_ *pending_dispatch[XHTTP_H3_MAX_PENDING_DISPATCH];
  int                  pending_count;
  /* Per-stream data lookup: an xArray of xH3StreamData* indexed by
   * stream_id won't work (sparse). Use integer-keyed map or store
   * in nghttp3 stream user data. */
};

/* Per-stream user data (set via nghttp3_conn_set_stream_user_data) */
XDEF_STRUCT(xH3StreamData) {
  struct xHttpStream_ *stream;
  char                *method;
  /* For h3_body_read callback — set before flushing responses */
  const uint8_t       *body_data;
  size_t               body_len;
  size_t               body_pos;
};

/* ═══════════════════════════════════════════════════════════════════════════
 *  External dependency: QUIC send (from server_quic.c)
 * ═══════════════════════════════════════════════════════════════════════════
 *
 * We call xHttpQuicConnSend() which is declared in server_private.h.
 * For proto_h3.c, we also need direct ngtcp2 write access for H3
 * response flush. We do it inline here.
 */

static int h3_quic_flush(struct xHttpConn_ *conn) {
  ngtcp2_conn *quic = (ngtcp2_conn *)conn->quic_conn;
  if (!quic) return 0;

  uint8_t buf[XHTTP_H3_FLUSH_BUF_SIZE];
  ngtcp2_path path;
  ngtcp2_pkt_info pi;
  ngtcp2_ssize nwrite;
  ngtcp2_tstamp ts = h3_timestamp();

  for (;;) {
    memset(&path, 0, sizeof(path));
    path.local.addrlen  = sizeof(struct sockaddr_in);
    path.remote.addrlen = conn->remote_addr.ss_len;
    memcpy(path.remote.addr, &conn->remote_addr, conn->remote_addr.ss_len);

    nwrite = ngtcp2_conn_write_pkt_versioned(
      quic, &path, NGTCP2_PKT_INFO_VERSION, &pi,
      buf, sizeof(buf), ts);

    if (nwrite < 0) {
      if (nwrite == NGTCP2_ERR_WRITE_MORE) {
        continue; /* More data can be written — same pi */
      }
      xLog(false, "xhttp h3: write_pkt error: %s",
           ngtcp2_strerror((int)nwrite));
      return -1;
    }

    /* Send via shared UDP fd */
    struct iovec iov;
    iov.iov_base = buf;
    iov.iov_len  = (size_t)nwrite;

    struct msghdr msg;
    memset(&msg, 0, sizeof(msg));
    msg.msg_name    = (void *)&conn->remote_addr;
    msg.msg_namelen = conn->remote_addr.ss_len;
    msg.msg_iov     = &iov;
    msg.msg_iovlen  = 1;

    ssize_t sent = sendmsg(conn->server->h3_listen_fd, &msg, 0);
    if (sent < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
      xLog(false, "xhttp h3: sendmsg error: %s", strerror(errno));
      return -1;
    }
  }

  /* Re-arm QUIC timer */
  xHttpQuicConnScheduleTimer(conn);
  return 0;
}

/**
 * Serialize and flush all pending HTTP/3 responses through QUIC.
 * Called after dispatch (and after every submit_response call).
 */
static int h3_flush_responses(struct xHttpConn_ *conn) {
  xHttpProtoH3 *h3 = (xHttpProtoH3 *)conn->proto.state;
  ngtcp2_conn  *quic = (ngtcp2_conn *)conn->quic_conn;
  if (!h3 || !h3->h3_conn || !quic) return 0;

  uint8_t   buf[XHTTP_H3_FLUSH_BUF_SIZE];
  ngtcp2_path path;
  ngtcp2_pkt_info pi;
  ngtcp2_tstamp ts = h3_timestamp();
  nghttp3_ssize h3_nwrite;
  int64_t stream_id = -1;
  uint32_t flags    = NGTCP2_WRITE_STREAM_FLAG_MORE;
  nghttp3_vec h3_vec;

  /* For each stream with pending data, serialize H3 → QUIC */
  for (;;) {
    /* Step 1: get serialized H3 data from nghttp3 */
    int fin = 0;
    h3_nwrite = nghttp3_conn_writev_stream(
      h3->h3_conn, &stream_id, &fin, &h3_vec, 1);

    if (h3_nwrite < 0) {
      if (h3_nwrite == NGHTTP3_ERR_WOULDBLOCK) break;
      xLog(false, "xhttp h3: writev_stream error: %s",
           nghttp3_strerror((int)h3_nwrite));
      return -1;
    }

    if (h3_nwrite == 0) break;

    /* Step 2: push through QUIC */
    ngtcp2_vec qvec;
    qvec.base = h3_vec.base;
    qvec.len  = h3_vec.len;

    ngtcp2_ssize q_nwrite;
    uint32_t wflags = flags;
    if (fin) wflags |= NGTCP2_WRITE_STREAM_FLAG_FIN;

    memset(&path, 0, sizeof(path));
    path.local.addrlen  = sizeof(struct sockaddr_in);
    path.remote.addrlen = conn->remote_addr.ss_len;
    memcpy(path.remote.addr, &conn->remote_addr, conn->remote_addr.ss_len);

    q_nwrite = ngtcp2_conn_writev_stream_versioned(
      quic, &path, NGTCP2_PKT_INFO_VERSION, &pi,
      buf, sizeof(buf), NULL, wflags, stream_id,
      &qvec, 1, ts);

    if (q_nwrite < 0) {
      if (q_nwrite == NGTCP2_ERR_WRITE_MORE) {
        /* More data to write — continue the loop */
        continue;
      }
      xLog(false, "xhttp h3: ngtcp2 writev_stream error: %s",
           ngtcp2_strerror((int)q_nwrite));
      return -1;
    }

    /* Send the QUIC packet(s) */
    struct iovec iov;
    iov.iov_base = buf;
    iov.iov_len  = (size_t)q_nwrite;

    struct msghdr msg;
    memset(&msg, 0, sizeof(msg));
    msg.msg_name    = (void *)&conn->remote_addr;
    msg.msg_namelen = conn->remote_addr.ss_len;
    msg.msg_iov     = &iov;
    msg.msg_iovlen  = 1;

    if (sendmsg(conn->server->h3_listen_fd, &msg, 0) < 0) {
      if (errno != EAGAIN && errno != EWOULDBLOCK) {
        xLog(false, "xhttp h3: flush sendmsg error: %s", strerror(errno));
        return -1;
      }
    }
  }

  /* Flush remaining QUIC packets (acks, etc.) */
  xHttpQuicConnScheduleTimer(conn);
  return h3_quic_flush(conn);
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  nghttp3 callbacks — request parsing
 * ═══════════════════════════════════════════════════════════════════════════
 */

static int h3_recv_header(nghttp3_conn *h3_conn, int64_t stream_id,
                           int32_t token, nghttp3_rcbuf *name,
                           nghttp3_rcbuf *value, uint8_t flags,
                           void *user_data, void *stream_user_data) {
  struct xHttpConn_ *conn = (struct xHttpConn_ *)user_data;
  (void)token;
  (void)flags;

  xH3StreamData *sd = (xH3StreamData *)stream_user_data;
  if (!sd) {
    /* First header → create stream */
    sd = (xH3StreamData *)calloc(1, sizeof(xH3StreamData));
    if (!sd) return NGHTTP3_ERR_CALLBACK_FAILURE;

    sd->stream = xHttpStreamCreate(conn, stream_id);
    if (!sd->stream) { free(sd); return NGHTTP3_ERR_CALLBACK_FAILURE; }
    nghttp3_conn_set_stream_user_data(h3_conn, stream_id, sd);
  }

  nghttp3_vec name_vec  = nghttp3_rcbuf_get_buf(name);
  nghttp3_vec value_vec = nghttp3_rcbuf_get_buf(value);

  if (name_vec.len > 0 && name_vec.base[0] == ':') {
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

static int h3_recv_data(nghttp3_conn *h3_conn, int64_t stream_id,
                         const uint8_t *data, size_t datalen,
                         void *user_data, void *stream_user_data) {
  (void)h3_conn;
  (void)stream_id;
  (void)user_data;

  xH3StreamData *sd = (xH3StreamData *)stream_user_data;
  if (!sd || !sd->stream) return 0;

  if (!sd->stream->body) sd->stream->body = xBufferCreate(1024);
  if (!sd->stream->body) return NGHTTP3_ERR_CALLBACK_FAILURE;
  xBufferAppend(&sd->stream->body, (const char *)data, datalen);
  return 0;
}

static int h3_end_stream_cb(nghttp3_conn *h3_conn, int64_t stream_id,
                             void *user_data, void *stream_user_data) {
  struct xHttpConn_ *conn = (struct xHttpConn_ *)user_data;
  xHttpProtoH3      *h3   = (xHttpProtoH3 *)conn->proto.state;
  (void)h3_conn;
  (void)stream_id;

  xH3StreamData *sd = (xH3StreamData *)stream_user_data;
  if (sd && sd->stream) {
    sd->stream->request_complete = 1;
    if (h3->pending_count < XHTTP_H3_MAX_PENDING_DISPATCH) {
      h3->pending_dispatch[h3->pending_count++] = sd->stream;
    }
  }
  return 0;
}

static int h3_stream_close_cb(nghttp3_conn *h3_conn, int64_t stream_id,
                               uint64_t app_error_code, void *user_data,
                               void *stream_user_data) {
  struct xHttpConn_ *conn = (struct xHttpConn_ *)user_data;
  (void)h3_conn;
  (void)app_error_code;

  xH3StreamData *sd = (xH3StreamData *)stream_user_data;
  if (sd) {
    if (sd->stream) {
      if (conn->stream == sd->stream) {
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
 *  vtable methods
 * ═══════════════════════════════════════════════════════════════════════════
 */

static int h3_on_data(struct xHttpConn_ *conn, const char *buf, size_t len,
                       int64_t stream_id) {
  xHttpProtoH3 *h3 = (xHttpProtoH3 *)conn->proto.state;
  if (!h3 || !h3->h3_conn) return -1;

  h3->pending_count = 0;

  /* Feed data to nghttp3 with the QUIC stream_id */
  int finish = 0;
  nghttp3_ssize consumed = nghttp3_conn_read_stream(
    h3->h3_conn, stream_id, (const uint8_t *)buf, len, finish);

  if (consumed < 0) {
    xLog(false, "xhttp h3: read_stream error: %s",
         nghttp3_strerror((int)consumed));
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

  /* Flush any queued responses */
  h3_flush_responses(conn);

  return 0;
}

static void h3_reset(struct xHttpConn_ *conn) {
  (void)conn;
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
  if (!stream || !stream->conn) return "GET";
  xHttpProtoH3 *h3 = (xHttpProtoH3 *)stream->conn->proto.state;
  if (!h3 || !h3->h3_conn) return "GET";

  xH3StreamData *sd = (xH3StreamData *)nghttp3_conn_get_stream_user_data(
    h3->h3_conn, stream->stream_id);
  if (sd && sd->method) return sd->method;
  return "GET";
}

static int h3_should_keep_alive(struct xHttpConn_ *conn) {
  (void)conn;
  return 1;
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  H3 response serialization
 * ═══════════════════════════════════════════════════════════════════════════
 */

static nghttp3_ssize h3_body_read(nghttp3_conn *conn, int64_t stream_id,
                                   nghttp3_vec *vec, size_t veccnt,
                                   uint32_t *pflags, void *user_data,
                                   void *stream_user_data) {
  (void)conn; (void)stream_id; (void)user_data;

  xH3StreamData *sd = (xH3StreamData *)stream_user_data;
  if (!sd || !sd->body_data || sd->body_pos >= sd->body_len) {
    *pflags |= NGHTTP3_DATA_FLAG_EOF;
    return 0;
  }

  size_t remaining = sd->body_len - sd->body_pos;
  vec[0].base = (uint8_t *)(sd->body_data + sd->body_pos);
  vec[0].len  = remaining;
  sd->body_pos = sd->body_len;

  if (veccnt > 0) {
    *pflags |= NGHTTP3_DATA_FLAG_EOF;
  }
  return 1;
}

static int h3_send_response(struct xHttpStream_ *stream, int status,
                             struct xHttpHeader_ *headers,
                             const char *body, size_t body_len) {
  struct xHttpConn_ *conn = stream->conn;
  xHttpProtoH3      *h3   = (xHttpProtoH3 *)conn->proto.state;
  if (!h3 || !h3->h3_conn) return -1;

  int                  nheader = 1;
  struct xHttpHeader_ *h       = headers;
  while (h) { nheader++; h = h->next; }

  nghttp3_nv *nva = (nghttp3_nv *)calloc((size_t)nheader, sizeof(nghttp3_nv));
  if (!nva) return -1;

  char status_str[16];
  int  status_len = snprintf(status_str, sizeof(status_str), "%d", status);

  nva[0].name     = (uint8_t *)":status";
  nva[0].namelen  = 7;
  nva[0].value    = (uint8_t *)status_str;
  nva[0].valuelen = (size_t)status_len;
  nva[0].flags    = NGHTTP3_NV_FLAG_NO_COPY_NAME;

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

  /* Store body in per-stream data for h3_body_read to access */
  xH3StreamData *sd = (xH3StreamData *)nghttp3_conn_get_stream_user_data(
    h3->h3_conn, stream->stream_id);

  if (body && body_len > 0) {
    nghttp3_data_reader dr = {h3_body_read};
    /* Body data must outlive the flush — store in stream user data */
    if (sd) {
      sd->body_data = (const uint8_t *)body;
      sd->body_len  = body_len;
      sd->body_pos  = 0;
    }
    rv = nghttp3_conn_submit_response(
      h3->h3_conn, stream->stream_id, nva, (size_t)nheader, &dr);
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

  /* Flush now — the data_reader callback will fire during writev_stream */
  rv = h3_flush_responses(conn);

  /* Clear body data after flush */
  if (sd) { sd->body_data = NULL; sd->body_len = 0; sd->body_pos = 0; }
  return rv;
}

static int h3_write_data(struct xHttpStream_ *stream,
                          const char *data, size_t len) {
  struct xHttpConn_ *conn = stream->conn;
  xHttpProtoH3      *h3   = (xHttpProtoH3 *)conn->proto.state;

  if (!stream->writer.streaming) {
    /* First call: submit headers with a data reader */
    stream->writer.streaming = 1;

    int                  nheader = 1;
    struct xHttpHeader_ *h       = stream->writer.headers;
    while (h) { nheader++; h = h->next; }

    nghttp3_nv *nva = (nghttp3_nv *)calloc((size_t)nheader, sizeof(nghttp3_nv));
    if (!nva) return -1;

    char status_str[16];
    int  status_len = snprintf(status_str, sizeof(status_str), "%d", stream->writer.status_code);
    nva[0].name     = (uint8_t *)":status";
    nva[0].namelen  = 7;
    nva[0].value    = (uint8_t *)status_str;
    nva[0].valuelen = (size_t)status_len;
    nva[0].flags    = NGHTTP3_NV_FLAG_NO_COPY_NAME;

    int i = 1;
    h = stream->writer.headers;
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

    nghttp3_data_reader dr = {h3_body_read};
    int rv = nghttp3_conn_submit_response(
      h3->h3_conn, stream->stream_id, nva, (size_t)nheader, &dr);
    free(nva);
    if (rv != 0) return -1;
  }

  /* Store body chunk in per-stream data, then flush */
  xH3StreamData *sd = (xH3StreamData *)nghttp3_conn_get_stream_user_data(
    h3->h3_conn, stream->stream_id);
  if (!sd || !data) return -1;

  sd->body_data = (const uint8_t *)data;
  sd->body_len  = len;
  sd->body_pos  = 0;

  nghttp3_conn_resume_stream(h3->h3_conn, stream->stream_id);
  h3_flush_responses(conn);

  sd->body_data = NULL;
  sd->body_len  = 0;
  sd->body_pos  = 0;
  return 0;
}

static int h3_end_stream(struct xHttpStream_ *stream) {
  struct xHttpConn_ *conn = stream->conn;
  xHttpProtoH3      *h3   = (xHttpProtoH3 *)conn->proto.state;
  (void)h3;
  stream->writer.sent = 1;
  h3_flush_responses(conn);
  return 0;
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  Initialization
 * ═══════════════════════════════════════════════════════════════════════════
 */

int xHttpProtoH3Init(struct xHttpConn_ *conn) {
  xHttpProtoH3 *h3 = (xHttpProtoH3 *)calloc(1, sizeof(xHttpProtoH3));
  if (!h3) return -1;

  nghttp3_callbacks callbacks;
  memset(&callbacks, 0, sizeof(callbacks));
  callbacks.recv_header      = h3_recv_header;
  callbacks.recv_data        = h3_recv_data;
  callbacks.end_stream       = h3_end_stream_cb;
  callbacks.stream_close     = h3_stream_close_cb;

  nghttp3_settings settings;
  nghttp3_settings_default(&settings);
  settings.qpack_max_dtable_capacity = 4096;
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
