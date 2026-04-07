/*
 * Copyright 2025 The xKit Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * transport_tls_client_openssl.c - OpenSSL TLS client transport
 */

#ifdef XK_HAS_OPENSSL

#include "transport_tls_client.h"

#include <openssl/err.h>
#include <openssl/ssl.h>
#include <openssl/x509v3.h>

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/uio.h>
#include <xbase/log.h>

/* ─────────────────── Per-connection TLS state ─────────────────── */

XDEF_STRUCT(xTlsClient_) {
  SSL     *ssl;
  SSL_CTX *ctx;  /**< Owned by this transport (one CTX per conn) */
  int      fd;
  char     alpn_result[16];
};

/* ─────────────────── Transport vtable callbacks ─────────────────── */

static ssize_t tls_client_read(void *ctx, void *buf, size_t len) {
  xTlsClient_ *t = (xTlsClient_ *)ctx;
  ERR_clear_error();
  int n = SSL_read(t->ssl, buf, (int)len);
  if (n > 0) return (ssize_t)n;

  int err = SSL_get_error(t->ssl, n);
  switch (err) {
  case SSL_ERROR_WANT_READ:
  case SSL_ERROR_WANT_WRITE:
    errno = EAGAIN;
    return -1;
  case SSL_ERROR_ZERO_RETURN:
    return 0;
  default:
    errno = EIO;
    return -1;
  }
}

static ssize_t tls_client_writev(void *ctx,
                                 const struct iovec *iov,
                                 int iovcnt) {
  xTlsClient_ *t     = (xTlsClient_ *)ctx;
  ssize_t       total = 0;

  for (int i = 0; i < iovcnt; i++) {
    if (iov[i].iov_len == 0) continue;

    ERR_clear_error();
    int n = SSL_write(t->ssl, iov[i].iov_base,
                      (int)iov[i].iov_len);
    if (n > 0) {
      total += n;
      if ((size_t)n < iov[i].iov_len) break;
      continue;
    }

    int err = SSL_get_error(t->ssl, n);
    if (err == SSL_ERROR_WANT_READ ||
        err == SSL_ERROR_WANT_WRITE) {
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

static int tls_client_handshake(void *ctx) {
  xTlsClient_ *t = (xTlsClient_ *)ctx;
  ERR_clear_error();
  int ret = SSL_do_handshake(t->ssl);
  if (ret == 1) {
    /* Cache ALPN result */
    const unsigned char *alpn_data = NULL;
    unsigned int         alpn_len  = 0;
    SSL_get0_alpn_selected(t->ssl, &alpn_data, &alpn_len);
    if (alpn_data && alpn_len > 0 &&
        alpn_len < sizeof(t->alpn_result)) {
      memcpy(t->alpn_result, alpn_data, alpn_len);
      t->alpn_result[alpn_len] = '\0';
    } else {
      t->alpn_result[0] = '\0';
    }
    return xHttpTransportResult_Done;
  }

  int err = SSL_get_error(t->ssl, ret);
  switch (err) {
  case SSL_ERROR_WANT_READ:
    return xHttpTransportResult_WantRead;
  case SSL_ERROR_WANT_WRITE:
    return xHttpTransportResult_WantWrite;
  default:
    return xHttpTransportResult_Error;
  }
}

static const char *tls_client_alpn(void *ctx) {
  xTlsClient_ *t = (xTlsClient_ *)ctx;
  if (t->alpn_result[0] == '\0') return NULL;
  return t->alpn_result;
}

static void tls_client_destroy(void *ctx) {
  xTlsClient_ *t = (xTlsClient_ *)ctx;
  if (t->ssl) {
    BIO *rbio = SSL_get_rbio(t->ssl);
    if (rbio) BIO_set_close(rbio, BIO_NOCLOSE);
    BIO *wbio = SSL_get_wbio(t->ssl);
    if (wbio && wbio != rbio)
      BIO_set_close(wbio, BIO_NOCLOSE);
    ERR_clear_error();
    SSL_free(t->ssl);
  }
  if (t->ctx) SSL_CTX_free(t->ctx);
  free(t);
}

/* ─────────────────── Public API ─────────────────── */

int xHttpTlsClientTransportInit(xHttpTransport *transport,
                                const xTlsClientConf *conf,
                                const char *hostname,
                                int fd) {
  if (!transport) return -1;

  SSL_CTX     *ctx = NULL;
  SSL         *ssl = NULL;
  xTlsClient_ *t  = NULL;

  /* Create per-connection SSL_CTX (client method) */
  ctx = SSL_CTX_new(TLS_client_method());
  if (!ctx) {
    xLog(false, "xhttp: SSL_CTX_new(client) failed");
    return -1;
  }

  SSL_CTX_set_min_proto_version(ctx, TLS1_2_VERSION);

  int skip_verify = conf ? conf->skip_verify : 0;

  if (!skip_verify) {
    /* Load CA certificates */
    if (conf && conf->ca) {
      if (SSL_CTX_load_verify_locations(ctx, conf->ca, NULL)
          != 1) {
        xLog(false, "xhttp: failed to load CA: %s", conf->ca);
        goto fail;
      }
    } else {
      /* Use system default CA store */
      SSL_CTX_set_default_verify_paths(ctx);
    }
    SSL_CTX_set_verify(ctx, SSL_VERIFY_PEER, NULL);
  } else {
    SSL_CTX_set_verify(ctx, SSL_VERIFY_NONE, NULL);
  }

  /* Load client certificate for mTLS (optional) */
  if (conf && conf->cert) {
    if (SSL_CTX_use_certificate_chain_file(ctx, conf->cert)
        != 1) {
      xLog(false, "xhttp: failed to load client cert: %s",
           conf->cert);
      goto fail;
    }
  }
  if (conf && conf->key) {
    if (SSL_CTX_use_PrivateKey_file(ctx, conf->key,
                                    SSL_FILETYPE_PEM) != 1) {
      xLog(false, "xhttp: failed to load client key: %s",
           conf->key);
      goto fail;
    }
  }

  /* Create SSL object */
  ssl = SSL_new(ctx);
  if (!ssl) goto fail;

  SSL_set_fd(ssl, fd);
  SSL_set_connect_state(ssl);

  /* SNI + hostname verification */
  if (hostname && !skip_verify) {
    SSL_set_tlsext_host_name(ssl, hostname);
    SSL_set1_host(ssl, hostname);
  } else if (hostname) {
    /* Set SNI even when skipping verify (some servers need it) */
    SSL_set_tlsext_host_name(ssl, hostname);
  }

  /* Allocate per-connection state */
  t = (xTlsClient_ *)calloc(1, sizeof(xTlsClient_));
  if (!t) goto fail_ssl;

  t->ssl            = ssl;
  t->ctx            = ctx;
  t->fd             = fd;
  t->alpn_result[0] = '\0';

  transport->read      = tls_client_read;
  transport->writev    = tls_client_writev;
  transport->handshake = tls_client_handshake;
  transport->alpn      = tls_client_alpn;
  transport->destroy   = tls_client_destroy;
  transport->ctx       = t;

  return 0;

fail_ssl:
  SSL_free(ssl);
fail:
  if (ctx) SSL_CTX_free(ctx);
  return -1;
}

#endif /* XK_HAS_OPENSSL */
