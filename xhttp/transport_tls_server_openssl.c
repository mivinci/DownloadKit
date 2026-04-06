/*
 * Copyright 2025 The xKit Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * transport_tls_server_openssl.c - OpenSSL TLS transport implementation
 */

#ifdef XK_HAS_OPENSSL

#include "transport_private.h"

#include <openssl/err.h>
#include <openssl/ssl.h>

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/uio.h>
#include <xbase/log.h>

/* ───────────────────── ALPN callback ───────────────────── */

static const unsigned char alpn_protos[] = {
  2, 'h', '2', 8, 'h', 't', 't', 'p', '/', '1', '.', '1',
};

static int alpn_select_cb(SSL *ssl, const unsigned char **out,
                          unsigned char *outlen, const unsigned char *in,
                          unsigned int inlen, void *arg) {
  (void)ssl;
  (void)arg;

  if (SSL_select_next_proto((unsigned char **)out, outlen, alpn_protos,
                            sizeof(alpn_protos), in,
                            inlen) != OPENSSL_NPN_NEGOTIATED) {
    return SSL_TLSEXT_ERR_NOACK;
  }
  return SSL_TLSEXT_ERR_OK;
}

/* ───────────────────── TLS context (server-level) ───────────────────── */

void *xHttpTlsCtxCreateOpenSSL(const xTlsServerConf *config) {
  SSL_CTX *ctx = SSL_CTX_new(TLS_server_method());
  if (!ctx) {
    xLog(false, "xhttp: SSL_CTX_new failed");
    return NULL;
  }

  /* Set minimum TLS version to 1.2 */
  SSL_CTX_set_min_proto_version(ctx, TLS1_2_VERSION);

  /* Load certificate */
  if (SSL_CTX_use_certificate_chain_file(ctx, config->cert) != 1) {
    xLog(false, "xhttp: failed to load certificate: %s", config->cert);
    SSL_CTX_free(ctx);
    return NULL;
  }

  /* Load private key */
  if (SSL_CTX_use_PrivateKey_file(ctx, config->key, SSL_FILETYPE_PEM) !=
      1) {
    xLog(false, "xhttp: failed to load private key: %s", config->key);
    SSL_CTX_free(ctx);
    return NULL;
  }

  /* Verify private key matches certificate */
  if (SSL_CTX_check_private_key(ctx) != 1) {
    xLog(false, "xhttp: private key does not match certificate");
    SSL_CTX_free(ctx);
    return NULL;
  }

  /* Load CA certificate for client verification (optional) */
  if (config->ca) {
    if (SSL_CTX_load_verify_locations(ctx, config->ca, NULL) != 1) {
      xLog(false, "xhttp: failed to load CA certificate: %s", config->ca);
      SSL_CTX_free(ctx);
      return NULL;
    }
  }

  /* Client verification mode */
  if (config->verify_peer == 2) {
    SSL_CTX_set_verify(ctx, SSL_VERIFY_PEER | SSL_VERIFY_FAIL_IF_NO_PEER_CERT,
                       NULL);
  } else if (config->verify_peer == 1) {
    SSL_CTX_set_verify(ctx, SSL_VERIFY_PEER, NULL);
  } else {
    SSL_CTX_set_verify(ctx, SSL_VERIFY_NONE, NULL);
  }

  /* Configure ALPN */
  SSL_CTX_set_alpn_select_cb(ctx, alpn_select_cb, NULL);

  return ctx;
}

void xHttpTlsCtxDestroyOpenSSL(void *ctx) {
  if (ctx) SSL_CTX_free((SSL_CTX *)ctx);
}

/* ───────────────────── Per-connection TLS state ───────────────────── */

typedef struct {
  SSL *ssl;
  int  fd;
  char alpn_result[16]; /**< Cached ALPN negotiation result */
} xHttpTlsOpenSSL_;

/* ───────────────────── Transport vtable callbacks ───────────────────── */

static ssize_t openssl_read(void *ctx, void *buf, size_t len) {
  xHttpTlsOpenSSL_ *t = (xHttpTlsOpenSSL_ *)ctx;
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
  xHttpTlsOpenSSL_ *t     = (xHttpTlsOpenSSL_ *)ctx;
  ssize_t           total = 0;

  for (int i = 0; i < iovcnt; i++) {
    if (iov[i].iov_len == 0) continue;

    ERR_clear_error();
    int n = SSL_write(t->ssl, iov[i].iov_base, (int)iov[i].iov_len);
    if (n > 0) {
      total += n;
      if ((size_t)n < iov[i].iov_len) {
        /* Partial write: return what we have so far */
        break;
      }
      continue;
    }

    int err = SSL_get_error(t->ssl, n);
    if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE) {
      if (total > 0) break; /* Return what we've written so far */
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
  xHttpTlsOpenSSL_ *t = (xHttpTlsOpenSSL_ *)ctx;
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

static const char *openssl_alpn(void *ctx) {
  xHttpTlsOpenSSL_ *t = (xHttpTlsOpenSSL_ *)ctx;
  if (t->alpn_result[0] == '\0') return NULL;
  return t->alpn_result;
}

static void openssl_destroy(void *ctx) {
  xHttpTlsOpenSSL_ *t = (xHttpTlsOpenSSL_ *)ctx;
  if (t->ssl) {
    /* Prevent SSL_free from closing the fd.  The fd is owned by
     * xSocket and will be closed by xSocketDestroy() after this
     * function returns (see xHttpConnClose).  Without this,
     * SSL_free's internal BIO_free closes the fd, and then
     * xSocketDestroy closes it again (double-close → SEGFAULT or
     * closing an unrelated fd). */
    BIO *rbio = SSL_get_rbio(t->ssl);
    if (rbio) BIO_set_close(rbio, BIO_NOCLOSE);
    BIO *wbio = SSL_get_wbio(t->ssl);
    if (wbio && wbio != rbio) BIO_set_close(wbio, BIO_NOCLOSE);

    /* NOTE: We intentionally do NOT call SSL_shutdown() here.
     * While the fd is still open at this point, SSL_shutdown sends
     * a close_notify alert which requires a round-trip with the peer.
     * This adds latency and complexity (handling partial shutdowns).
     * The subsequent close(fd) in xSocketDestroy sends a TCP RST,
     * which is sufficient to signal the peer. */

    /* Clear the OpenSSL error queue for this thread before freeing
     * the SSL object. This prevents stale error state from interfering
     * with other SSL operations in the same thread. */
    ERR_clear_error();
    SSL_free(t->ssl);
  }
  free(t);
}

/* ───────────────────── Public API ───────────────────── */

void xHttpTlsTransportInitOpenSSL(xHttpTransport *transport, void *tls_ctx,
                                  int fd) {
  if (!transport || !tls_ctx) return;

  xHttpTlsOpenSSL_ *t = (xHttpTlsOpenSSL_ *)calloc(1, sizeof(xHttpTlsOpenSSL_));
  if (!t) {
    transport->read      = NULL;
    transport->writev    = NULL;
    transport->handshake = NULL;
    transport->alpn      = NULL;
    transport->destroy   = NULL;
    transport->ctx       = NULL;
    return;
  }

  SSL *ssl = SSL_new((SSL_CTX *)tls_ctx);
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
  t->fd             = fd;
  t->alpn_result[0] = '\0';

  transport->read      = openssl_read;
  transport->writev    = openssl_writev;
  transport->handshake = openssl_handshake;
  transport->alpn      = openssl_alpn;
  transport->destroy   = openssl_destroy;
  transport->ctx       = t;
}

#endif /* XK_HAS_OPENSSL */
