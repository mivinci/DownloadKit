/*
 * Copyright 2025 The xKit Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * tls_openssl.c - OpenSSL TLS context management
 *
 * Implements xTlsCtxCreate / xTlsCtxDestroy / xTlsCtxReload /
 * xTlsCtxGetNative for the OpenSSL backend.
 */

#ifdef XK_HAS_OPENSSL

#include "tls_private.h"

#include <openssl/err.h>
#include <openssl/ssl.h>

#include <stdlib.h>
#include <string.h>
#include <xbase/log.h>

/* ═══════════════════════════════════════════════════════════════════
 *  ALPN callback (server-side, parameterized)
 * ═══════════════════════════════════════════════════════════════════
 */

/**
 * Internal state that wraps SSL_CTX and the wire-encoded ALPN list.
 */
XDEF_STRUCT(xTlsCtxOpenSSL_) {
  SSL_CTX       *ssl_ctx;
  unsigned char *alpn_wire; /**< Wire-encoded ALPN list, or NULL */
  size_t         alpn_wire_len;
  int            is_server; /**< Non-zero if server mode */
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
 *  TLS context management (server + client)
 * ═══════════════════════════════════════════════════════════════════
 */

xTlsCtx xTlsCtxCreate(const xTlsConf *conf) {
  if (!conf) return NULL;

  /* Determine mode: server if cert+key provided, client otherwise */
  int is_server = (conf->cert && conf->key) ? 1 : 0;

  SSL_CTX *ssl_ctx =
    SSL_CTX_new(is_server ? TLS_server_method() : TLS_client_method());
  if (!ssl_ctx) {
    xLog(false, "xnet: SSL_CTX_new failed");
    return NULL;
  }

  /* Set minimum TLS version to 1.2 */
  SSL_CTX_set_min_proto_version(ssl_ctx, TLS1_2_VERSION);

  if (is_server) {
    /* ── Server mode ── */

    /* Load certificate */
    if (SSL_CTX_use_certificate_chain_file(ssl_ctx, conf->cert) != 1) {
      xLog(false, "xnet: failed to load certificate: %s", conf->cert);
      goto fail;
    }

    /* Load private key */
    if (SSL_CTX_use_PrivateKey_file(ssl_ctx, conf->key, SSL_FILETYPE_PEM) !=
        1) {
      xLog(false, "xnet: failed to load private key: %s", conf->key);
      goto fail;
    }

    /* Verify private key matches certificate */
    if (SSL_CTX_check_private_key(ssl_ctx) != 1) {
      xLog(false, "xnet: private key does not match certificate");
      goto fail;
    }

    /* Load CA certificate for client verification (optional) */
    if (conf->ca) {
      if (SSL_CTX_load_verify_locations(ssl_ctx, conf->ca, NULL) != 1) {
        xLog(false, "xnet: failed to load CA certificate: %s", conf->ca);
        goto fail;
      }
    }

    /* Peer verification mode */
    if (conf->skip_verify) {
      SSL_CTX_set_verify(ssl_ctx, SSL_VERIFY_NONE, NULL);
    } else {
      SSL_CTX_set_verify(
        ssl_ctx, SSL_VERIFY_PEER | SSL_VERIFY_FAIL_IF_NO_PEER_CERT, NULL);
    }
  } else {
    /* ── Client mode ── */

    if (!conf->skip_verify) {
      /* Load CA certificates */
      if (conf->ca) {
        if (SSL_CTX_load_verify_locations(ssl_ctx, conf->ca, NULL) != 1) {
          xLog(false, "xnet: failed to load CA: %s", conf->ca);
          goto fail;
        }
      } else {
        /* Use system default CA store */
        SSL_CTX_set_default_verify_paths(ssl_ctx);
      }
      SSL_CTX_set_verify(ssl_ctx, SSL_VERIFY_PEER, NULL);
    } else {
      SSL_CTX_set_verify(ssl_ctx, SSL_VERIFY_NONE, NULL);
    }

    /* Load client certificate for mTLS (optional) */
    if (conf->cert) {
      if (SSL_CTX_use_certificate_chain_file(ssl_ctx, conf->cert) != 1) {
        xLog(false, "xnet: failed to load client cert: %s", conf->cert);
        goto fail;
      }
    }
    if (conf->key) {
      if (SSL_CTX_use_PrivateKey_file(ssl_ctx, conf->key, SSL_FILETYPE_PEM) !=
          1) {
        xLog(false, "xnet: failed to load client key: %s", conf->key);
        goto fail;
      }
    }
  }

  /* Allocate wrapper */
  xTlsCtxOpenSSL_ *ctx = (xTlsCtxOpenSSL_ *)calloc(1, sizeof(xTlsCtxOpenSSL_));
  if (!ctx) goto fail;
  ctx->ssl_ctx       = ssl_ctx;
  ctx->alpn_wire     = NULL;
  ctx->alpn_wire_len = 0;
  ctx->is_server     = is_server;

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
    if (is_server) {
      /* Server: register ALPN selection callback */
      SSL_CTX_set_alpn_select_cb(ssl_ctx, alpn_select_cb, ctx);
    } else {
      /* Client: advertise ALPN protocols */
      SSL_CTX_set_alpn_protos(ssl_ctx, ctx->alpn_wire,
                              (unsigned int)ctx->alpn_wire_len);
    }
  }

  return (xTlsCtx)ctx;

fail:
  SSL_CTX_free(ssl_ctx);
  return NULL;
}

void xTlsCtxDestroy(xTlsCtx raw) {
  if (!raw) return;
  xTlsCtxOpenSSL_ *ctx = (xTlsCtxOpenSSL_ *)raw;
  if (ctx->ssl_ctx) SSL_CTX_free(ctx->ssl_ctx);
  free(ctx->alpn_wire);
  free(ctx);
}

int xTlsCtxReload(xTlsCtx raw, const xTlsConf *conf) {
  if (!raw || !conf || !conf->cert || !conf->key) return -1;

  xTlsCtxOpenSSL_ *ctx = (xTlsCtxOpenSSL_ *)raw;
  SSL_CTX *ssl_ctx = ctx->ssl_ctx;

  /* Load new certificate */
  if (SSL_CTX_use_certificate_chain_file(ssl_ctx, conf->cert) != 1) {
    xLog(false, "xnet: reload: failed to load certificate: %s", conf->cert);
    return -1;
  }

  /* Load new private key */
  if (SSL_CTX_use_PrivateKey_file(ssl_ctx, conf->key, SSL_FILETYPE_PEM) != 1) {
    xLog(false, "xnet: reload: failed to load private key: %s", conf->key);
    return -1;
  }

  /* Verify private key matches certificate */
  if (SSL_CTX_check_private_key(ssl_ctx) != 1) {
    xLog(false, "xnet: reload: private key does not match certificate");
    return -1;
  }

  /* Reload CA certificate (optional) */
  if (conf->ca) {
    if (SSL_CTX_load_verify_locations(ssl_ctx, conf->ca, NULL) != 1) {
      xLog(false, "xnet: reload: failed to load CA certificate: %s", conf->ca);
      return -1;
    }
  }

  /* Update verification mode */
  if (conf->skip_verify) {
    SSL_CTX_set_verify(ssl_ctx, SSL_VERIFY_NONE, NULL);
  } else {
    SSL_CTX_set_verify(ssl_ctx,
                       SSL_VERIFY_PEER | SSL_VERIFY_FAIL_IF_NO_PEER_CERT, NULL);
  }

  /* Update ALPN if provided */
  if (conf->alpn) {
    size_t total = 0;
    for (const char **p = conf->alpn; *p; p++) {
      size_t slen = strlen(*p);
      if (slen > 255) continue;
      total += 1 + slen;
    }
    if (total > 0) {
      unsigned char *new_wire = (unsigned char *)malloc(total);
      if (new_wire) {
        unsigned char *dst = new_wire;
        for (const char **p = conf->alpn; *p; p++) {
          size_t slen = strlen(*p);
          if (slen > 255) continue;
          *dst++ = (unsigned char)slen;
          memcpy(dst, *p, slen);
          dst += slen;
        }
        free(ctx->alpn_wire);
        ctx->alpn_wire     = new_wire;
        ctx->alpn_wire_len = total;
      }
    }
  }

  return 0;
}

void *xTlsCtxGetNative(xTlsCtx raw) {
  if (!raw) return NULL;
  xTlsCtxOpenSSL_ *ctx = (xTlsCtxOpenSSL_ *)raw;
  return ctx->ssl_ctx;
}

int xTlsCtxIsServer(xTlsCtx raw) {
  if (!raw) return 0;
  xTlsCtxOpenSSL_ *ctx = (xTlsCtxOpenSSL_ *)raw;
  return ctx->is_server;
}

#endif /* XK_HAS_OPENSSL */
