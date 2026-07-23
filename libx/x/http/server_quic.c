/*
 * Copyright 2025 The libx Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * server_quic.c - HTTP/3 server via ngtcp2 + nghttp3
 *
 * Manages QUIC connections (UDP listener, CID routing, handshake,
 * packet I/O, timers) and delegates HTTP/3 stream handling to
 * proto_h3.c via the shared xHttpProto vtable.
 */

#include "proto_h3.h"
#include "server_private.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <ngtcp2/ngtcp2.h>
#include <ngtcp2/ngtcp2_crypto.h>
#include <ngtcp2/ngtcp2_crypto_ossl.h>
#include <nghttp3/nghttp3.h>
#include <openssl/err.h>
#include <openssl/rand.h>
#include <openssl/ssl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
#include <sys/socket.h>
#include <time.h>
#include <x/base/log.h>
#include <x/base/map.h>

/* For xTlsCtxGetNative (SSL_CTX*) */
#include <x/net/tls_private.h>

/* ═══════════════════════════════════════════════════════════════════════════
 *  Configuration
 * ═══════════════════════════════════════════════════════════════════════════
 */

#define H3_QUIC_IDLE_TIMEOUT_MS 30000
#define H3_QUIC_MAX_IDLE_TIMEOUT_MS 60000
#define H3_UDP_RECV_BUF_SIZE 65536
#define H3_MAX_PKT_SIZE 1350

/* ═══════════════════════════════════════════════════════════════════════════
 *  Per-connection QUIC state
 * ═══════════════════════════════════════════════════════════════════════════
 */

typedef struct {
  ngtcp2_crypto_ossl_ctx *crypto_ctx;
  SSL                    *ssl;
} h3_quic_conn_state_t;

/* ═══════════════════════════════════════════════════════════════════════════
 *  Forward declarations
 * ═══════════════════════════════════════════════════════════════════════════
 */

static void h3_on_listen_event(xSocket sock, xEventMask mask, void *arg);
static void h3_on_quic_timer(void *arg);

/* ═══════════════════════════════════════════════════════════════════════════
 *  Helpers
 * ═══════════════════════════════════════════════════════════════════════════
 */

static ngtcp2_tstamp h3_timestamp(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (ngtcp2_tstamp)ts.tv_sec * 1000000000ULL + (ngtcp2_tstamp)ts.tv_nsec;
}

static void h3_rand_cb(uint8_t *dest, size_t destlen,
                        const ngtcp2_rand_ctx *rand_ctx) {
  (void)rand_ctx;
  RAND_bytes(dest, (int)destlen);
}

static int h3_get_new_connection_id_cb(ngtcp2_conn *conn, ngtcp2_cid *cid,
                                        uint8_t *token, size_t cidlen,
                                        void *user_data) {
  (void)conn;
  (void)user_data;
  RAND_bytes(cid->data, (int)cidlen);
  cid->datalen = cidlen;
  RAND_bytes(token, NGTCP2_STATELESS_RESET_TOKENLEN);
  return 0;
}

/* CID hex helper for xMap key */
static void h3_cid_to_hex(const ngtcp2_cid *cid, char *out, size_t out_len) {
  static const char hex[] = "0123456789abcdef";
  size_t i;
  for (i = 0; i < cid->datalen && i * 2 + 1 < out_len; i++) {
    out[i * 2]     = hex[(cid->data[i] >> 4) & 0xf];
    out[i * 2 + 1] = hex[cid->data[i] & 0xf];
  }
  out[i * 2] = '\0';
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  ngtcp2 callbacks — I/O
 * ═══════════════════════════════════════════════════════════════════════════
 */

static int h3_recv_stream_data_cb(ngtcp2_conn *quic_conn,
                                   uint32_t flags, int64_t stream_id,
                                   uint64_t offset, const uint8_t *data,
                                   size_t datalen, void *user_data,
                                   void *stream_user_data) {
  struct xHttpConn_ *conn = (struct xHttpConn_ *)user_data;
  (void)quic_conn;
  (void)flags;
  (void)offset;
  (void)stream_user_data;

  /* Feed stream data to the HTTP/3 protocol handler with stream routing */
  return conn->proto.on_data(conn, (const char *)data, datalen, stream_id);
}

static int h3_acked_stream_data_cb(ngtcp2_conn *quic_conn,
                                    int64_t stream_id,
                                    uint64_t datalen, uint64_t app_datalen,
                                    void *user_data,
                                    void *stream_user_data) {
  (void)quic_conn;
  (void)stream_id;
  (void)datalen;
  (void)app_datalen;
  (void)user_data;
  (void)stream_user_data;
  /* nghttp3 handles stream-level flow control internally */
  return 0;
}

static int h3_handshake_completed_cb(ngtcp2_conn *quic_conn, void *user_data) {
  struct xHttpConn_ *conn = (struct xHttpConn_ *)user_data;
  (void)quic_conn;
  xLog(false, "xhttp h3: handshake completed (conn=%p)", (void *)conn);
  return 0;
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  ngtcp2 callbacks — (delegated to ngtcp2_crypto)
 * ═══════════════════════════════════════════════════════════════════════════
 */

static int h3_recv_crypto_data_cb(ngtcp2_conn *conn,
                                   ngtcp2_encryption_level encryption_level,
                                   uint64_t offset, const uint8_t *data,
                                   size_t datalen, void *user_data) {
  return ngtcp2_crypto_recv_crypto_data_cb(conn, encryption_level, offset,
                                           data, datalen, user_data);
}

static int h3_recv_client_initial_cb(ngtcp2_conn *conn,
                                      const ngtcp2_cid *dcid,
                                      void *user_data) {
  return ngtcp2_crypto_recv_client_initial_cb(conn, dcid, user_data);
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  QUIC connection lifecycle
 * ═══════════════════════════════════════════════════════════════════════════
 */

static int h3_quic_conn_init(struct xHttpConn_ *conn, const ngtcp2_cid *dcid,
                              const ngtcp2_cid *scid,
                              const struct sockaddr *remote_addr,
                              socklen_t remote_addrlen,
                              struct xHttpServer_ *s) {
  ngtcp2_path path;
  ngtcp2_settings settings;
  ngtcp2_transport_params params;
  ngtcp2_callbacks callbacks;
  int rv;

  /* Store remote address */
  memcpy(&conn->remote_addr, remote_addr, remote_addrlen);

  /* Store CIDs */
  memcpy(conn->conn_id, scid->data, scid->datalen);
  conn->conn_id_len = scid->datalen;
  memcpy(conn->remote_cid, dcid->data, dcid->datalen);
  conn->remote_cid_len = dcid->datalen;
  conn->is_quic = 1;

  /* Create OpenSSL context for this connection */
  h3_quic_conn_state_t *qs = calloc(1, sizeof(h3_quic_conn_state_t));
  if (!qs) return -1;

  qs->ssl = SSL_new((SSL_CTX *)xTlsCtxGetNative(s->h3_tls_ctx));
  if (!qs->ssl) {
    free(qs);
    return -1;
  }
  SSL_set_app_data(qs->ssl, conn);

  rv = ngtcp2_crypto_ossl_ctx_new(&qs->crypto_ctx, qs->ssl);
  if (rv != 0) {
    SSL_free(qs->ssl);
    free(qs);
    return -1;
  }

  /* Store state separately from quic_conn */
  conn->quic_state = qs;

  /* Configure server-side TLS session */
  ngtcp2_crypto_ossl_configure_server_session(qs->ssl);

  /* Build path */
  memset(&path, 0, sizeof(path));
  path.local.addrlen  = sizeof(struct sockaddr_in);
  path.remote.addrlen = remote_addrlen;
  memcpy(path.remote.addr, remote_addr, remote_addrlen);

  /* Set up ngtcp2 callbacks */
  memset(&callbacks, 0, sizeof(callbacks));
  callbacks.recv_client_initial     = h3_recv_client_initial_cb;
  callbacks.recv_crypto_data        = h3_recv_crypto_data_cb;
  callbacks.encrypt                 = ngtcp2_crypto_encrypt_cb;
  callbacks.decrypt                 = ngtcp2_crypto_decrypt_cb;
  callbacks.hp_mask                 = ngtcp2_crypto_hp_mask_cb;
  callbacks.handshake_completed     = h3_handshake_completed_cb;
  callbacks.recv_stream_data        = h3_recv_stream_data_cb;
  callbacks.acked_stream_data_offset= h3_acked_stream_data_cb;
  callbacks.rand                    = h3_rand_cb;
  callbacks.get_new_connection_id   = h3_get_new_connection_id_cb;
  callbacks.update_key              = ngtcp2_crypto_update_key_cb;

  /* Settings */
  ngtcp2_settings_default_versioned(
    NGTCP2_SETTINGS_VERSION, &settings);
  settings.initial_ts  = h3_timestamp();
  settings.max_tx_udp_payload_size = H3_MAX_PKT_SIZE;

  /* Transport params */
  ngtcp2_transport_params_default_versioned(
    NGTCP2_TRANSPORT_PARAMS_VERSION, &params);
  params.initial_max_streams_uni  = 3;
  params.initial_max_streams_bidi = 100;
  params.initial_max_stream_data_bidi_local  = 1048576;
  params.initial_max_stream_data_bidi_remote = 1048576;
  params.initial_max_data                    = 4194304;
  params.max_idle_timeout                    = H3_QUIC_MAX_IDLE_TIMEOUT_MS;

  /* Create ngtcp2 server connection */
  ngtcp2_cid scid_copy = *scid;
  ngtcp2_cid dcid_copy = *dcid;
  uint32_t version     = NGTCP2_PROTO_VER_V1;

  ngtcp2_conn *quic_conn = NULL;
  rv = ngtcp2_conn_server_new_versioned(
    &quic_conn, &dcid_copy, &scid_copy, &path, version,
    NGTCP2_CALLBACKS_VERSION, &callbacks,
    NGTCP2_SETTINGS_VERSION, &settings,
    NGTCP2_TRANSPORT_PARAMS_VERSION, &params,
    NULL, conn);

  if (rv != 0) {
    xLog(false, "xhttp h3: ngtcp2_conn_server_new failed: %s",
         ngtcp2_strerror(rv));
    ngtcp2_crypto_ossl_ctx_del(qs->crypto_ctx);
    SSL_free(qs->ssl);
    free(qs);
    return -1;
  }

  qs->crypto_ctx = qs->crypto_ctx; /* keep */

  /* Store the QUIC conn pointer — opaque to server_private.h */
  conn->quic_conn = quic_conn;

  return 0;
}

void xHttpQuicConnDestroy(struct xHttpConn_ *conn) {
  if (!conn || !conn->is_quic) return;

  xHttpQuicConnCancelTimer(conn);

  if (conn->quic_conn) {
    ngtcp2_conn_del((ngtcp2_conn *)conn->quic_conn);
    conn->quic_conn = NULL;
  }

  if (conn->quic_state) {
    h3_quic_conn_state_t *qs = (h3_quic_conn_state_t *)conn->quic_state;
    if (qs->crypto_ctx) {
      ngtcp2_crypto_ossl_ctx_del(qs->crypto_ctx);
      qs->crypto_ctx = NULL;
    }
    if (qs->ssl) {
      SSL_free(qs->ssl);
      qs->ssl = NULL;
    }
    free(qs);
    conn->quic_state = NULL;
  }
}

void xHttpQuicConnScheduleTimer(struct xHttpConn_ *conn) {
  struct xHttpServer_ *s = conn->server;
  ngtcp2_conn *quic_conn = (ngtcp2_conn *)conn->quic_conn;
  if (!quic_conn) return;

  ngtcp2_tstamp expiry = ngtcp2_conn_get_expiry(quic_conn);
  if (expiry == UINT64_MAX) return;

  ngtcp2_tstamp now = h3_timestamp();
  int64_t delay_ms   = expiry > now ? (int64_t)((expiry - now) / 1000000) : 0;

  conn->quic_timer = xEventLoopTimerAfter(
    s->loop, h3_on_quic_timer, conn, delay_ms);
}

void xHttpQuicConnCancelTimer(struct xHttpConn_ *conn) {
  if (conn->quic_timer) {
    xEventLoopTimerCancel(conn->server->loop, conn->quic_timer);
    conn->quic_timer = NULL;
  }
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  QUIC timer callback
 * ═══════════════════════════════════════════════════════════════════════════
 */

static void h3_on_quic_timer(void *arg) {
  struct xHttpConn_ *conn = (struct xHttpConn_ *)arg;

  ngtcp2_conn *quic_conn = (ngtcp2_conn *)conn->quic_conn;
  if (!quic_conn) return;

  conn->quic_timer = NULL;

  ngtcp2_tstamp now = h3_timestamp();
  int rv = ngtcp2_conn_handle_expiry(quic_conn, now);
  if (rv != 0) {
    xLog(false, "xhttp h3: handle_expiry error: %s", ngtcp2_strerror(rv));
    xHttpConnClose(conn);
    return;
  }

  xHttpQuicConnScheduleTimer(conn);
}

int xHttpQuicConnSend(struct xHttpConn_ *conn) {
  struct xHttpServer_ *s    = conn->server;
  ngtcp2_conn         *quic = (ngtcp2_conn *)conn->quic_conn;
  if (!quic) return 0;

  uint8_t buf[H3_UDP_RECV_BUF_SIZE];
  ngtcp2_path path;
  ngtcp2_pkt_info pkt_info;
  ngtcp2_ssize nwrite;

  for (;;) {
    memset(&path, 0, sizeof(path));
    path.local.addrlen  = sizeof(struct sockaddr_in);
    path.remote.addrlen = conn->remote_addr.ss_len;
    memcpy(path.remote.addr, &conn->remote_addr,
           conn->remote_addr.ss_len);

    nwrite = ngtcp2_conn_write_pkt_versioned(
      quic, &path, NGTCP2_PKT_INFO_VERSION, &pkt_info,
      buf, sizeof(buf), h3_timestamp());

    if (nwrite < 0) {
      if (nwrite == NGTCP2_ERR_WRITE_MORE) continue;
      xLog(false, "xhttp h3: write_pkt error: %s",
           ngtcp2_strerror((int)nwrite));
      return -1;
    }

    /* Send on shared UDP fd */
    struct iovec iov;
    iov.iov_base = buf;
    iov.iov_len  = (size_t)nwrite;

    struct msghdr msg;
    memset(&msg, 0, sizeof(msg));
    msg.msg_name    = (void *)&conn->remote_addr;
    msg.msg_namelen = conn->remote_addr.ss_len;
    msg.msg_iov     = &iov;
    msg.msg_iovlen  = 1;

    ssize_t sent = sendmsg(s->h3_listen_fd, &msg, 0);
    if (sent < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
      xLog(false, "xhttp h3: sendmsg error: %s", strerror(errno));
      return -1;
    }
  }

  /* Re-schedule timer after sending */
  xHttpQuicConnScheduleTimer(conn);
  return 0;
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  UDP listener — packet dispatch
 * ═══════════════════════════════════════════════════════════════════════════
 */

static void h3_on_listen_event(xSocket sock, xEventMask mask, void *arg) {
  struct xHttpServer_ *s = (struct xHttpServer_ *)arg;
  (void)sock;

  if (!(mask & xEvent_Read)) return;

  uint8_t buf[H3_UDP_RECV_BUF_SIZE];
  struct sockaddr_storage remote_addr;
  socklen_t remote_addrlen = sizeof(remote_addr);
  ngtcp2_pkt_info pkt_info;
  ngtcp2_path path;

  for (;;) {
    ssize_t nread = recvfrom(s->h3_listen_fd, buf, sizeof(buf), 0,
                             (struct sockaddr *)&remote_addr,
                             &remote_addrlen);
    if (nread < 0) {
      if (errno == EAGAIN || errno == EWOULDBLOCK) break;
      xLog(false, "xhttp h3: recvfrom error: %s", strerror(errno));
      break;
    }

    memset(&path, 0, sizeof(path));
    path.local.addrlen  = sizeof(struct sockaddr_in);
    path.remote.addrlen = remote_addrlen;
    memcpy(path.remote.addr, &remote_addr, remote_addrlen);

    memset(&pkt_info, 0, sizeof(pkt_info));

    /* Try to parse the QUIC short header to extract DCID */
    ngtcp2_version_cid vc;
    int rv = ngtcp2_pkt_decode_version_cid(
      &vc, buf, (size_t)nread, NGTCP2_MAX_CIDLEN);
    if (rv < 0) {
      /* Packet too short or malformed — skip */
      continue;
    }

    /* Look up connection by DCID */
    char cid_hex[41]; /* 20 bytes → 40 hex + NUL */
    ngtcp2_cid dcid = { .datalen = vc.dcidlen };
    memcpy(dcid.data, vc.dcid, vc.dcidlen);
    h3_cid_to_hex(&dcid, cid_hex, sizeof(cid_hex));

    struct xHttpConn_ *conn = (struct xHttpConn_ *)xMapGet(
      s->h3_quic_conns, cid_hex);

    if (!conn) {
      /* New connection — must be a long-header Initial packet */
      if (!(buf[0] & 0x80)) {
        /* Short header but no matching CID — maybe stateless reset */
        continue;
      }

      /* Allocate new connection */
      conn = (struct xHttpConn_ *)calloc(1, sizeof(struct xHttpConn_));
      if (!conn) continue;
      conn->server = s;

      /* Generate random server-side CID */
      ngtcp2_cid scid;
      RAND_bytes(scid.data, NGTCP2_MAX_CIDLEN);
      scid.datalen = NGTCP2_MAX_CIDLEN;

      /* Initialize QUIC connection state */
      rv = h3_quic_conn_init(conn, &dcid, &scid,
                              (struct sockaddr *)&remote_addr,
                              remote_addrlen, s);
      if (rv != 0) {
        free(conn);
        continue;
      }

      /* Register in CID lookup table */
      char scid_hex[41];
      h3_cid_to_hex(&scid, scid_hex, sizeof(scid_hex));
      xMapSet(s->h3_quic_conns, scid_hex, conn);

      /* Add to connection list */
      conn->next = s->conns;
      if (s->conns) s->conns->prev = conn;
      s->conns = conn;

      /* Initialize proto_h3 */
      xHttpProtoH3Init(conn);
    }

    /* Feed packet to ngtcp2 */
    ngtcp2_conn *quic = (ngtcp2_conn *)conn->quic_conn;
    rv = ngtcp2_conn_read_pkt_versioned(
      quic, &path, NGTCP2_PKT_INFO_VERSION, &pkt_info,
      buf, (size_t)nread, h3_timestamp());

    if (rv < 0) {
      xLog(false, "xhttp h3: read_pkt error: %s (conn=%p)",
           ngtcp2_strerror(rv), (void *)conn);
      xHttpConnClose(conn);
      continue;
    }

    /* Schedule timer */
    xHttpQuicConnScheduleTimer(conn);
  }
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  Public API: xHttpServerListenH3
 * ═══════════════════════════════════════════════════════════════════════════
 */

xErrno xHttpServerListenH3(xHttpServer server, const char *host,
                            uint16_t port, const xTlsConf *config) {
  struct xHttpServer_ *s = (struct xHttpServer_ *)server;
  if (!s || !config) return xErrno_InvalidArg;

#ifndef X_HAS_NGHTTP3
  (void)host;
  (void)port;
  return xErrno_NotSupported;
#else
  if (s->h3_listen_fd >= 0) {
    /* Already listening */
    return xErrno_AlreadyExists;
  }

  /* Build address */
  struct sockaddr_in addr;
  memset(&addr, 0, sizeof(addr));
  addr.sin_family      = AF_INET;
  addr.sin_port        = htons(port);
  addr.sin_addr.s_addr = host ? inet_addr(host) : INADDR_ANY;

  /* Create UDP socket */
  int fd = socket(AF_INET, SOCK_DGRAM, 0);
  if (fd < 0) {
    xLog(false, "xhttp h3: socket() failed: %s", strerror(errno));
    return xErrno_InvalidArg;
  }

  int on = 1;
  setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));

  if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
    xLog(false, "xhttp h3: bind() failed: %s", strerror(errno));
    close(fd);
    return xErrno_InvalidArg;
  }

  /* Wrap in xSocket for event loop integration */
  s->h3_listen_sock = xSocketCreateFromFd(
    s->loop, fd, xEvent_Read, h3_on_listen_event, server);
  if (!s->h3_listen_sock) {
    close(fd);
    return xErrno_NoMemory;
  }

  s->h3_listen_fd = fd;
  s->h3_port      = port;

  /* Create TLS context for QUIC */
  s->h3_tls_ctx = xTlsCtxCreate(config);
  if (!s->h3_tls_ctx) {
    xSocketDestroy(s->loop, s->h3_listen_sock);
    s->h3_listen_sock = NULL;
    s->h3_listen_fd   = -1;
    close(fd);
    return xErrno_NoMemory;
  }

  /* Create CID → connection lookup map */
  s->h3_quic_conns = xMapCreate(xMapType_Hash, 32, xMapStrHash, xMapStrEq);
  if (!s->h3_quic_conns) {
    xTlsCtxDestroy(s->h3_tls_ctx);
    s->h3_tls_ctx = NULL;
    xSocketDestroy(s->loop, s->h3_listen_sock);
    s->h3_listen_sock = NULL;
    s->h3_listen_fd   = -1;
    close(fd);
    return xErrno_NoMemory;
  }

  s->h3_enabled = 1;

  /* Build Alt-Svc header value for H1TLS/H2 responses (RFC 9114 section 3.1.1) */
  snprintf(s->alt_svc, sizeof(s->alt_svc), "h3=\":%u\"; ma=3600", port);

  /* Initialize ngtcp2_crypto_ossl */
  ngtcp2_crypto_ossl_init();

  xLog(false, "xhttp: H3 listening on %s:%u (UDP)", host ? host : "0.0.0.0", port);
  return xErrno_Ok;
#endif
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  Cleanup
 * ═══════════════════════════════════════════════════════════════════════════
 */

void xHttpServerQuicCleanup(struct xHttpServer_ *s) {
  if (!s) return;

  if (s->h3_quic_conns) {
    xMapDestroy(s->h3_quic_conns);
    s->h3_quic_conns = NULL;
  }

  if (s->h3_listen_fd >= 0) {
    if (s->h3_listen_sock) {
      xSocketDestroy(s->loop, s->h3_listen_sock);
      s->h3_listen_sock = NULL;
    }
    s->h3_listen_fd = -1;
  }

  if (s->h3_tls_ctx) {
    xTlsCtxDestroy(s->h3_tls_ctx);
    s->h3_tls_ctx = NULL;
  }
}
