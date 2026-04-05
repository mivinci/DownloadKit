/*
 * Copyright 2025 The xKit Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * transport_tls_mbedtls.c - mbedTLS transport implementation
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
#define XK_MBEDTLS_SET_MIN_TLS12(conf) \
  mbedtls_ssl_conf_min_tls_version((conf), MBEDTLS_SSL_VERSION_TLS1_2)
#else
/* mbedTLS 2.x */
#define XK_MBEDTLS_SET_MIN_TLS12(conf)                              \
  mbedtls_ssl_conf_min_version((conf), MBEDTLS_SSL_MAJOR_VERSION_3, \
                               MBEDTLS_SSL_MINOR_VERSION_3)
#endif

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/uio.h>
#include <unistd.h>
#include <xbase/log.h>

/* ───────────────────── ALPN protocol list ───────────────────── */

static const char *alpn_protos[] = {"h2", "http/1.1", NULL};

/* ───────────────────── TLS context (server-level) ───────────────────── */

typedef struct {
  mbedtls_ssl_config conf;
  mbedtls_x509_crt   cert;
  mbedtls_pk_context pkey;
  mbedtls_x509_crt   ca_cert;
#if MBEDTLS_VERSION_NUMBER < 0x04000000
  mbedtls_entropy_context  entropy;
  mbedtls_ctr_drbg_context ctr_drbg;
#endif
  int has_ca;
} xHttpTlsCtxMbedTLS_;

void *xHttpTlsCtxCreateMbedTLS(const xHttpTlsServerConf *config) {
  xHttpTlsCtxMbedTLS_ *ctx =
    (xHttpTlsCtxMbedTLS_ *)calloc(1, sizeof(xHttpTlsCtxMbedTLS_));
  if (!ctx) return NULL;

  mbedtls_ssl_config_init(&ctx->conf);
  mbedtls_x509_crt_init(&ctx->cert);
  mbedtls_pk_init(&ctx->pkey);
  mbedtls_x509_crt_init(&ctx->ca_cert);
  ctx->has_ca = 0;

  int ret;

#if MBEDTLS_VERSION_NUMBER < 0x04000000
  /* mbedTLS 2.x/3.x: seed the random number generator manually */
  mbedtls_entropy_init(&ctx->entropy);
  mbedtls_ctr_drbg_init(&ctx->ctr_drbg);
  ret = mbedtls_ctr_drbg_seed(&ctx->ctr_drbg, mbedtls_entropy_func,
                              &ctx->entropy, NULL, 0);
  if (ret != 0) {
    xLog(false, "xhttp: mbedtls_ctr_drbg_seed failed: -0x%04x", -ret);
    goto fail;
  }
#endif

  /* Configure as TLS server */
  ret = mbedtls_ssl_config_defaults(&ctx->conf, MBEDTLS_SSL_IS_SERVER,
                                    MBEDTLS_SSL_TRANSPORT_STREAM,
                                    MBEDTLS_SSL_PRESET_DEFAULT);
  if (ret != 0) {
    xLog(false, "xhttp: mbedtls_ssl_config_defaults failed: -0x%04x", -ret);
    goto fail;
  }

#if MBEDTLS_VERSION_NUMBER < 0x04000000
  mbedtls_ssl_conf_rng(&ctx->conf, mbedtls_ctr_drbg_random, &ctx->ctr_drbg);
#endif

  /* Set minimum TLS version to 1.2 */
  XK_MBEDTLS_SET_MIN_TLS12(&ctx->conf);

  /* Load certificate */
  ret = mbedtls_x509_crt_parse_file(&ctx->cert, config->cert_file);
  if (ret != 0) {
    xLog(false, "xhttp: failed to load certificate: %s (ret=-0x%04x)",
         config->cert_file, -ret);
    goto fail;
  }

  /* Load private key */
#if MBEDTLS_VERSION_NUMBER >= 0x04000000
  ret = mbedtls_pk_parse_keyfile(&ctx->pkey, config->key_file, NULL);
#elif MBEDTLS_VERSION_NUMBER >= 0x03000000
  ret = mbedtls_pk_parse_keyfile(&ctx->pkey, config->key_file, NULL,
                                 mbedtls_ctr_drbg_random, &ctx->ctr_drbg);
#else
  ret = mbedtls_pk_parse_keyfile(&ctx->pkey, config->key_file, NULL);
#endif
  if (ret != 0) {
    xLog(false, "xhttp: failed to load private key: %s (ret=-0x%04x)",
         config->key_file, -ret);
    goto fail;
  }

  /* Set own certificate and key */
  ret = mbedtls_ssl_conf_own_cert(&ctx->conf, &ctx->cert, &ctx->pkey);
  if (ret != 0) {
    xLog(false, "xhttp: mbedtls_ssl_conf_own_cert failed: -0x%04x", -ret);
    goto fail;
  }

  /* Load CA certificate for client verification (optional) */
  if (config->ca_file) {
    ret = mbedtls_x509_crt_parse_file(&ctx->ca_cert, config->ca_file);
    if (ret != 0) {
      xLog(false, "xhttp: failed to load CA certificate: %s (ret=-0x%04x)",
           config->ca_file, -ret);
      goto fail;
    }
    mbedtls_ssl_conf_ca_chain(&ctx->conf, &ctx->ca_cert, NULL);
    ctx->has_ca = 1;
  }

  /* Client verification mode */
  if (config->verify_client == 2) {
    mbedtls_ssl_conf_authmode(&ctx->conf, MBEDTLS_SSL_VERIFY_REQUIRED);
  } else if (config->verify_client == 1) {
    mbedtls_ssl_conf_authmode(&ctx->conf, MBEDTLS_SSL_VERIFY_OPTIONAL);
  } else {
    mbedtls_ssl_conf_authmode(&ctx->conf, MBEDTLS_SSL_VERIFY_NONE);
  }

  /* Configure ALPN */
  ret = mbedtls_ssl_conf_alpn_protocols(&ctx->conf, alpn_protos);
  if (ret != 0) {
    xLog(false, "xhttp: mbedtls_ssl_conf_alpn_protocols failed: -0x%04x", -ret);
    goto fail;
  }

  return ctx;

fail:
#if MBEDTLS_VERSION_NUMBER < 0x04000000
  mbedtls_ctr_drbg_free(&ctx->ctr_drbg);
  mbedtls_entropy_free(&ctx->entropy);
#endif
  mbedtls_pk_free(&ctx->pkey);
  mbedtls_x509_crt_free(&ctx->cert);
  mbedtls_x509_crt_free(&ctx->ca_cert);
  mbedtls_ssl_config_free(&ctx->conf);
  free(ctx);
  return NULL;
}

void xHttpTlsCtxDestroyMbedTLS(void *ctx) {
  if (!ctx) return;
  xHttpTlsCtxMbedTLS_ *c = (xHttpTlsCtxMbedTLS_ *)ctx;
#if MBEDTLS_VERSION_NUMBER < 0x04000000
  mbedtls_ctr_drbg_free(&c->ctr_drbg);
  mbedtls_entropy_free(&c->entropy);
#endif
  mbedtls_pk_free(&c->pkey);
  mbedtls_x509_crt_free(&c->cert);
  if (c->has_ca) mbedtls_x509_crt_free(&c->ca_cert);
  mbedtls_ssl_config_free(&c->conf);
  free(c);
}

/* ───────────────────── Per-connection TLS state ───────────────────── */

typedef struct {
  mbedtls_ssl_context ssl;
  mbedtls_net_context net;
  int                 fd;
} xHttpTlsMbedTLS_;

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

void xHttpTlsTransportInitMbedTLS(xHttpTransport *transport, void *tls_ctx,
                                  int fd) {
  if (!transport || !tls_ctx) return;

  xHttpTlsCtxMbedTLS_ *server_ctx = (xHttpTlsCtxMbedTLS_ *)tls_ctx;

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

  int ret = mbedtls_ssl_setup(&t->ssl, &server_ctx->conf);
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
