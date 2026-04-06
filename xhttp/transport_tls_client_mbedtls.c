/*
 * Copyright 2025 The xKit Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * transport_tls_client_mbedtls.c - mbedTLS TLS client transport
 */

#ifdef XK_HAS_MBEDTLS

#include "transport_tls_client.h"

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
  mbedtls_ssl_conf_min_tls_version(     \
    (conf), MBEDTLS_SSL_VERSION_TLS1_2)
#else
/* mbedTLS 2.x */
#define XK_MBEDTLS_SET_MIN_TLS12(conf)           \
  mbedtls_ssl_conf_min_version(                   \
    (conf), MBEDTLS_SSL_MAJOR_VERSION_3,          \
    MBEDTLS_SSL_MINOR_VERSION_3)
#endif

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/uio.h>
#include <unistd.h>
#include <xbase/log.h>

/* ─────────── Per-connection TLS client state ─────────── */

typedef struct {
  mbedtls_ssl_context  ssl;
  mbedtls_ssl_config   conf;
  mbedtls_x509_crt     ca_cert;
  mbedtls_x509_crt     client_cert;
  mbedtls_pk_context   client_key;
#if MBEDTLS_VERSION_NUMBER < 0x04000000
  mbedtls_entropy_context  entropy;
  mbedtls_ctr_drbg_context ctr_drbg;
#endif
  int  fd;
  int  has_ca;
  int  has_client_cert;
  char alpn_result[16];
} xTlsClientMbed_;

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
  if (t->has_client_cert) {
    mbedtls_x509_crt_free(&t->client_cert);
    mbedtls_pk_free(&t->client_key);
  }
  if (t->has_ca) {
    mbedtls_x509_crt_free(&t->ca_cert);
  }
#if MBEDTLS_VERSION_NUMBER < 0x04000000
  mbedtls_ctr_drbg_free(&t->ctr_drbg);
  mbedtls_entropy_free(&t->entropy);
#endif
  mbedtls_ssl_config_free(&t->conf);
  free(t);
}

/* ─────────── Public API ─────────── */

int xHttpTlsClientTransportInit(xHttpTransport *transport,
                                const xTlsClientConf *conf,
                                const char *hostname,
                                int fd) {
  if (!transport) return -1;

  xTlsClientMbed_ *t =
    (xTlsClientMbed_ *)calloc(1, sizeof(xTlsClientMbed_));
  if (!t) return -1;

  mbedtls_ssl_config_init(&t->conf);
  mbedtls_ssl_init(&t->ssl);
  mbedtls_x509_crt_init(&t->ca_cert);
  mbedtls_x509_crt_init(&t->client_cert);
  mbedtls_pk_init(&t->client_key);
  t->fd              = fd;
  t->has_ca          = 0;
  t->has_client_cert = 0;
  t->alpn_result[0]  = '\0';

  int ret;

#if MBEDTLS_VERSION_NUMBER < 0x04000000
  mbedtls_entropy_init(&t->entropy);
  mbedtls_ctr_drbg_init(&t->ctr_drbg);
  ret = mbedtls_ctr_drbg_seed(&t->ctr_drbg,
                              mbedtls_entropy_func,
                              &t->entropy, NULL, 0);
  if (ret != 0) {
    xLog(false,
         "xhttp: mbedtls_ctr_drbg_seed failed: -0x%04x",
         -ret);
    goto fail;
  }
#endif

  /* Configure as TLS client */
  ret = mbedtls_ssl_config_defaults(
    &t->conf, MBEDTLS_SSL_IS_CLIENT,
    MBEDTLS_SSL_TRANSPORT_STREAM,
    MBEDTLS_SSL_PRESET_DEFAULT);
  if (ret != 0) {
    xLog(false,
         "xhttp: mbedtls_ssl_config_defaults failed: "
         "-0x%04x", -ret);
    goto fail;
  }

#if MBEDTLS_VERSION_NUMBER < 0x04000000
  mbedtls_ssl_conf_rng(&t->conf,
                       mbedtls_ctr_drbg_random,
                       &t->ctr_drbg);
#endif

  /* Set minimum TLS version to 1.2 */
  XK_MBEDTLS_SET_MIN_TLS12(&t->conf);

  int skip_verify = conf ? conf->skip_verify : 0;

  if (!skip_verify) {
    /* Load CA certificates */
    if (conf && conf->ca) {
      ret = mbedtls_x509_crt_parse_file(&t->ca_cert,
                                        conf->ca);
      if (ret != 0) {
        xLog(false,
             "xhttp: failed to load CA: %s "
             "(ret=-0x%04x)", conf->ca, -ret);
        goto fail;
      }
    } else {
      /* Load system default CA bundle.
       * mbedTLS doesn't have a single "load system CAs"
       * call, so we try common paths. */
      static const char *ca_paths[] = {
        "/etc/ssl/certs/ca-certificates.crt",
        "/etc/pki/tls/certs/ca-bundle.crt",
        "/usr/local/share/certs/ca-root-nss.crt",
        "/etc/ssl/cert.pem",
        NULL,
      };
      int loaded = 0;
      for (int i = 0; ca_paths[i]; i++) {
        ret = mbedtls_x509_crt_parse_file(
          &t->ca_cert, ca_paths[i]);
        if (ret == 0) { loaded = 1; break; }
      }
      if (!loaded) {
        /* Try directory-based loading */
        ret = mbedtls_x509_crt_parse_path(
          &t->ca_cert, "/etc/ssl/certs");
        if (ret == 0) loaded = 1;
      }
      if (!loaded) {
        xLog(false,
             "xhttp: no system CA bundle found "
             "for mbedTLS client");
        /* Continue anyway; verification will fail
         * if the server cert is not self-signed */
      }
    }
    mbedtls_ssl_conf_ca_chain(&t->conf,
                              &t->ca_cert, NULL);
    t->has_ca = 1;
    mbedtls_ssl_conf_authmode(
      &t->conf, MBEDTLS_SSL_VERIFY_REQUIRED);
  } else {
    mbedtls_ssl_conf_authmode(
      &t->conf, MBEDTLS_SSL_VERIFY_NONE);
  }

  /* Load client certificate for mTLS (optional) */
  if (conf && conf->cert && conf->key) {
    ret = mbedtls_x509_crt_parse_file(&t->client_cert,
                                      conf->cert);
    if (ret != 0) {
      xLog(false,
           "xhttp: failed to load client cert: %s "
           "(ret=-0x%04x)", conf->cert, -ret);
      goto fail;
    }

    const char *pwd = conf->key_password;

#if MBEDTLS_VERSION_NUMBER >= 0x04000000
    ret = mbedtls_pk_parse_keyfile(
      &t->client_key, conf->key,
      pwd);
#elif MBEDTLS_VERSION_NUMBER >= 0x03000000
    ret = mbedtls_pk_parse_keyfile(
      &t->client_key, conf->key,
      pwd,
      mbedtls_ctr_drbg_random, &t->ctr_drbg);
#else
    ret = mbedtls_pk_parse_keyfile(
      &t->client_key, conf->key, pwd);
#endif
    if (ret != 0) {
      xLog(false,
           "xhttp: failed to load client key: %s "
           "(ret=-0x%04x)", conf->key, -ret);
      goto fail;
    }

    ret = mbedtls_ssl_conf_own_cert(
      &t->conf, &t->client_cert, &t->client_key);
    if (ret != 0) {
      xLog(false,
           "xhttp: mbedtls_ssl_conf_own_cert failed: "
           "-0x%04x", -ret);
      goto fail;
    }
    t->has_client_cert = 1;
  }

  /* Set up SSL context */
  ret = mbedtls_ssl_setup(&t->ssl, &t->conf);
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
  if (t->has_client_cert) {
    mbedtls_x509_crt_free(&t->client_cert);
    mbedtls_pk_free(&t->client_key);
  }
  if (t->has_ca) {
    mbedtls_x509_crt_free(&t->ca_cert);
  }
#if MBEDTLS_VERSION_NUMBER < 0x04000000
  mbedtls_ctr_drbg_free(&t->ctr_drbg);
  mbedtls_entropy_free(&t->entropy);
#endif
  mbedtls_ssl_config_free(&t->conf);
  free(t);
  return -1;
}

#endif /* XK_HAS_MBEDTLS */
