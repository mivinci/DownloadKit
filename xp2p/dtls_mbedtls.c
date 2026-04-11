/*
 * Copyright 2025 The xKit Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * dtls_mbedtls.c - mbedTLS DTLS backend implementation
 */

#include "dtls_backend.h"

#include <mbedtls/ctr_drbg.h>
#include <mbedtls/entropy.h>
#include <mbedtls/error.h>
#include <mbedtls/pk.h>
#include <mbedtls/sha256.h>
#include <mbedtls/ssl.h>
#include <mbedtls/ssl_cookie.h>
#include <mbedtls/timing.h>
#include <mbedtls/x509_crt.h>

#include <stdlib.h>
#include <string.h>

/* ───────────────────── Ring Buffer for Network I/O ───────────────────── */

#define MBEDTLS_IO_BUF_SIZE 8192

typedef struct {
  uint8_t data[MBEDTLS_IO_BUF_SIZE];
  size_t  read_pos;
  size_t  write_pos;
  size_t  len;
} IoBuf;

static void iobuf_init(IoBuf *b) {
  b->read_pos  = 0;
  b->write_pos = 0;
  b->len       = 0;
}

static int iobuf_write(IoBuf *b, const uint8_t *data, size_t len) {
  if (len > MBEDTLS_IO_BUF_SIZE - b->len) return -1;
  for (size_t i = 0; i < len; i++) {
    b->data[b->write_pos] = data[i];
    b->write_pos          = (b->write_pos + 1) % MBEDTLS_IO_BUF_SIZE;
  }
  b->len += len;
  return (int)len;
}

static int iobuf_read(IoBuf *b, uint8_t *out, size_t cap) {
  if (b->len == 0) return MBEDTLS_ERR_SSL_WANT_READ;
  size_t to_read = (cap < b->len) ? cap : b->len;
  for (size_t i = 0; i < to_read; i++) {
    out[i]      = b->data[b->read_pos];
    b->read_pos = (b->read_pos + 1) % MBEDTLS_IO_BUF_SIZE;
  }
  b->len -= to_read;
  return (int)to_read;
}

/* ───────────────────── Internal Context ───────────────────── */

struct xDtlsBackendCtx {
  mbedtls_ssl_context      ssl;
  mbedtls_ssl_config       conf;
  mbedtls_x509_crt         cert;
  mbedtls_pk_context       pkey;
  mbedtls_entropy_context  entropy;
  mbedtls_ctr_drbg_context ctr_drbg;
  mbedtls_timing_delay_context timer;
  mbedtls_ssl_cookie_ctx   cookie;

  IoBuf       recv_buf; /* Network → mbedTLS */
  xDtlsRole   role;
  xDtlsSendFn send_fn;
  void       *send_arg;
  bool        handshake_done;
};

/* ───────────────────── Custom I/O Callbacks ───────────────────── */

static int mbedtls_send_cb(void *ctx_arg, const unsigned char *buf,
                           size_t len) {
  xDtlsBackendCtx *ctx = (xDtlsBackendCtx *)ctx_arg;
  if (ctx->send_fn) {
    xErrno err = ctx->send_fn((const uint8_t *)buf, len, ctx->send_arg);
    if (err != xErrno_Ok) return MBEDTLS_ERR_SSL_INTERNAL_ERROR;
  }
  return (int)len;
}

static int mbedtls_recv_cb(void *ctx_arg, unsigned char *buf, size_t len) {
  xDtlsBackendCtx *ctx = (xDtlsBackendCtx *)ctx_arg;
  return iobuf_read(&ctx->recv_buf, (uint8_t *)buf, len);
}

/* ───────────────────── Helpers ───────────────────── */

/**
 * @brief Generate a self-signed ECDSA P-256 certificate using mbedTLS.
 *
 * mbedTLS doesn't have a simple X509 write API in all versions, so we
 * generate a key pair and a self-signed certificate using the write API.
 */
static bool generate_self_signed_cert(xDtlsBackendCtx *ctx) {
  int ret;

  /* Generate ECDSA P-256 key */
  ret = mbedtls_pk_setup(&ctx->pkey,
                         mbedtls_pk_info_from_type(MBEDTLS_PK_ECKEY));
  if (ret != 0) return false;

  ret = mbedtls_ecp_gen_key(MBEDTLS_ECP_DP_SECP256R1,
                            mbedtls_pk_ec(ctx->pkey),
                            mbedtls_ctr_drbg_random, &ctx->ctr_drbg);
  if (ret != 0) return false;

  /* Create self-signed certificate */
  mbedtls_x509write_cert crt;
  mbedtls_x509write_crt_init(&crt);

  mbedtls_x509write_crt_set_version(&crt, MBEDTLS_X509_CRT_VERSION_3);
  mbedtls_x509write_crt_set_md_alg(&crt, MBEDTLS_MD_SHA256);
  mbedtls_x509write_crt_set_subject_key(&crt, &ctx->pkey);
  mbedtls_x509write_crt_set_issuer_key(&crt, &ctx->pkey);

  ret = mbedtls_x509write_crt_set_subject_name(&crt, "CN=xKit WebRTC");
  if (ret != 0) {
    mbedtls_x509write_crt_free(&crt);
    return false;
  }

  ret = mbedtls_x509write_crt_set_issuer_name(&crt, "CN=xKit WebRTC");
  if (ret != 0) {
    mbedtls_x509write_crt_free(&crt);
    return false;
  }

  /* Serial number */
  mbedtls_mpi serial;
  mbedtls_mpi_init(&serial);
  mbedtls_mpi_lset(&serial, 1);
  mbedtls_x509write_crt_set_serial(&crt, &serial);
  mbedtls_mpi_free(&serial);

  /* Validity */
  ret = mbedtls_x509write_crt_set_validity(&crt, "20250101000000",
                                            "20260101000000");
  if (ret != 0) {
    mbedtls_x509write_crt_free(&crt);
    return false;
  }

  /* Write DER certificate */
  uint8_t der_buf[XDTLS_MAX_CERT_SIZE];
  ret = mbedtls_x509write_crt_der(&crt, der_buf, sizeof(der_buf),
                                   mbedtls_ctr_drbg_random, &ctx->ctr_drbg);
  mbedtls_x509write_crt_free(&crt);

  if (ret <= 0) return false;

  /* Parse the DER certificate back into the cert structure.
   * mbedtls_x509write_crt_der writes from the END of the buffer. */
  int der_len = ret;
  ret = mbedtls_x509_crt_parse_der(&ctx->cert,
                                    der_buf + sizeof(der_buf) - der_len,
                                    (size_t)der_len);
  return (ret == 0);
}

/* ───────────────────── Backend Interface ───────────────────── */

static xDtlsBackendCtx *mbedtls_create(xDtlsRole role, xDtlsSendFn send_fn,
                                         void *send_arg) {
  xDtlsBackendCtx *ctx =
    (xDtlsBackendCtx *)calloc(1, sizeof(xDtlsBackendCtx));
  if (!ctx) return NULL;

  ctx->role     = role;
  ctx->send_fn  = send_fn;
  ctx->send_arg = send_arg;
  iobuf_init(&ctx->recv_buf);

  /* Initialize mbedTLS structures */
  mbedtls_ssl_init(&ctx->ssl);
  mbedtls_ssl_config_init(&ctx->conf);
  mbedtls_x509_crt_init(&ctx->cert);
  mbedtls_pk_init(&ctx->pkey);
  mbedtls_entropy_init(&ctx->entropy);
  mbedtls_ctr_drbg_init(&ctx->ctr_drbg);
  mbedtls_ssl_cookie_init(&ctx->cookie);

  /* Seed the DRBG */
  int ret = mbedtls_ctr_drbg_seed(&ctx->ctr_drbg, mbedtls_entropy_func,
                                   &ctx->entropy,
                                   (const unsigned char *)"xp2p_dtls", 9);
  if (ret != 0) goto fail;

  /* Generate certificate */
  if (!generate_self_signed_cert(ctx)) goto fail;

  /* Configure SSL */
  int endpoint = (role == xDtlsRole_Active) ? MBEDTLS_SSL_IS_CLIENT
                                             : MBEDTLS_SSL_IS_SERVER;
  ret = mbedtls_ssl_config_defaults(&ctx->conf, endpoint,
                                     MBEDTLS_SSL_TRANSPORT_DATAGRAM,
                                     MBEDTLS_SSL_PRESET_DEFAULT);
  if (ret != 0) goto fail;

  mbedtls_ssl_conf_rng(&ctx->conf, mbedtls_ctr_drbg_random, &ctx->ctr_drbg);

  /* Set minimum TLS version to DTLS 1.2 */
  mbedtls_ssl_conf_min_tls_version(&ctx->conf, MBEDTLS_SSL_VERSION_TLS1_2);
  mbedtls_ssl_conf_max_tls_version(&ctx->conf, MBEDTLS_SSL_VERSION_TLS1_2);

  /* Use our certificate */
  ret = mbedtls_ssl_conf_own_cert(&ctx->conf, &ctx->cert, &ctx->pkey);
  if (ret != 0) goto fail;

  /* Verify peer (we do fingerprint check ourselves) */
  mbedtls_ssl_conf_authmode(&ctx->conf, MBEDTLS_SSL_VERIFY_OPTIONAL);

  /* DTLS cookie for server */
  if (role == xDtlsRole_Passive) {
    ret = mbedtls_ssl_cookie_setup(&ctx->cookie, mbedtls_ctr_drbg_random,
                                    &ctx->ctr_drbg);
    if (ret != 0) goto fail;
    mbedtls_ssl_conf_dtls_cookies(&ctx->conf, mbedtls_ssl_cookie_write,
                                   mbedtls_ssl_cookie_check, &ctx->cookie);
  }

  /* Setup SSL context */
  ret = mbedtls_ssl_setup(&ctx->ssl, &ctx->conf);
  if (ret != 0) goto fail;

  /* Set I/O callbacks */
  mbedtls_ssl_set_bio(&ctx->ssl, ctx, mbedtls_send_cb, mbedtls_recv_cb, NULL);

  /* Set timer callbacks for DTLS retransmission */
  mbedtls_ssl_set_timer_cb(&ctx->ssl, &ctx->timer,
                            mbedtls_timing_set_delay,
                            mbedtls_timing_get_delay);

  return ctx;

fail:
  mbedtls_ssl_free(&ctx->ssl);
  mbedtls_ssl_config_free(&ctx->conf);
  mbedtls_x509_crt_free(&ctx->cert);
  mbedtls_pk_free(&ctx->pkey);
  mbedtls_ctr_drbg_free(&ctx->ctr_drbg);
  mbedtls_entropy_free(&ctx->entropy);
  mbedtls_ssl_cookie_free(&ctx->cookie);
  free(ctx);
  return NULL;
}

static void mbedtls_backend_destroy(xDtlsBackendCtx *ctx) {
  if (!ctx) return;
  mbedtls_ssl_free(&ctx->ssl);
  mbedtls_ssl_config_free(&ctx->conf);
  mbedtls_x509_crt_free(&ctx->cert);
  mbedtls_pk_free(&ctx->pkey);
  mbedtls_ctr_drbg_free(&ctx->ctr_drbg);
  mbedtls_entropy_free(&ctx->entropy);
  mbedtls_ssl_cookie_free(&ctx->cookie);
  free(ctx);
}

static xErrno mbedtls_get_fingerprint(xDtlsBackendCtx *ctx, uint8_t *out) {
  if (!ctx || !out) return xErrno_InvalidArg;
  if (ctx->cert.raw.len == 0) return xErrno_SysError;

  /* SHA-256 of the DER-encoded certificate */
  int ret = mbedtls_sha256(ctx->cert.raw.p, ctx->cert.raw.len, out, 0);
  return (ret == 0) ? xErrno_Ok : xErrno_SysError;
}

static xErrno mbedtls_backend_handshake(xDtlsBackendCtx *ctx) {
  if (!ctx) return xErrno_InvalidArg;

  int ret = mbedtls_ssl_handshake(&ctx->ssl);

  if (ret == 0) {
    ctx->handshake_done = true;
    return xErrno_Ok;
  }

  if (ret == MBEDTLS_ERR_SSL_WANT_READ ||
      ret == MBEDTLS_ERR_SSL_WANT_WRITE) {
    return xErrno_Again;
  }

  /* Hello verify request (DTLS cookie) — need to restart */
  if (ret == MBEDTLS_ERR_SSL_HELLO_VERIFY_REQUIRED) {
    mbedtls_ssl_session_reset(&ctx->ssl);
    return xErrno_Again;
  }

  return xErrno_SysError;
}

static xErrno mbedtls_backend_feed_input(xDtlsBackendCtx *ctx,
                                          const uint8_t *data, size_t len) {
  if (!ctx || !data) return xErrno_InvalidArg;

  int written = iobuf_write(&ctx->recv_buf, data, len);
  return (written > 0) ? xErrno_Ok : xErrno_SysError;
}

static xErrno mbedtls_backend_encrypt_send(xDtlsBackendCtx *ctx,
                                            const uint8_t *data, size_t len) {
  if (!ctx || !data) return xErrno_InvalidArg;

  int ret = mbedtls_ssl_write(&ctx->ssl, data, len);
  if (ret >= 0) return xErrno_Ok;
  if (ret == MBEDTLS_ERR_SSL_WANT_WRITE) return xErrno_Again;
  return xErrno_SysError;
}

static xErrno mbedtls_backend_decrypt_read(xDtlsBackendCtx *ctx, uint8_t *buf,
                                            size_t buf_cap, size_t *out_len) {
  if (!ctx || !buf || !out_len) return xErrno_InvalidArg;

  int ret = mbedtls_ssl_read(&ctx->ssl, buf, buf_cap);
  if (ret > 0) {
    *out_len = (size_t)ret;
    return xErrno_Ok;
  }

  if (ret == MBEDTLS_ERR_SSL_WANT_READ) {
    *out_len = 0;
    return xErrno_Again;
  }

  *out_len = 0;
  return xErrno_SysError;
}

static xErrno mbedtls_backend_get_remote_fingerprint(xDtlsBackendCtx *ctx,
                                                      uint8_t         *out) {
  if (!ctx || !out) return xErrno_InvalidArg;

  const mbedtls_x509_crt *peer = mbedtls_ssl_get_peer_cert(&ctx->ssl);
  if (!peer || peer->raw.len == 0) return xErrno_SysError;

  int ret = mbedtls_sha256(peer->raw.p, peer->raw.len, out, 0);
  return (ret == 0) ? xErrno_Ok : xErrno_SysError;
}

static bool mbedtls_backend_is_handshake_done(xDtlsBackendCtx *ctx) {
  if (!ctx) return false;
  return ctx->handshake_done;
}

/* ───────────────────── Backend Singleton ───────────────────── */

static const xDtlsBackend g_mbedtls_backend = {
  .name                   = "mbedtls",
  .create                 = mbedtls_create,
  .destroy                = mbedtls_backend_destroy,
  .get_fingerprint        = mbedtls_get_fingerprint,
  .handshake              = mbedtls_backend_handshake,
  .feed_input             = mbedtls_backend_feed_input,
  .encrypt_send           = mbedtls_backend_encrypt_send,
  .decrypt_read           = mbedtls_backend_decrypt_read,
  .get_remote_fingerprint = mbedtls_backend_get_remote_fingerprint,
  .is_handshake_done      = mbedtls_backend_is_handshake_done,
};

const xDtlsBackend *xDtlsBackendGet(void) { return &g_mbedtls_backend; }
