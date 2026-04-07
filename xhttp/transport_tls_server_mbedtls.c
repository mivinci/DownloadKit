/*
 * Copyright 2025 The xKit Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * transport_tls_server_mbedtls.c - mbedTLS transport implementation
 */

#ifdef XK_HAS_MBEDTLS

#include "server_private.h"
#include "transport_private.h"

/* mbedTLS 3.x+ provides build_info.h; mbedTLS 2.x uses version.h */
#if __has_include(<mbedtls/build_info.h>)
#include <mbedtls/build_info.h>
#else
#include <mbedtls/version.h>
#endif
#include <mbedtls/error.h>
#include <mbedtls/net_sockets.h>
#include <mbedtls/ssl.h>

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/uio.h>
#include <unistd.h>
#include <xbase/log.h>

/* ───────────────────── Per-connection TLS state ───────────────────── */

XDEF_STRUCT(xHttpTlsMbedTLS_) {
  mbedtls_ssl_context ssl;
  mbedtls_net_context net;
  int                 fd;
};

/* ───────────────────── Custom I/O callbacks for mbedTLS ─────────────────────
 */

static int mbedtls_net_send_cb(void *ctx, const unsigned char *buf,
                               size_t len) {
  xHttpTlsMbedTLS_ *t = (xHttpTlsMbedTLS_ *)ctx;
  ssize_t           n;
  do {
    n = write(t->fd, buf, len);
  } while (n < 0 && errno == EINTR);
  if (n < 0) {
    if (errno == EAGAIN || errno == EWOULDBLOCK)
      return MBEDTLS_ERR_SSL_WANT_WRITE;
    return MBEDTLS_ERR_NET_SEND_FAILED;
  }
  return (int)n;
}

static int mbedtls_net_recv_cb(void *ctx, unsigned char *buf, size_t len) {
  xHttpTlsMbedTLS_ *t = (xHttpTlsMbedTLS_ *)ctx;
  ssize_t           n;
  do {
    n = read(t->fd, buf, len);
  } while (n < 0 && errno == EINTR);
  if (n < 0) {
    if (errno == EAGAIN || errno == EWOULDBLOCK)
      return MBEDTLS_ERR_SSL_WANT_READ;
    return MBEDTLS_ERR_NET_RECV_FAILED;
  }
  if (n == 0) return 0; /* EOF */
  return (int)n;
}

/* ───────────────────── Transport vtable callbacks ───────────────────── */

static ssize_t mbedtls_transport_read(void *ctx, void *buf, size_t len) {
  xHttpTlsMbedTLS_ *t = (xHttpTlsMbedTLS_ *)ctx;
  int               n = mbedtls_ssl_read(&t->ssl, (unsigned char *)buf, len);
  if (n > 0) return (ssize_t)n;

  switch (n) {
  case MBEDTLS_ERR_SSL_WANT_READ:
  case MBEDTLS_ERR_SSL_WANT_WRITE:
    errno = EAGAIN;
    return -1;
  case MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY:
  case 0:
    return 0; /* Clean shutdown (EOF) */
  default:
    errno = EIO;
    return -1;
  }
}

static ssize_t mbedtls_transport_writev(void *ctx, const struct iovec *iov,
                                        int iovcnt) {
  xHttpTlsMbedTLS_ *t     = (xHttpTlsMbedTLS_ *)ctx;
  ssize_t           total = 0;

  for (int i = 0; i < iovcnt; i++) {
    if (iov[i].iov_len == 0) continue;

    int n = mbedtls_ssl_write(&t->ssl, (const unsigned char *)iov[i].iov_base,
                              iov[i].iov_len);
    if (n > 0) {
      total += n;
      if ((size_t)n < iov[i].iov_len) break; /* Partial write */
      continue;
    }

    if (n == MBEDTLS_ERR_SSL_WANT_READ || n == MBEDTLS_ERR_SSL_WANT_WRITE) {
      if (total > 0) break;
      errno = EAGAIN;
      return -1;
    }
    if (total > 0) break;
    errno = EIO;
    return -1;
  }

  return total;
}

static int mbedtls_transport_handshake(void *ctx) {
  xHttpTlsMbedTLS_ *t   = (xHttpTlsMbedTLS_ *)ctx;
  int               ret = mbedtls_ssl_handshake(&t->ssl);
  if (ret == 0) return xHttpTransportResult_Done;

  switch (ret) {
  case MBEDTLS_ERR_SSL_WANT_READ:
    return xHttpTransportResult_WantRead;
  case MBEDTLS_ERR_SSL_WANT_WRITE:
    return xHttpTransportResult_WantWrite;
  default:
    return xHttpTransportResult_Error;
  }
}

static const char *mbedtls_transport_alpn(void *ctx) {
  xHttpTlsMbedTLS_ *t    = (xHttpTlsMbedTLS_ *)ctx;
  const char       *alpn = mbedtls_ssl_get_alpn_protocol(&t->ssl);
  return alpn; /* NULL if no ALPN negotiated */
}

static void mbedtls_transport_destroy(void *ctx) {
  xHttpTlsMbedTLS_ *t = (xHttpTlsMbedTLS_ *)ctx;
  /* Only send close_notify if the handshake was completed.
   * Calling mbedtls_ssl_close_notify on an SSL context whose
   * handshake never finished can cause undefined behavior. */
#if MBEDTLS_VERSION_NUMBER >= 0x03000000
  if (mbedtls_ssl_is_handshake_over(&t->ssl)) {
    mbedtls_ssl_close_notify(&t->ssl);
  }
#else
  /* mbedTLS 2.x: check state directly (public field in 2.x) */
  if (t->ssl.state == MBEDTLS_SSL_HANDSHAKE_OVER) {
    mbedtls_ssl_close_notify(&t->ssl);
  }
#endif
  mbedtls_ssl_free(&t->ssl);
  free(t);
}

/* ───────────────────── Public API ───────────────────── */

void xHttpTlsTransportInitMbedTLS(xHttpTransport *transport, xTlsCtx tls_ctx,
                                  int fd) {
  if (!transport || !tls_ctx) return;

  mbedtls_ssl_config *server_conf = (mbedtls_ssl_config *)xTlsCtxGetNative_(tls_ctx);
  if (!server_conf) return;

  xHttpTlsMbedTLS_ *t = (xHttpTlsMbedTLS_ *)calloc(1, sizeof(xHttpTlsMbedTLS_));
  if (!t) {
    transport->read      = NULL;
    transport->writev    = NULL;
    transport->handshake = NULL;
    transport->alpn      = NULL;
    transport->destroy   = NULL;
    transport->ctx       = NULL;
    return;
  }

  mbedtls_ssl_init(&t->ssl);
  t->fd = fd;

  int ret = mbedtls_ssl_setup(&t->ssl, server_conf);
  if (ret != 0) {
    xLog(false, "xhttp: mbedtls_ssl_setup failed: -0x%04x", -ret);
    mbedtls_ssl_free(&t->ssl);
    free(t);
    transport->read      = NULL;
    transport->writev    = NULL;
    transport->handshake = NULL;
    transport->alpn      = NULL;
    transport->destroy   = NULL;
    transport->ctx       = NULL;
    return;
  }

  /* Set custom I/O callbacks using the raw fd */
  mbedtls_ssl_set_bio(&t->ssl, t, mbedtls_net_send_cb, mbedtls_net_recv_cb,
                      NULL);

  transport->read      = mbedtls_transport_read;
  transport->writev    = mbedtls_transport_writev;
  transport->handshake = mbedtls_transport_handshake;
  transport->alpn      = mbedtls_transport_alpn;
  transport->destroy   = mbedtls_transport_destroy;
  transport->ctx       = t;
}

#endif /* XK_HAS_MBEDTLS */
