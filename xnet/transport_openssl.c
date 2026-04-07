/*
 * Copyright 2025 The xKit Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * transport_openssl.c - OpenSSL TLS transport implementation
 *
 * Provides both server-side and client-side TLS transport using OpenSSL.
 * ALPN is parameterized (not hardcoded) for the server context.
 */

#ifdef XK_HAS_OPENSSL

#include "transport_private.h"

#include <openssl/err.h>
#include <openssl/ssl.h>
#include <openssl/x509v3.h>

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/uio.h>
#include <xbase/log.h>

/* ═══════════════════════════════════════════════════════════════════
 *  ALPN callback (server-side, parameterized)
 * ═══════════════════════════════════════════════════════════════════
 */

/**
 * Server-level state that wraps SSL_CTX and the wire-encoded ALPN list.
 */
XDEF_STRUCT(xTlsCtxOpenSSL_) {
  SSL_CTX       *ssl_ctx;
  unsigned char *alpn_wire; /**< Wire-encoded ALPN list, or NULL */
  size_t         alpn_wire_len;
};

static int alpn_select_cb(SSL *ssl, const unsigned char **out,
                          unsigned char *outlen, const unsigned char *in,
                          unsigned int inlen, void *arg) {
  (void)ssl;
  xTlsCtxOpenSSL_ *ctx = (xTlsCtxOpenSSL_ *)arg;

  if (!ctx->alpn_wire || ctx->alpn_wire_len == 0) return SSL_TLSEXT_ERR_NOACK;

  if (SSL_select_next_proto((unsigned char **)out, outlen, ctx->alpn_wire,
                            (unsigned int)ctx->alpn_wire_len, in,
                            inlen) != OPENSSL_NPN_NEGOTIATED) {
    return SSL_TLSEXT_ERR_NOACK;
  }
  return SSL_TLSEXT_ERR_OK;
}

/* ═══════════════════════════════════════════════════════════════════
 *  TLS context management (server-level)
 * ═══════════════════════════════════════════════════════════════════
 */

void *xTlsCtxCreate(const xTlsServerConf *conf) {
  if (!conf || !conf->cert || !conf->key) return NULL;

  SSL_CTX *ssl_ctx = SSL_CTX_new(TLS_server_method());
  if (!ssl_ctx) {
    xLog(false, "xnet: SSL_CTX_new failed");
    return NULL;
  }

  /* Set minimum TLS version to 1.2 */
  SSL_CTX_set_min_proto_version(ssl_ctx, TLS1_2_VERSION);

  /* Load certificate */
  if (SSL_CTX_use_certificate_chain_file(ssl_ctx, conf->cert) != 1) {
    xLog(false, "xnet: failed to load certificate: %s", conf->cert);
    SSL_CTX_free(ssl_ctx);
    return NULL;
  }

  /* Load private key */
  if (SSL_CTX_use_PrivateKey_file(ssl_ctx, conf->key, SSL_FILETYPE_PEM) != 1) {
    xLog(false, "xnet: failed to load private key: %s", conf->key);
    SSL_CTX_free(ssl_ctx);
    return NULL;
  }

  /* Verify private key matches certificate */
  if (SSL_CTX_check_private_key(ssl_ctx) != 1) {
    xLog(false, "xnet: private key does not match certificate");
    SSL_CTX_free(ssl_ctx);
    return NULL;
  }

  /* Load CA certificate for client verification (optional) */
  if (conf->ca) {
    if (SSL_CTX_load_verify_locations(ssl_ctx, conf->ca, NULL) != 1) {
      xLog(false, "xnet: failed to load CA certificate: %s", conf->ca);
      SSL_CTX_free(ssl_ctx);
      return NULL;
    }
  }

  /* Client verification mode */
  if (conf->verify_peer == 2) {
    SSL_CTX_set_verify(ssl_ctx,
                       SSL_VERIFY_PEER | SSL_VERIFY_FAIL_IF_NO_PEER_CERT, NULL);
  } else if (conf->verify_peer == 1) {
    SSL_CTX_set_verify(ssl_ctx, SSL_VERIFY_PEER, NULL);
  } else {
    SSL_CTX_set_verify(ssl_ctx, SSL_VERIFY_NONE, NULL);
  }

  /* Allocate wrapper */
  xTlsCtxOpenSSL_ *ctx = (xTlsCtxOpenSSL_ *)calloc(1, sizeof(xTlsCtxOpenSSL_));
  if (!ctx) {
    SSL_CTX_free(ssl_ctx);
    return NULL;
  }
  ctx->ssl_ctx       = ssl_ctx;
  ctx->alpn_wire     = NULL;
  ctx->alpn_wire_len = 0;

  /* Configure ALPN (parameterized) */
  if (conf->alpn) {
    /* Calculate wire-encoded length */
    size_t total = 0;
    for (const char **p = conf->alpn; *p; p++) {
      size_t slen = strlen(*p);
      if (slen > 255) continue; /* Skip invalid entries */
      total += 1 + slen;
    }
    if (total > 0) {
      ctx->alpn_wire = (unsigned char *)malloc(total);
      if (ctx->alpn_wire) {
        ctx->alpn_wire_len = total;
        unsigned char *dst = ctx->alpn_wire;
        for (const char **p = conf->alpn; *p; p++) {
          size_t slen = strlen(*p);
          if (slen > 255) continue;
          *dst++ = (unsigned char)slen;
          memcpy(dst, *p, slen);
          dst += slen;
        }
      }
    }
    SSL_CTX_set_alpn_select_cb(ssl_ctx, alpn_select_cb, ctx);
  }

  return ctx;
}

void xTlsCtxDestroy(void *raw) {
  if (!raw) return;
  xTlsCtxOpenSSL_ *ctx = (xTlsCtxOpenSSL_ *)raw;
  if (ctx->ssl_ctx) SSL_CTX_free(ctx->ssl_ctx);
  free(ctx->alpn_wire);
  free(ctx);
}

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

void xTransportTlsServerInit(xTransport *transport, void *tls_ctx, int fd) {
  if (!transport || !tls_ctx) return;

  xTlsCtxOpenSSL_ *server_ctx = (xTlsCtxOpenSSL_ *)tls_ctx;

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

  SSL *ssl = SSL_new(server_ctx->ssl_ctx);
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

int xTransportTlsClientInit(xTransport *transport, const xTlsClientConf *conf,
                            const char *hostname, int fd) {
  if (!transport) return -1;

  /* Create per-connection SSL_CTX (client method) */
  SSL_CTX *ctx = SSL_CTX_new(TLS_client_method());
  if (!ctx) {
    xLog(false, "xnet: SSL_CTX_new(client) failed");
    return -1;
  }

  SSL_CTX_set_min_proto_version(ctx, TLS1_2_VERSION);

  int skip_verify = conf ? conf->skip_verify : 0;

  if (!skip_verify) {
    /* Load CA certificates */
    if (conf && conf->ca) {
      if (SSL_CTX_load_verify_locations(ctx, conf->ca, NULL) != 1) {
        xLog(false, "xnet: failed to load CA: %s", conf->ca);
        SSL_CTX_free(ctx);
        return -1;
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
    if (SSL_CTX_use_certificate_chain_file(ctx, conf->cert) != 1) {
      xLog(false, "xnet: failed to load client cert: %s", conf->cert);
      SSL_CTX_free(ctx);
      return -1;
    }
  }
  if (conf && conf->key) {
    if (SSL_CTX_use_PrivateKey_file(ctx, conf->key, SSL_FILETYPE_PEM) != 1) {
      xLog(false, "xnet: failed to load client key: %s", conf->key);
      SSL_CTX_free(ctx);
      return -1;
    }
  }

  /* Create SSL object */
  SSL *ssl = SSL_new(ctx);
  if (!ssl) {
    SSL_CTX_free(ctx);
    return -1;
  }

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
  xTlsOpenSSL_ *t = (xTlsOpenSSL_ *)calloc(1, sizeof(xTlsOpenSSL_));
  if (!t) {
    SSL_free(ssl);
    SSL_CTX_free(ctx);
    return -1;
  }

  t->ssl            = ssl;
  t->owned_ctx      = ctx; /* Client owns its SSL_CTX */
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

#endif /* XK_HAS_OPENSSL */
