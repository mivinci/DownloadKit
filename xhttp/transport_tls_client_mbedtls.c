/*
 * Copyright 2025 The xKit Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * transport_tls_client_mbedtls.c - mbedTLS TLS client transport
 *
 * Per-connection SSL object using a shared mbedtls_ssl_config from xTlsCtx.
 */

#ifdef XK_HAS_MBEDTLS

#include "transport_tls_client.h"

#include <xnet/tls.h>

/* Internal: get native mbedtls_ssl_config* from opaque xTlsCtx handle */
extern void *xTlsCtxGetNative(xTlsCtx ctx);

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

/* ─────────── Per-connection TLS client state ─────────── */

XDEF_STRUCT(xTlsClientMbed_) {
  mbedtls_ssl_context ssl;
  int                 fd;
  char                alpn_result[16];
};

/* ─────────── Custom I/O callbacks ─────────── */

static int mbed_client_send(void *ctx,
                            const unsigned char *buf,
                            size_t len) {
  xTlsClientMbed_ *t = (xTlsClientMbed_ *)ctx;
  ssize_t n;
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

static int mbed_client_recv(void *ctx,
                            unsigned char *buf,
                            size_t len) {
  xTlsClientMbed_ *t = (xTlsClientMbed_ *)ctx;
  ssize_t n;
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

/* ─────────── Transport vtable callbacks ─────────── */

static ssize_t tls_client_mbed_read(void *ctx,
                                    void *buf,
                                    size_t len) {
  xTlsClientMbed_ *t = (xTlsClientMbed_ *)ctx;
  int n = mbedtls_ssl_read(&t->ssl,
                           (unsigned char *)buf, len);
  if (n > 0) return (ssize_t)n;

  switch (n) {
  case MBEDTLS_ERR_SSL_WANT_READ:
  case MBEDTLS_ERR_SSL_WANT_WRITE:
    errno = EAGAIN;
    return -1;
  case MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY:
  case 0:
    return 0;
  default:
    errno = EIO;
    return -1;
  }
}

static ssize_t tls_client_mbed_writev(void *ctx,
                                      const struct iovec *iov,
                                      int iovcnt) {
  xTlsClientMbed_ *t = (xTlsClientMbed_ *)ctx;
  ssize_t total = 0;

  for (int i = 0; i < iovcnt; i++) {
    if (iov[i].iov_len == 0) continue;

    int n = mbedtls_ssl_write(
      &t->ssl,
      (const unsigned char *)iov[i].iov_base,
      iov[i].iov_len);
    if (n > 0) {
      total += n;
      if ((size_t)n < iov[i].iov_len) break;
      continue;
    }

    if (n == MBEDTLS_ERR_SSL_WANT_READ ||
        n == MBEDTLS_ERR_SSL_WANT_WRITE) {
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

static int tls_client_mbed_handshake(void *ctx) {
  xTlsClientMbed_ *t = (xTlsClientMbed_ *)ctx;
  int ret = mbedtls_ssl_handshake(&t->ssl);
  if (ret == 0) {
    /* Cache ALPN result */
    const char *alpn =
      mbedtls_ssl_get_alpn_protocol(&t->ssl);
    if (alpn) {
      size_t alen = strlen(alpn);
      if (alen < sizeof(t->alpn_result)) {
        memcpy(t->alpn_result, alpn, alen + 1);
      } else {
        t->alpn_result[0] = '\0';
      }
    } else {
      t->alpn_result[0] = '\0';
    }
    return xHttpTransportResult_Done;
  }

  switch (ret) {
  case MBEDTLS_ERR_SSL_WANT_READ:
    return xHttpTransportResult_WantRead;
  case MBEDTLS_ERR_SSL_WANT_WRITE:
    return xHttpTransportResult_WantWrite;
  default:
    return xHttpTransportResult_Error;
  }
}

static const char *tls_client_mbed_alpn(void *ctx) {
  xTlsClientMbed_ *t = (xTlsClientMbed_ *)ctx;
  if (t->alpn_result[0] == '\0') return NULL;
  return t->alpn_result;
}

static void tls_client_mbed_destroy(void *ctx) {
  xTlsClientMbed_ *t = (xTlsClientMbed_ *)ctx;
#if MBEDTLS_VERSION_NUMBER >= 0x03000000
  if (mbedtls_ssl_is_handshake_over(&t->ssl)) {
    mbedtls_ssl_close_notify(&t->ssl);
  }
#else
  if (t->ssl.state == MBEDTLS_SSL_HANDSHAKE_OVER) {
    mbedtls_ssl_close_notify(&t->ssl);
  }
#endif
  mbedtls_ssl_free(&t->ssl);
  /* ssl_config is owned by the shared xTlsCtx — do NOT free it */
  free(t);
}

/* ─────────── Public API ─────────── */

int xHttpTlsClientTransportInit(xHttpTransport *transport,
                                xTlsCtx tls_ctx,
                                const char *hostname,
                                int fd) {
  if (!transport || !tls_ctx) return -1;

  mbedtls_ssl_config *client_conf =
    (mbedtls_ssl_config *)xTlsCtxGetNative(tls_ctx);
  if (!client_conf) return -1;

  xTlsClientMbed_ *t =
    (xTlsClientMbed_ *)calloc(1, sizeof(xTlsClientMbed_));
  if (!t) return -1;

  t->fd             = fd;
  t->alpn_result[0] = '\0';

  mbedtls_ssl_init(&t->ssl);

  int ret = mbedtls_ssl_setup(&t->ssl, client_conf);
  if (ret != 0) {
    xLog(false,
         "xhttp: mbedtls_ssl_setup failed: -0x%04x",
         -ret);
    goto fail;
  }

  /* Set hostname for SNI and verification */
  if (hostname) {
    ret = mbedtls_ssl_set_hostname(&t->ssl, hostname);
    if (ret != 0) {
      xLog(false,
           "xhttp: mbedtls_ssl_set_hostname failed: "
           "-0x%04x", -ret);
      goto fail;
    }
  }

  /* Set custom I/O callbacks */
  mbedtls_ssl_set_bio(&t->ssl, t,
                      mbed_client_send,
                      mbed_client_recv, NULL);

  /* Fill transport vtable */
  transport->read      = tls_client_mbed_read;
  transport->writev    = tls_client_mbed_writev;
  transport->handshake = tls_client_mbed_handshake;
  transport->alpn      = tls_client_mbed_alpn;
  transport->destroy   = tls_client_mbed_destroy;
  transport->ctx       = t;

  return 0;

fail:
  mbedtls_ssl_free(&t->ssl);
  free(t);
  return -1;
}

#endif /* XK_HAS_MBEDTLS */
