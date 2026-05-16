/*
 * Copyright 2025 The libx Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * transport_openssl.c - OpenSSL per-connection TLS transport
 *
 * Provides both server-side and client-side TLS transport using OpenSSL.
 * TLS context management (xTlsCtxCreate etc.) lives in tls_openssl.c.
 */

#ifdef X_HAS_OPENSSL

#include "tls_private.h"
#include "transport.h"

#include <openssl/err.h>
#include <openssl/ssl.h>
#include <openssl/x509v3.h>

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/uio.h>
#include <x/base/log.h>

/* ═══════════════════════════════════════════════════════════════════
 *  Per-connection TLS state (shared by server and client)
 * ═══════════════════════════════════════════════════════════════════
 */

XDEF_STRUCT(xTlsOpenSSL_) {
  SSL     *ssl;
  SSL_CTX *owned_ctx; /**< Non-NULL only for client (per-conn CTX) */
  int      fd;
  char     alpn_result[16];
};

/* ═══════════════════════════════════════════════════════════════════
 *  Transport vtable callbacks (shared by server and client)
 * ═══════════════════════════════════════════════════════════════════
 */

static ssize_t openssl_read(void *ctx, void *buf, size_t len) {
  xTlsOpenSSL_ *t = (xTlsOpenSSL_ *)ctx;
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
    return 0; /* Clean shutdown (EOF) */
  default:
    errno = EIO;
    return -1;
  }
}

static ssize_t openssl_writev(void *ctx, const struct iovec *iov, int iovcnt) {
  xTlsOpenSSL_ *t     = (xTlsOpenSSL_ *)ctx;
  ssize_t       total = 0;

  for (int i = 0; i < iovcnt; i++) {
    if (iov[i].iov_len == 0) continue;

    ERR_clear_error();
    int n = SSL_write(t->ssl, iov[i].iov_base, (int)iov[i].iov_len);
    if (n > 0) {
      total += n;
      if ((size_t)n < iov[i].iov_len) break; /* Partial write */
      continue;
    }

    int err = SSL_get_error(t->ssl, n);
    if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE) {
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

static int openssl_handshake(void *ctx) {
  xTlsOpenSSL_ *t = (xTlsOpenSSL_ *)ctx;
  ERR_clear_error();
  int ret = SSL_do_handshake(t->ssl);
  if (ret == 1) {
    /* Handshake complete: cache ALPN result */
    const unsigned char *alpn_data = NULL;
    unsigned int         alpn_len  = 0;
    SSL_get0_alpn_selected(t->ssl, &alpn_data, &alpn_len);
    if (alpn_data && alpn_len > 0 && alpn_len < sizeof(t->alpn_result)) {
      memcpy(t->alpn_result, alpn_data, alpn_len);
      t->alpn_result[alpn_len] = '\0';
    } else {
      t->alpn_result[0] = '\0';
    }
    return xTransportResult_Done;
  }

  int err = SSL_get_error(t->ssl, ret);
  switch (err) {
  case SSL_ERROR_WANT_READ:
    return xTransportResult_WantRead;
  case SSL_ERROR_WANT_WRITE:
    return xTransportResult_WantWrite;
  default:
    return xTransportResult_Error;
  }
}

static const char *openssl_alpn(void *ctx) {
  xTlsOpenSSL_ *t = (xTlsOpenSSL_ *)ctx;
  if (t->alpn_result[0] == '\0') return NULL;
  return t->alpn_result;
}

static void openssl_destroy(void *ctx) {
  xTlsOpenSSL_ *t = (xTlsOpenSSL_ *)ctx;
  if (t->ssl) {
    /* Prevent SSL_free from closing the fd. The fd is owned by
     * xSocket and will be closed by xSocketDestroy(). */
    BIO *rbio = SSL_get_rbio(t->ssl);
    if (rbio) BIO_set_close(rbio, BIO_NOCLOSE);
    BIO *wbio = SSL_get_wbio(t->ssl);
    if (wbio && wbio != rbio) BIO_set_close(wbio, BIO_NOCLOSE);

    ERR_clear_error();
    SSL_free(t->ssl);
  }
  /* Client transport owns its SSL_CTX; server transport does not */
  if (t->owned_ctx) SSL_CTX_free(t->owned_ctx);
  free(t);
}

/* ═══════════════════════════════════════════════════════════════════
 *  Server transport init
 * ═══════════════════════════════════════════════════════════════════
 */

void xTransportTlsServerInit(xTransport *transport, xTlsCtx tls_ctx, int fd) {
  if (!transport || !tls_ctx) return;

  SSL_CTX *ssl_ctx = (SSL_CTX *)xTlsCtxGetNative(tls_ctx);
  if (!ssl_ctx) return;

  xTlsOpenSSL_ *t = (xTlsOpenSSL_ *)calloc(1, sizeof(xTlsOpenSSL_));
  if (!t) {
    transport->read      = NULL;
    transport->writev    = NULL;
    transport->handshake = NULL;
    transport->alpn      = NULL;
    transport->destroy   = NULL;
    transport->ctx       = NULL;
    return;
  }

  SSL *ssl = SSL_new(ssl_ctx);
  if (!ssl) {
    free(t);
    transport->read      = NULL;
    transport->writev    = NULL;
    transport->handshake = NULL;
    transport->alpn      = NULL;
    transport->destroy   = NULL;
    transport->ctx       = NULL;
    return;
  }

  SSL_set_fd(ssl, fd);
  SSL_set_accept_state(ssl);

  t->ssl            = ssl;
  t->owned_ctx      = NULL; /* Server does NOT own the SSL_CTX */
  t->fd             = fd;
  t->alpn_result[0] = '\0';

  transport->read      = openssl_read;
  transport->writev    = openssl_writev;
  transport->handshake = openssl_handshake;
  transport->alpn      = openssl_alpn;
  transport->destroy   = openssl_destroy;
  transport->ctx       = t;
}

/* ═══════════════════════════════════════════════════════════════════
 *  Client transport init
 * ═══════════════════════════════════════════════════════════════════
 */

int xTransportTlsClientInit(xTransport *transport, xTlsCtx tls_ctx,
                            const char *hostname, int fd) {
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
  xTlsOpenSSL_ *t = (xTlsOpenSSL_ *)calloc(1, sizeof(xTlsOpenSSL_));
  if (!t) {
    SSL_free(ssl);
    return -1;
  }

  t->ssl            = ssl;
  t->owned_ctx      = NULL; /* Client no longer owns the SSL_CTX */
  t->fd             = fd;
  t->alpn_result[0] = '\0';

  transport->read      = openssl_read;
  transport->writev    = openssl_writev;
  transport->handshake = openssl_handshake;
  transport->alpn      = openssl_alpn;
  transport->destroy   = openssl_destroy;
  transport->ctx       = t;

  return 0;
}

#endif /* X_HAS_OPENSSL */
