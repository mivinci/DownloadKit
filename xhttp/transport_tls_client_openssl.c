/*
 * Copyright 2025 The xKit Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * transport_tls_client_openssl.c - OpenSSL TLS client transport
 *
 * Per-connection SSL object using a shared SSL_CTX from xTlsCtx.
 */

#ifdef XK_HAS_OPENSSL

#include "transport_tls_client.h"

#include <xnet/tls.h>

/* Internal: get native SSL_CTX* from opaque xTlsCtx handle */
extern void *xTlsCtxGetNative(xTlsCtx ctx);

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
  SSL  *ssl;
  int   fd;
  char  alpn_result[16];
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
  /* SSL_CTX is owned by the shared xTlsCtx — do NOT free it */
  free(t);
}

/* ─────────────────── Public API ─────────────────── */

int xHttpTlsClientTransportInit(xHttpTransport *transport,
                                xTlsCtx tls_ctx,
                                const char *hostname,
                                int fd) {
  if (!transport || !tls_ctx) return -1;

  SSL_CTX *ssl_ctx = (SSL_CTX *)xTlsCtxGetNative(tls_ctx);
  if (!ssl_ctx) return -1;

  SSL *ssl = SSL_new(ssl_ctx);
  if (!ssl) return -1;

  SSL_set_fd(ssl, fd);
  SSL_set_connect_state(ssl);

  /* SNI + hostname verification */
  int skip_verify = (SSL_CTX_get_verify_mode(ssl_ctx) == SSL_VERIFY_NONE);
  if (hostname && !skip_verify) {
    SSL_set_tlsext_host_name(ssl, hostname);
    SSL_set1_host(ssl, hostname);
  } else if (hostname) {
    /* Set SNI even when skipping verify (some servers need it) */
    SSL_set_tlsext_host_name(ssl, hostname);
  }

  /* Allocate per-connection state */
  xTlsClient_ *t = (xTlsClient_ *)calloc(1, sizeof(xTlsClient_));
  if (!t) {
    SSL_free(ssl);
    return -1;
  }

  t->ssl            = ssl;
  t->fd             = fd;
  t->alpn_result[0] = '\0';

  transport->read      = tls_client_read;
  transport->writev    = tls_client_writev;
  transport->handshake = tls_client_handshake;
  transport->alpn      = tls_client_alpn;
  transport->destroy   = tls_client_destroy;
  transport->ctx       = t;

  return 0;
}

#endif /* XK_HAS_OPENSSL */
