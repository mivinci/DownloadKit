/*
 * Copyright 2025 The libx Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * transport_mbedtls.c - mbedTLS per-connection TLS transport
 *
 * Provides both server-side and client-side TLS transport using mbedTLS.
 * TLS context management (xTlsCtxCreate etc.) lives in tls_mbedtls.c.
 */

#ifdef X_HAS_MBEDTLS

#include "tls_private.h"
#include "transport.h"

/* mbedTLS 3.x+ provides build_info.h; mbedTLS 2.x uses version.h */
#if __has_include(<mbedtls/build_info.h>)
#include <mbedtls/build_info.h>
#else
#include <mbedtls/version.h>
#endif
#include <mbedtls/error.h>
#include <mbedtls/net_sockets.h>
#include <mbedtls/pk.h>
#include <mbedtls/ssl.h>
#include <mbedtls/x509_crt.h>

#if MBEDTLS_VERSION_NUMBER < 0x04000000
/* mbedTLS 2.x/3.x: manual RNG management required */
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/entropy.h>
#endif

/* mbedTLS version compatibility */
#if MBEDTLS_VERSION_NUMBER >= 0x03000000
/* mbedTLS 3.x/4.x */
#define X_MBEDTLS_SET_MIN_TLS12(conf) \
  mbedtls_ssl_conf_min_tls_version((conf), MBEDTLS_SSL_VERSION_TLS1_2)
#else
/* mbedTLS 2.x */
#define X_MBEDTLS_SET_MIN_TLS12(conf)                              \
  mbedtls_ssl_conf_min_version((conf), MBEDTLS_SSL_MAJOR_VERSION_3, \
                               MBEDTLS_SSL_MINOR_VERSION_3)
#endif

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/uio.h>
#include <unistd.h>
#include <x/base/log.h>

/* ═══════════════════════════════════════════════════════════════════
 *  Custom I/O callbacks for mbedTLS
 * ═══════════════════════════════════════════════════════════════════
 */

XDEF_STRUCT(xTlsMbedTLS_) {
  mbedtls_ssl_context ssl;
  int                 fd;
  /* Client-only fields (NULL/zero for server) */
  mbedtls_ssl_config *owned_conf;
  mbedtls_x509_crt   *owned_ca;
  mbedtls_x509_crt   *owned_client_cert;
  mbedtls_pk_context *owned_client_key;
#if MBEDTLS_VERSION_NUMBER < 0x04000000
  mbedtls_entropy_context  *owned_entropy;
  mbedtls_ctr_drbg_context *owned_ctr_drbg;
#endif
};

static int mbed_send_cb(void *ctx, const unsigned char *buf, size_t len) {
  xTlsMbedTLS_ *t = (xTlsMbedTLS_ *)ctx;
  ssize_t       n;
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

static int mbed_recv_cb(void *ctx, unsigned char *buf, size_t len) {
  xTlsMbedTLS_ *t = (xTlsMbedTLS_ *)ctx;
  ssize_t       n;
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

/* ═══════════════════════════════════════════════════════════════════
 *  Transport vtable callbacks (shared by server and client)
 * ═══════════════════════════════════════════════════════════════════
 */

static ssize_t mbed_read(void *ctx, void *buf, size_t len) {
  xTlsMbedTLS_ *t = (xTlsMbedTLS_ *)ctx;
  int           n = mbedtls_ssl_read(&t->ssl, (unsigned char *)buf, len);
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

static ssize_t mbed_writev(void *ctx, const struct iovec *iov, int iovcnt) {
  xTlsMbedTLS_ *t     = (xTlsMbedTLS_ *)ctx;
  ssize_t       total = 0;

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

static int mbed_handshake(void *ctx) {
  xTlsMbedTLS_ *t   = (xTlsMbedTLS_ *)ctx;
  int           ret = mbedtls_ssl_handshake(&t->ssl);
  if (ret == 0) return xTransportResult_Done;

  switch (ret) {
  case MBEDTLS_ERR_SSL_WANT_READ:
    return xTransportResult_WantRead;
  case MBEDTLS_ERR_SSL_WANT_WRITE:
    return xTransportResult_WantWrite;
  default:
    return xTransportResult_Error;
  }
}

static const char *mbed_alpn(void *ctx) {
  xTlsMbedTLS_ *t    = (xTlsMbedTLS_ *)ctx;
  const char   *alpn = mbedtls_ssl_get_alpn_protocol(&t->ssl);
  return alpn; /* NULL if no ALPN negotiated */
}

static void mbed_destroy(void *ctx) {
  xTlsMbedTLS_ *t = (xTlsMbedTLS_ *)ctx;
  /* Only send close_notify if the handshake was completed */
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

  /* Free client-owned resources (NULL for server transport) */
  if (t->owned_client_cert) {
    mbedtls_x509_crt_free(t->owned_client_cert);
    free(t->owned_client_cert);
  }
  if (t->owned_client_key) {
    mbedtls_pk_free(t->owned_client_key);
    free(t->owned_client_key);
  }
  if (t->owned_ca) {
    mbedtls_x509_crt_free(t->owned_ca);
    free(t->owned_ca);
  }
#if MBEDTLS_VERSION_NUMBER < 0x04000000
  if (t->owned_ctr_drbg) {
    mbedtls_ctr_drbg_free(t->owned_ctr_drbg);
    free(t->owned_ctr_drbg);
  }
  if (t->owned_entropy) {
    mbedtls_entropy_free(t->owned_entropy);
    free(t->owned_entropy);
  }
#endif
  if (t->owned_conf) {
    mbedtls_ssl_config_free(t->owned_conf);
    free(t->owned_conf);
  }
  free(t);
}

/* ═══════════════════════════════════════════════════════════════════
 *  Server transport init
 * ═══════════════════════════════════════════════════════════════════
 */

void xTransportTlsServerInit(xTransport *transport, xTlsCtx tls_ctx, int fd) {
  if (!transport || !tls_ctx) return;

  mbedtls_ssl_config *server_conf = (mbedtls_ssl_config *)xTlsCtxGetNative(tls_ctx);
  if (!server_conf) return;

  xTlsMbedTLS_ *t = (xTlsMbedTLS_ *)calloc(1, sizeof(xTlsMbedTLS_));
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
    xLog(false, "xnet: mbedtls_ssl_setup failed: -0x%04x", -ret);
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
  mbedtls_ssl_set_bio(&t->ssl, t, mbed_send_cb, mbed_recv_cb, NULL);

  transport->read      = mbed_read;
  transport->writev    = mbed_writev;
  transport->handshake = mbed_handshake;
  transport->alpn      = mbed_alpn;
  transport->destroy   = mbed_destroy;
  transport->ctx       = t;
}

/* ═══════════════════════════════════════════════════════════════════
 *  Client transport init
 * ═══════════════════════════════════════════════════════════════════
 */

int xTransportTlsClientInit(xTransport *transport, xTlsCtx tls_ctx,
                            const char *hostname, int fd) {
  if (!transport || !tls_ctx) return -1;

  mbedtls_ssl_config *client_conf =
    (mbedtls_ssl_config *)xTlsCtxGetNative(tls_ctx);
  if (!client_conf) return -1;

  xTlsMbedTLS_ *t = (xTlsMbedTLS_ *)calloc(1, sizeof(xTlsMbedTLS_));
  if (!t) return -1;

  t->fd = fd;

  /* No owned resources — the shared xTlsCtx owns everything */
  t->owned_conf        = NULL;
  t->owned_ca          = NULL;
  t->owned_client_cert = NULL;
  t->owned_client_key  = NULL;
#if MBEDTLS_VERSION_NUMBER < 0x04000000
  t->owned_entropy  = NULL;
  t->owned_ctr_drbg = NULL;
#endif

  mbedtls_ssl_init(&t->ssl);

  int ret = mbedtls_ssl_setup(&t->ssl, client_conf);
  if (ret != 0) {
    xLog(false, "xnet: mbedtls_ssl_setup failed: -0x%04x", -ret);
    goto fail;
  }

  /* Set hostname for SNI and verification */
  if (hostname) {
    ret = mbedtls_ssl_set_hostname(&t->ssl, hostname);
    if (ret != 0) {
      xLog(false, "xnet: mbedtls_ssl_set_hostname failed: -0x%04x", -ret);
      goto fail;
    }
  }

  /* Set custom I/O callbacks */
  mbedtls_ssl_set_bio(&t->ssl, t, mbed_send_cb, mbed_recv_cb, NULL);

  /* Fill transport vtable */
  transport->read      = mbed_read;
  transport->writev    = mbed_writev;
  transport->handshake = mbed_handshake;
  transport->alpn      = mbed_alpn;
  transport->destroy   = mbed_destroy;
  transport->ctx       = t;

  return 0;

fail:
  mbedtls_ssl_free(&t->ssl);
  free(t);
  return -1;
}

#endif /* X_HAS_MBEDTLS */
