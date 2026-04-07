/*
 * Copyright 2025 The xKit Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * transport_mbedtls.c - mbedTLS TLS transport implementation
 *
 * Provides both server-side and client-side TLS transport using mbedTLS.
 * ALPN is parameterized (not hardcoded) for the server context.
 */

#ifdef XK_HAS_MBEDTLS

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

/* ═══════════════════════════════════════════════════════════════════
 *  TLS context (server-level)
 * ═══════════════════════════════════════════════════════════════════
 */

XDEF_STRUCT(xTlsCtxMbedTLS_) {
  mbedtls_ssl_config conf;
  mbedtls_x509_crt   cert;
  mbedtls_pk_context pkey;
  mbedtls_x509_crt   ca_cert;
#if MBEDTLS_VERSION_NUMBER < 0x04000000
  mbedtls_entropy_context  entropy;
  mbedtls_ctr_drbg_context ctr_drbg;
#endif
  int          has_ca;
  const char **alpn_list; /**< Borrowed pointer to user's ALPN list */
};

xTlsCtx xTlsCtxCreate(const xTlsConf *config) {
  if (!config || !config->cert || !config->key) return NULL;

  xTlsCtxMbedTLS_ *ctx = (xTlsCtxMbedTLS_ *)calloc(1, sizeof(xTlsCtxMbedTLS_));
  if (!ctx) return NULL;

  mbedtls_ssl_config_init(&ctx->conf);
  mbedtls_x509_crt_init(&ctx->cert);
  mbedtls_pk_init(&ctx->pkey);
  mbedtls_x509_crt_init(&ctx->ca_cert);
  ctx->has_ca    = 0;
  ctx->alpn_list = NULL;

  int ret;

#if MBEDTLS_VERSION_NUMBER < 0x04000000
  /* mbedTLS 2.x/3.x: seed the random number generator manually */
  mbedtls_entropy_init(&ctx->entropy);
  mbedtls_ctr_drbg_init(&ctx->ctr_drbg);
  ret = mbedtls_ctr_drbg_seed(&ctx->ctr_drbg, mbedtls_entropy_func,
                              &ctx->entropy, NULL, 0);
  if (ret != 0) {
    xLog(false, "xnet: mbedtls_ctr_drbg_seed failed: -0x%04x", -ret);
    goto fail;
  }
#endif

  /* Configure as TLS server */
  ret = mbedtls_ssl_config_defaults(&ctx->conf, MBEDTLS_SSL_IS_SERVER,
                                    MBEDTLS_SSL_TRANSPORT_STREAM,
                                    MBEDTLS_SSL_PRESET_DEFAULT);
  if (ret != 0) {
    xLog(false, "xnet: mbedtls_ssl_config_defaults failed: -0x%04x", -ret);
    goto fail;
  }

#if MBEDTLS_VERSION_NUMBER < 0x04000000
  mbedtls_ssl_conf_rng(&ctx->conf, mbedtls_ctr_drbg_random, &ctx->ctr_drbg);
#endif

  /* Set minimum TLS version to 1.2 */
  XK_MBEDTLS_SET_MIN_TLS12(&ctx->conf);

  /* Load certificate */
  ret = mbedtls_x509_crt_parse_file(&ctx->cert, config->cert);
  if (ret != 0) {
    xLog(false, "xnet: failed to load certificate: %s (ret=-0x%04x)",
         config->cert, -ret);
    goto fail;
  }

  /* Load private key */
#if MBEDTLS_VERSION_NUMBER >= 0x04000000
  ret = mbedtls_pk_parse_keyfile(&ctx->pkey, config->key, NULL);
#elif MBEDTLS_VERSION_NUMBER >= 0x03000000
  ret = mbedtls_pk_parse_keyfile(&ctx->pkey, config->key, NULL,
                                 mbedtls_ctr_drbg_random, &ctx->ctr_drbg);
#else
  ret = mbedtls_pk_parse_keyfile(&ctx->pkey, config->key, NULL);
#endif
  if (ret != 0) {
    xLog(false, "xnet: failed to load private key: %s (ret=-0x%04x)",
         config->key, -ret);
    goto fail;
  }

  /* Set own certificate and key */
  ret = mbedtls_ssl_conf_own_cert(&ctx->conf, &ctx->cert, &ctx->pkey);
  if (ret != 0) {
    xLog(false, "xnet: mbedtls_ssl_conf_own_cert failed: -0x%04x", -ret);
    goto fail;
  }

  /* Load CA certificate for client verification (optional) */
  if (config->ca) {
    ret = mbedtls_x509_crt_parse_file(&ctx->ca_cert, config->ca);
    if (ret != 0) {
      xLog(false, "xnet: failed to load CA certificate: %s (ret=-0x%04x)",
           config->ca, -ret);
      goto fail;
    }
    mbedtls_ssl_conf_ca_chain(&ctx->conf, &ctx->ca_cert, NULL);
    ctx->has_ca = 1;
  }

  /* Peer verification mode */
  if (config->skip_verify) {
    mbedtls_ssl_conf_authmode(&ctx->conf, MBEDTLS_SSL_VERIFY_NONE);
  } else {
    mbedtls_ssl_conf_authmode(&ctx->conf, MBEDTLS_SSL_VERIFY_REQUIRED);
  }

  /* Configure ALPN (parameterized) */
  if (config->alpn) {
    ret = mbedtls_ssl_conf_alpn_protocols(&ctx->conf, config->alpn);
    if (ret != 0) {
      xLog(false, "xnet: mbedtls_ssl_conf_alpn_protocols failed: -0x%04x",
           -ret);
      goto fail;
    }
    ctx->alpn_list = config->alpn;
  }

  return (xTlsCtx)ctx;

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

void xTlsCtxDestroy(xTlsCtx raw) {
  if (!raw) return;
  xTlsCtxMbedTLS_ *ctx = (xTlsCtxMbedTLS_ *)raw;
#if MBEDTLS_VERSION_NUMBER < 0x04000000
  mbedtls_ctr_drbg_free(&ctx->ctr_drbg);
  mbedtls_entropy_free(&ctx->entropy);
#endif
  mbedtls_pk_free(&ctx->pkey);
  mbedtls_x509_crt_free(&ctx->cert);
  if (ctx->has_ca) mbedtls_x509_crt_free(&ctx->ca_cert);
  mbedtls_ssl_config_free(&ctx->conf);
  free(ctx);
}

int xTlsCtxReload(xTlsCtx raw, const xTlsConf *config) {
  if (!raw || !config || !config->cert || !config->key) return -1;

  xTlsCtxMbedTLS_ *ctx = (xTlsCtxMbedTLS_ *)raw;
  int ret;

  /* Reload certificate: free old, parse new */
  mbedtls_x509_crt new_cert;
  mbedtls_x509_crt_init(&new_cert);
  ret = mbedtls_x509_crt_parse_file(&new_cert, config->cert);
  if (ret != 0) {
    xLog(false, "xnet: reload: failed to load certificate: %s (ret=-0x%04x)",
         config->cert, -ret);
    mbedtls_x509_crt_free(&new_cert);
    return -1;
  }

  /* Reload private key: free old, parse new */
  mbedtls_pk_context new_pkey;
  mbedtls_pk_init(&new_pkey);
#if MBEDTLS_VERSION_NUMBER >= 0x04000000
  ret = mbedtls_pk_parse_keyfile(&new_pkey, config->key, NULL);
#elif MBEDTLS_VERSION_NUMBER >= 0x03000000
  ret = mbedtls_pk_parse_keyfile(&new_pkey, config->key, NULL,
                                 mbedtls_ctr_drbg_random, &ctx->ctr_drbg);
#else
  ret = mbedtls_pk_parse_keyfile(&new_pkey, config->key, NULL);
#endif
  if (ret != 0) {
    xLog(false, "xnet: reload: failed to load private key: %s (ret=-0x%04x)",
         config->key, -ret);
    mbedtls_x509_crt_free(&new_cert);
    mbedtls_pk_free(&new_pkey);
    return -1;
  }

  /* Swap in the new cert and key */
  mbedtls_x509_crt_free(&ctx->cert);
  mbedtls_pk_free(&ctx->pkey);
  ctx->cert = new_cert;
  ctx->pkey = new_pkey;

  /* Re-bind own cert to the config */
  ret = mbedtls_ssl_conf_own_cert(&ctx->conf, &ctx->cert, &ctx->pkey);
  if (ret != 0) {
    xLog(false, "xnet: reload: mbedtls_ssl_conf_own_cert failed: -0x%04x",
         -ret);
    return -1;
  }

  /* Reload CA certificate (optional) */
  if (config->ca) {
    mbedtls_x509_crt new_ca;
    mbedtls_x509_crt_init(&new_ca);
    ret = mbedtls_x509_crt_parse_file(&new_ca, config->ca);
    if (ret != 0) {
      xLog(false, "xnet: reload: failed to load CA: %s (ret=-0x%04x)",
           config->ca, -ret);
      mbedtls_x509_crt_free(&new_ca);
      return -1;
    }
    if (ctx->has_ca) mbedtls_x509_crt_free(&ctx->ca_cert);
    ctx->ca_cert = new_ca;
    ctx->has_ca  = 1;
    mbedtls_ssl_conf_ca_chain(&ctx->conf, &ctx->ca_cert, NULL);
  }

  /* Update verification mode */
  if (config->skip_verify) {
    mbedtls_ssl_conf_authmode(&ctx->conf, MBEDTLS_SSL_VERIFY_NONE);
  } else {
    mbedtls_ssl_conf_authmode(&ctx->conf, MBEDTLS_SSL_VERIFY_REQUIRED);
  }

  return 0;
}

void *xTlsCtxGetNative_(xTlsCtx raw) {
  if (!raw) return NULL;
  xTlsCtxMbedTLS_ *ctx = (xTlsCtxMbedTLS_ *)raw;
  return &ctx->conf;
}

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

  xTlsCtxMbedTLS_ *server_ctx = (xTlsCtxMbedTLS_ *)tls_ctx;

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

  int ret = mbedtls_ssl_setup(&t->ssl, &server_ctx->conf);
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

int xTransportTlsClientInit(xTransport *transport, const xTlsConf *conf,
                            const char *hostname, int fd) {
  if (!transport) return -1;

  xTlsMbedTLS_ *t = (xTlsMbedTLS_ *)calloc(1, sizeof(xTlsMbedTLS_));
  if (!t) return -1;

  t->fd = fd;

  /* Allocate per-connection config */
  t->owned_conf = (mbedtls_ssl_config *)calloc(1, sizeof(mbedtls_ssl_config));
  if (!t->owned_conf) goto fail;
  mbedtls_ssl_config_init(t->owned_conf);

  mbedtls_ssl_init(&t->ssl);

  int ret;

#if MBEDTLS_VERSION_NUMBER < 0x04000000
  t->owned_entropy =
    (mbedtls_entropy_context *)calloc(1, sizeof(mbedtls_entropy_context));
  t->owned_ctr_drbg =
    (mbedtls_ctr_drbg_context *)calloc(1, sizeof(mbedtls_ctr_drbg_context));
  if (!t->owned_entropy || !t->owned_ctr_drbg) goto fail;

  mbedtls_entropy_init(t->owned_entropy);
  mbedtls_ctr_drbg_init(t->owned_ctr_drbg);
  ret = mbedtls_ctr_drbg_seed(t->owned_ctr_drbg, mbedtls_entropy_func,
                              t->owned_entropy, NULL, 0);
  if (ret != 0) {
    xLog(false, "xnet: mbedtls_ctr_drbg_seed failed: -0x%04x", -ret);
    goto fail;
  }
#endif

  /* Configure as TLS client */
  ret = mbedtls_ssl_config_defaults(t->owned_conf, MBEDTLS_SSL_IS_CLIENT,
                                    MBEDTLS_SSL_TRANSPORT_STREAM,
                                    MBEDTLS_SSL_PRESET_DEFAULT);
  if (ret != 0) {
    xLog(false, "xnet: mbedtls_ssl_config_defaults failed: -0x%04x", -ret);
    goto fail;
  }

#if MBEDTLS_VERSION_NUMBER < 0x04000000
  mbedtls_ssl_conf_rng(t->owned_conf, mbedtls_ctr_drbg_random,
                       t->owned_ctr_drbg);
#endif

  /* Set minimum TLS version to 1.2 */
  XK_MBEDTLS_SET_MIN_TLS12(t->owned_conf);

  int skip_verify = conf ? conf->skip_verify : 0;

  if (!skip_verify) {
    /* Load CA certificates */
    t->owned_ca = (mbedtls_x509_crt *)calloc(1, sizeof(mbedtls_x509_crt));
    if (!t->owned_ca) goto fail;
    mbedtls_x509_crt_init(t->owned_ca);

    if (conf && conf->ca) {
      ret = mbedtls_x509_crt_parse_file(t->owned_ca, conf->ca);
      if (ret != 0) {
        xLog(false, "xnet: failed to load CA: %s (ret=-0x%04x)", conf->ca,
             -ret);
        goto fail;
      }
    } else {
      /* Load system default CA bundle */
      static const char *ca_paths[] = {
        "/etc/ssl/certs/ca-certificates.crt",
        "/etc/pki/tls/certs/ca-bundle.crt",
        "/usr/local/share/certs/ca-root-nss.crt",
        "/etc/ssl/cert.pem",
        NULL,
      };
      int loaded = 0;
      for (int i = 0; ca_paths[i]; i++) {
        ret = mbedtls_x509_crt_parse_file(t->owned_ca, ca_paths[i]);
        if (ret == 0) {
          loaded = 1;
          break;
        }
      }
      if (!loaded) {
        ret = mbedtls_x509_crt_parse_path(t->owned_ca, "/etc/ssl/certs");
        if (ret == 0) loaded = 1;
      }
      if (!loaded) {
        xLog(false, "xnet: no system CA bundle found for mbedTLS client");
      }
    }
    mbedtls_ssl_conf_ca_chain(t->owned_conf, t->owned_ca, NULL);
    mbedtls_ssl_conf_authmode(t->owned_conf, MBEDTLS_SSL_VERIFY_REQUIRED);
  } else {
    mbedtls_ssl_conf_authmode(t->owned_conf, MBEDTLS_SSL_VERIFY_NONE);
  }

  /* Load client certificate for mTLS (optional) */
  if (conf && conf->cert && conf->key) {
    t->owned_client_cert =
      (mbedtls_x509_crt *)calloc(1, sizeof(mbedtls_x509_crt));
    t->owned_client_key =
      (mbedtls_pk_context *)calloc(1, sizeof(mbedtls_pk_context));
    if (!t->owned_client_cert || !t->owned_client_key) goto fail;

    mbedtls_x509_crt_init(t->owned_client_cert);
    mbedtls_pk_init(t->owned_client_key);

    ret = mbedtls_x509_crt_parse_file(t->owned_client_cert, conf->cert);
    if (ret != 0) {
      xLog(false, "xnet: failed to load client cert: %s (ret=-0x%04x)",
           conf->cert, -ret);
      goto fail;
    }

    const char *pwd = conf->key_password;
#if MBEDTLS_VERSION_NUMBER >= 0x04000000
    ret = mbedtls_pk_parse_keyfile(t->owned_client_key, conf->key, pwd);
#elif MBEDTLS_VERSION_NUMBER >= 0x03000000
    ret = mbedtls_pk_parse_keyfile(t->owned_client_key, conf->key, pwd,
                                   mbedtls_ctr_drbg_random, t->owned_ctr_drbg);
#else
    ret = mbedtls_pk_parse_keyfile(t->owned_client_key, conf->key, pwd);
#endif
    if (ret != 0) {
      xLog(false, "xnet: failed to load client key: %s (ret=-0x%04x)",
           conf->key, -ret);
      goto fail;
    }

    ret = mbedtls_ssl_conf_own_cert(t->owned_conf, t->owned_client_cert,
                                    t->owned_client_key);
    if (ret != 0) {
      xLog(false, "xnet: mbedtls_ssl_conf_own_cert failed: -0x%04x", -ret);
      goto fail;
    }
  }

  /* Set up SSL context */
  ret = mbedtls_ssl_setup(&t->ssl, t->owned_conf);
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
  return -1;
}

#endif /* XK_HAS_MBEDTLS */
