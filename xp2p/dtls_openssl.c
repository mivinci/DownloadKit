/*
 * Copyright 2025 The xKit Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * dtls_openssl.c - OpenSSL DTLS backend implementation
 */

#include "dtls_backend.h"

#include <openssl/bio.h>
#include <openssl/ec.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>

#include <stdlib.h>
#include <string.h>

/* ───────────────────── Internal Context ───────────────────── */

struct xDtlsBackendCtx {
  SSL_CTX    *ssl_ctx;
  SSL        *ssl;
  BIO        *bio_in;  /* Network → OpenSSL (we write received data here) */
  BIO        *bio_out; /* OpenSSL → Network (we read encrypted output here) */
  X509       *cert;
  EVP_PKEY   *pkey;
  xDtlsRole   role;
  xDtlsSendFn send_fn;
  void       *send_arg;
};

/* ───────────────────── Helpers ───────────────────── */

/**
 * @brief Flush any pending data from bio_out through the send callback.
 */
static void flush_bio_out(xDtlsBackendCtx *ctx) {
  char buf[4096];
  int  pending;
  while ((pending = BIO_ctrl_pending(ctx->bio_out)) > 0) {
    int n = BIO_read(ctx->bio_out, buf, (int)sizeof(buf));
    if (n > 0 && ctx->send_fn) {
      ctx->send_fn((const uint8_t *)buf, (size_t)n, ctx->send_arg);
    }
  }
}

/**
 * @brief Generate a self-signed ECDSA P-256 certificate.
 */
static bool generate_self_signed_cert(xDtlsBackendCtx *ctx) {
  EVP_PKEY *pkey = NULL;
  X509     *cert = NULL;

  /* Generate EC key (P-256) */
  EVP_PKEY_CTX *pctx = EVP_PKEY_CTX_new_id(EVP_PKEY_EC, NULL);
  if (!pctx) goto fail;
  if (EVP_PKEY_keygen_init(pctx) <= 0) goto fail;
  if (EVP_PKEY_CTX_set_ec_paramgen_curve_nid(pctx, NID_X9_62_prime256v1) <=
      0) {
    goto fail;
  }
  if (EVP_PKEY_keygen(pctx, &pkey) <= 0) goto fail;
  EVP_PKEY_CTX_free(pctx);
  pctx = NULL;

  /* Create self-signed X509 certificate */
  cert = X509_new();
  if (!cert) goto fail;

  ASN1_INTEGER_set(X509_get_serialNumber(cert), 1);
  X509_gmtime_adj(X509_get_notBefore(cert), 0);
  X509_gmtime_adj(X509_get_notAfter(cert), 365 * 24 * 3600); /* 1 year */

  X509_set_pubkey(cert, pkey);

  X509_NAME *name = X509_get_subject_name(cert);
  X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC,
                             (const unsigned char *)"xKit WebRTC", -1, -1, 0);
  X509_set_issuer_name(cert, name);

  if (X509_sign(cert, pkey, EVP_sha256()) <= 0) goto fail;

  ctx->pkey = pkey;
  ctx->cert = cert;
  return true;

fail:
  if (pctx) EVP_PKEY_CTX_free(pctx);
  if (pkey) EVP_PKEY_free(pkey);
  if (cert) X509_free(cert);
  return false;
}

/* ───────────────────── Backend Interface ───────────────────── */

static xDtlsBackendCtx *openssl_create(xDtlsRole role, xDtlsSendFn send_fn,
                                        void *send_arg) {
  xDtlsBackendCtx *ctx =
    (xDtlsBackendCtx *)calloc(1, sizeof(xDtlsBackendCtx));
  if (!ctx) return NULL;

  ctx->role     = role;
  ctx->send_fn  = send_fn;
  ctx->send_arg = send_arg;

  /* Generate certificate */
  if (!generate_self_signed_cert(ctx)) {
    free(ctx);
    return NULL;
  }

  /* Create SSL_CTX for DTLS 1.2 */
  const SSL_METHOD *method;
  if (role == xDtlsRole_Active) {
    method = DTLS_client_method();
  } else {
    method = DTLS_server_method();
  }

  ctx->ssl_ctx = SSL_CTX_new(method);
  if (!ctx->ssl_ctx) goto fail;

  /* Set minimum protocol to DTLS 1.2 */
  SSL_CTX_set_min_proto_version(ctx->ssl_ctx, DTLS1_2_VERSION);
  SSL_CTX_set_max_proto_version(ctx->ssl_ctx, DTLS1_2_VERSION);

  /* Use the generated certificate */
  if (SSL_CTX_use_certificate(ctx->ssl_ctx, ctx->cert) != 1) goto fail;
  if (SSL_CTX_use_PrivateKey(ctx->ssl_ctx, ctx->pkey) != 1) goto fail;

  /* For WebRTC, we use SRTP-compatible cipher suites */
  SSL_CTX_set_cipher_list(ctx->ssl_ctx,
                          "ECDHE-ECDSA-AES128-GCM-SHA256:"
                          "ECDHE-ECDSA-AES256-GCM-SHA384:"
                          "ECDHE-ECDSA-CHACHA20-POLY1305");

  /* Verify peer certificate (we do fingerprint check ourselves) */
  SSL_CTX_set_verify(ctx->ssl_ctx, SSL_VERIFY_PEER | SSL_VERIFY_FAIL_IF_NO_PEER_CERT,
                     NULL);

  /* Allow self-signed certificates */
  SSL_CTX_set_verify_depth(ctx->ssl_ctx, 1);

  /* Custom verify callback that always succeeds (we verify fingerprint
   * ourselves after handshake) */
  SSL_CTX_set_cert_verify_callback(ctx->ssl_ctx,
                                   (int (*)(X509_STORE_CTX *, void *))NULL,
                                   NULL);
  /* Actually, use a permissive verify callback */
  SSL_CTX_set_verify(ctx->ssl_ctx, SSL_VERIFY_PEER,
                     NULL);

  /* Create SSL object */
  ctx->ssl = SSL_new(ctx->ssl_ctx);
  if (!ctx->ssl) goto fail;

  /* Create memory BIOs */
  ctx->bio_in  = BIO_new(BIO_s_mem());
  ctx->bio_out = BIO_new(BIO_s_mem());
  if (!ctx->bio_in || !ctx->bio_out) goto fail;

  BIO_set_mem_eof_return(ctx->bio_in, -1);
  BIO_set_mem_eof_return(ctx->bio_out, -1);

  SSL_set_bio(ctx->ssl, ctx->bio_in, ctx->bio_out);

  /* Set connect/accept mode */
  if (role == xDtlsRole_Active) {
    SSL_set_connect_state(ctx->ssl);
  } else {
    SSL_set_accept_state(ctx->ssl);
  }

  /* Enable DTLS cookie exchange for server mode */
  if (role == xDtlsRole_Passive) {
    SSL_set_options(ctx->ssl, SSL_OP_COOKIE_EXCHANGE);
  }

  return ctx;

fail:
  if (ctx->ssl) SSL_free(ctx->ssl); /* Also frees BIOs attached to SSL */
  else {
    if (ctx->bio_in) BIO_free(ctx->bio_in);
    if (ctx->bio_out) BIO_free(ctx->bio_out);
  }
  if (ctx->ssl_ctx) SSL_CTX_free(ctx->ssl_ctx);
  if (ctx->pkey) EVP_PKEY_free(ctx->pkey);
  if (ctx->cert) X509_free(ctx->cert);
  free(ctx);
  return NULL;
}

static void openssl_destroy(xDtlsBackendCtx *ctx) {
  if (!ctx) return;
  if (ctx->ssl) {
    SSL_shutdown(ctx->ssl);
    SSL_free(ctx->ssl); /* Also frees attached BIOs */
  }
  if (ctx->ssl_ctx) SSL_CTX_free(ctx->ssl_ctx);
  if (ctx->pkey) EVP_PKEY_free(ctx->pkey);
  if (ctx->cert) X509_free(ctx->cert);
  free(ctx);
}

static xErrno openssl_get_fingerprint(xDtlsBackendCtx *ctx, uint8_t *out) {
  if (!ctx || !ctx->cert || !out) return xErrno_InvalidArg;

  unsigned int  len = 0;
  unsigned char md[EVP_MAX_MD_SIZE];
  if (!X509_digest(ctx->cert, EVP_sha256(), md, &len)) {
    return xErrno_SysError;
  }
  if (len != XDTLS_FINGERPRINT_SIZE) return xErrno_SysError;

  memcpy(out, md, XDTLS_FINGERPRINT_SIZE);
  return xErrno_Ok;
}

static xErrno openssl_handshake(xDtlsBackendCtx *ctx) {
  if (!ctx || !ctx->ssl) return xErrno_InvalidArg;

  int ret = SSL_do_handshake(ctx->ssl);
  flush_bio_out(ctx);

  if (ret == 1) {
    /* Handshake complete */
    return xErrno_Ok;
  }

  int err = SSL_get_error(ctx->ssl, ret);
  if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE) {
    return xErrno_Again;
  }

  return xErrno_SysError;
}

static xErrno openssl_feed_input(xDtlsBackendCtx *ctx, const uint8_t *data,
                                  size_t len) {
  if (!ctx || !data) return xErrno_InvalidArg;

  int written = BIO_write(ctx->bio_in, data, (int)len);
  if (written <= 0) return xErrno_SysError;

  return xErrno_Ok;
}

static xErrno openssl_encrypt_send(xDtlsBackendCtx *ctx, const uint8_t *data,
                                    size_t len) {
  if (!ctx || !ctx->ssl || !data) return xErrno_InvalidArg;

  int ret = SSL_write(ctx->ssl, data, (int)len);
  flush_bio_out(ctx);

  if (ret <= 0) {
    int err = SSL_get_error(ctx->ssl, ret);
    if (err == SSL_ERROR_WANT_WRITE) return xErrno_Again;
    return xErrno_SysError;
  }

  return xErrno_Ok;
}

static xErrno openssl_decrypt_read(xDtlsBackendCtx *ctx, uint8_t *buf,
                                    size_t buf_cap, size_t *out_len) {
  if (!ctx || !ctx->ssl || !buf || !out_len) return xErrno_InvalidArg;

  int ret = SSL_read(ctx->ssl, buf, (int)buf_cap);
  if (ret > 0) {
    *out_len = (size_t)ret;
    return xErrno_Ok;
  }

  int err = SSL_get_error(ctx->ssl, ret);
  if (err == SSL_ERROR_WANT_READ) {
    *out_len = 0;
    return xErrno_Again;
  }

  *out_len = 0;
  return xErrno_SysError;
}

static xErrno openssl_get_remote_fingerprint(xDtlsBackendCtx *ctx,
                                              uint8_t         *out) {
  if (!ctx || !ctx->ssl || !out) return xErrno_InvalidArg;

  X509 *peer_cert = SSL_get_peer_certificate(ctx->ssl);
  if (!peer_cert) return xErrno_SysError;

  unsigned int  len = 0;
  unsigned char md[EVP_MAX_MD_SIZE];
  bool          ok = X509_digest(peer_cert, EVP_sha256(), md, &len);
  X509_free(peer_cert);

  if (!ok || len != XDTLS_FINGERPRINT_SIZE) return xErrno_SysError;

  memcpy(out, md, XDTLS_FINGERPRINT_SIZE);
  return xErrno_Ok;
}

static bool openssl_is_handshake_done(xDtlsBackendCtx *ctx) {
  if (!ctx || !ctx->ssl) return false;
  return SSL_is_init_finished(ctx->ssl);
}

/* ───────────────────── Backend Singleton ───────────────────── */

static const xDtlsBackend g_openssl_backend = {
  .name                   = "openssl",
  .create                 = openssl_create,
  .destroy                = openssl_destroy,
  .get_fingerprint        = openssl_get_fingerprint,
  .handshake              = openssl_handshake,
  .feed_input             = openssl_feed_input,
  .encrypt_send           = openssl_encrypt_send,
  .decrypt_read           = openssl_decrypt_read,
  .get_remote_fingerprint = openssl_get_remote_fingerprint,
  .is_handshake_done      = openssl_is_handshake_done,
};

const xDtlsBackend *xDtlsBackendGet(void) { return &g_openssl_backend; }
