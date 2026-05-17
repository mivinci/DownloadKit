/*
 * Copyright 2025 The libx Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * dtls_mbedtls.c - mbedTLS DTLS backend implementation
 */

#include "dtls_backend.h"

/* mbedTLS 3.x+ provides build_info.h; mbedTLS 2.x uses version.h */
#if __has_include(<mbedtls/build_info.h>)
#include <mbedtls/build_info.h>
#else
#include <mbedtls/version.h>
#endif

#include <mbedtls/error.h>
#include <mbedtls/pk.h>
#include <mbedtls/ssl.h>
#include <mbedtls/ssl_cookie.h>
#include <mbedtls/timing.h>
#include <mbedtls/x509_crt.h>

#if MBEDTLS_VERSION_NUMBER < 0x04000000
/* mbedTLS 2.x/3.x: manual RNG management required */
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/entropy.h>
#include <mbedtls/sha256.h>
#else
/* mbedTLS 4.x: PSA Crypto API for key generation and hashing */
#include <psa/crypto.h>
#endif

/* mbedTLS version compatibility for min TLS version */
#if MBEDTLS_VERSION_NUMBER >= 0x04000000
/* mbedTLS 4.x: min/max version set via ssl_config_defaults preset */
#elif MBEDTLS_VERSION_NUMBER >= 0x03000000
/* mbedTLS 3.x */
#define XP2P_MBEDTLS_HAS_TLS_VERSION_API 1
#else
/* mbedTLS 2.x */
#define XP2P_MBEDTLS_HAS_TLS_VERSION_API 0
#endif

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
  mbedtls_ssl_context ssl;
  mbedtls_ssl_config  conf;
  mbedtls_x509_crt    cert;
  mbedtls_pk_context  pkey;
#if MBEDTLS_VERSION_NUMBER < 0x04000000
  mbedtls_entropy_context  entropy;
  mbedtls_ctr_drbg_context ctr_drbg;
#else
  mbedtls_svc_key_id_t psa_key_id; /* PSA key handle for 4.x */
#endif
  mbedtls_timing_delay_context timer;
  mbedtls_ssl_cookie_ctx       cookie;

  IoBuf       recv_buf; /* Network → mbedTLS */
  xDtlsRole   role;
  xDtlsSendFn send_fn;
  void       *send_arg;
  bool        handshake_done;
};

/* ───────────────────── Custom I/O Callbacks ───────────────────── */

static int mbedtls_send_cb(void *ctx_arg, const unsigned char *buf, size_t len) {
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

/* ───────────────────── SHA-256 Helper ───────────────────── */

/**
 * @brief Compute SHA-256 hash, compatible with mbedTLS 2.x/3.x/4.x.
 */
static int xp2p_sha256(const uint8_t *input, size_t len, uint8_t *output) {
#if MBEDTLS_VERSION_NUMBER >= 0x04000000
  /* mbedTLS 4.x: use PSA Crypto API */
  size_t       hash_len = 0;
  psa_status_t status   = psa_hash_compute(PSA_ALG_SHA_256, input, len, output, 32, &hash_len);
  return (status == PSA_SUCCESS && hash_len == 32) ? 0 : -1;
#elif MBEDTLS_VERSION_NUMBER >= 0x03000000
  /* mbedTLS 3.x: mbedtls_sha256 returns int */
  return mbedtls_sha256(input, len, output, 0);
#else
  /* mbedTLS 2.x: mbedtls_sha256 is deprecated (returns void),
   * use mbedtls_sha256_ret which returns int. */
  return mbedtls_sha256_ret(input, len, output, 0);
#endif
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

#if MBEDTLS_VERSION_NUMBER >= 0x04000000
  /* mbedTLS 4.x: use PSA Crypto API for key generation */
  psa_key_attributes_t attributes = PSA_KEY_ATTRIBUTES_INIT;
  psa_set_key_usage_flags(&attributes, PSA_KEY_USAGE_SIGN_HASH | PSA_KEY_USAGE_SIGN_MESSAGE |
                                         PSA_KEY_USAGE_EXPORT);
  psa_set_key_algorithm(&attributes, PSA_ALG_ECDSA(PSA_ALG_SHA_256));
  psa_set_key_type(&attributes, PSA_KEY_TYPE_ECC_KEY_PAIR(PSA_ECC_FAMILY_SECP_R1));
  psa_set_key_bits(&attributes, 256);

  psa_status_t status = psa_generate_key(&attributes, &ctx->psa_key_id);
  psa_reset_key_attributes(&attributes);
  if (status != PSA_SUCCESS) return false;

  /* Wrap PSA key into mbedtls_pk_context for x509write */
  ret = mbedtls_pk_copy_from_psa(ctx->psa_key_id, &ctx->pkey);
  if (ret != 0) return false;
#else
  /* mbedTLS 2.x/3.x: classic key generation */
  ret = mbedtls_pk_setup(&ctx->pkey, mbedtls_pk_info_from_type(MBEDTLS_PK_ECKEY));
  if (ret != 0) return false;

  ret = mbedtls_ecp_gen_key(MBEDTLS_ECP_DP_SECP256R1, mbedtls_pk_ec(ctx->pkey),
                            mbedtls_ctr_drbg_random, &ctx->ctr_drbg);
  if (ret != 0) return false;
#endif

  /* Create self-signed certificate */
  mbedtls_x509write_cert crt;
  mbedtls_x509write_crt_init(&crt);

  mbedtls_x509write_crt_set_version(&crt, MBEDTLS_X509_CRT_VERSION_3);
  mbedtls_x509write_crt_set_md_alg(&crt, MBEDTLS_MD_SHA256);
  mbedtls_x509write_crt_set_subject_key(&crt, &ctx->pkey);
  mbedtls_x509write_crt_set_issuer_key(&crt, &ctx->pkey);

  ret = mbedtls_x509write_crt_set_subject_name(&crt, "CN=moo WebRTC");
  if (ret != 0) {
    mbedtls_x509write_crt_free(&crt);
    return false;
  }

  ret = mbedtls_x509write_crt_set_issuer_name(&crt, "CN=moo WebRTC");
  if (ret != 0) {
    mbedtls_x509write_crt_free(&crt);
    return false;
  }

  /* Serial number */
#if MBEDTLS_VERSION_NUMBER >= 0x04000000
  const unsigned char serial_raw[] = {0x01};
  ret = mbedtls_x509write_crt_set_serial_raw(&crt, serial_raw, sizeof(serial_raw));
  if (ret != 0) {
    mbedtls_x509write_crt_free(&crt);
    return false;
  }
#else
  mbedtls_mpi serial;
  mbedtls_mpi_init(&serial);
  mbedtls_mpi_lset(&serial, 1);
  mbedtls_x509write_crt_set_serial(&crt, &serial);
  mbedtls_mpi_free(&serial);
#endif

  /* Validity */
  ret = mbedtls_x509write_crt_set_validity(&crt, "20250101000000", "20260101000000");
  if (ret != 0) {
    mbedtls_x509write_crt_free(&crt);
    return false;
  }

  /* Write DER certificate */
  uint8_t der_buf[XDTLS_MAX_CERT_SIZE];
#if MBEDTLS_VERSION_NUMBER >= 0x04000000
  /* mbedTLS 4.x: f_rng/p_rng parameters removed */
  ret = mbedtls_x509write_crt_der(&crt, der_buf, sizeof(der_buf));
#else
  ret = mbedtls_x509write_crt_der(&crt, der_buf, sizeof(der_buf), mbedtls_ctr_drbg_random,
                                  &ctx->ctr_drbg);
#endif
  mbedtls_x509write_crt_free(&crt);

  if (ret <= 0) return false;

  /* Parse the DER certificate back into the cert structure.
   * mbedtls_x509write_crt_der writes from the END of the buffer. */
  int der_len = ret;
  ret =
    mbedtls_x509_crt_parse_der(&ctx->cert, der_buf + sizeof(der_buf) - der_len, (size_t)der_len);
  return (ret == 0);
}

/* ───────────────────── Backend Interface ───────────────────── */

static xDtlsBackendCtx *mbedtls_create(xDtlsRole role, xDtlsSendFn send_fn, void *send_arg) {
  xDtlsBackendCtx *ctx = (xDtlsBackendCtx *)calloc(1, sizeof(xDtlsBackendCtx));
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
  mbedtls_ssl_cookie_init(&ctx->cookie);

#if MBEDTLS_VERSION_NUMBER >= 0x04000000
  /* mbedTLS 4.x: initialize PSA Crypto subsystem */
  psa_status_t psa_ret = psa_crypto_init();
  if (psa_ret != PSA_SUCCESS) goto fail;
  ctx->psa_key_id = MBEDTLS_SVC_KEY_ID_INIT;
#else
  /* mbedTLS 2.x/3.x: manual RNG management */
  mbedtls_entropy_init(&ctx->entropy);
  mbedtls_ctr_drbg_init(&ctx->ctr_drbg);

  /* Seed the DRBG */
  {
    int seed_ret = mbedtls_ctr_drbg_seed(&ctx->ctr_drbg, mbedtls_entropy_func, &ctx->entropy,
                                         (const unsigned char *)"xp2p_dtls", 9);
    if (seed_ret != 0) goto fail;
  }
#endif

  /* Generate certificate */
  if (!generate_self_signed_cert(ctx)) goto fail;

  /* Configure SSL */
  int endpoint = (role == xDtlsRole_Active) ? MBEDTLS_SSL_IS_CLIENT : MBEDTLS_SSL_IS_SERVER;
  {
    int cfg_ret = mbedtls_ssl_config_defaults(&ctx->conf, endpoint, MBEDTLS_SSL_TRANSPORT_DATAGRAM,
                                              MBEDTLS_SSL_PRESET_DEFAULT);
    if (cfg_ret != 0) goto fail;
  }

#if MBEDTLS_VERSION_NUMBER < 0x04000000
  /* mbedTLS 2.x/3.x: explicit RNG required */
  mbedtls_ssl_conf_rng(&ctx->conf, mbedtls_ctr_drbg_random, &ctx->ctr_drbg);
#endif
  /* mbedTLS 4.x: RNG is handled internally via PSA Crypto */

#if defined(XP2P_MBEDTLS_HAS_TLS_VERSION_API) && XP2P_MBEDTLS_HAS_TLS_VERSION_API
  /* mbedTLS 3.x: set min/max TLS version to DTLS 1.2 */
  mbedtls_ssl_conf_min_tls_version(&ctx->conf, MBEDTLS_SSL_VERSION_TLS1_2);
  mbedtls_ssl_conf_max_tls_version(&ctx->conf, MBEDTLS_SSL_VERSION_TLS1_2);
#elif MBEDTLS_VERSION_NUMBER < 0x03000000
  /* mbedTLS 2.x */
  mbedtls_ssl_conf_min_version(&ctx->conf, MBEDTLS_SSL_MAJOR_VERSION_3,
                               MBEDTLS_SSL_MINOR_VERSION_3);
  mbedtls_ssl_conf_max_version(&ctx->conf, MBEDTLS_SSL_MAJOR_VERSION_3,
                               MBEDTLS_SSL_MINOR_VERSION_3);
#endif
  /* mbedTLS 4.x: DTLS 1.2 is the default via MBEDTLS_SSL_PRESET_DEFAULT */

  /* Use our certificate */
  {
    int own_ret = mbedtls_ssl_conf_own_cert(&ctx->conf, &ctx->cert, &ctx->pkey);
    if (own_ret != 0) goto fail;
  }

  /* Verify peer (we do fingerprint check ourselves) */
  mbedtls_ssl_conf_authmode(&ctx->conf, MBEDTLS_SSL_VERIFY_OPTIONAL);

  /* Note: DTLS cookie exchange is NOT enabled here because in WebRTC,
   * DTLS runs on top of an already-authenticated ICE connection.
   * ICE provides the DoS protection that cookies would otherwise offer. */
  mbedtls_ssl_conf_dtls_cookies(&ctx->conf, NULL, NULL, NULL);

  /* Setup SSL context */
  {
    int setup_ret = mbedtls_ssl_setup(&ctx->ssl, &ctx->conf);
    if (setup_ret != 0) goto fail;
  }

  /* Set I/O callbacks */
  mbedtls_ssl_set_bio(&ctx->ssl, ctx, mbedtls_send_cb, mbedtls_recv_cb, NULL);

  /* Set timer callbacks for DTLS retransmission */
  mbedtls_ssl_set_timer_cb(&ctx->ssl, &ctx->timer, mbedtls_timing_set_delay,
                           mbedtls_timing_get_delay);

  return ctx;

fail:
  mbedtls_ssl_free(&ctx->ssl);
  mbedtls_ssl_config_free(&ctx->conf);
  mbedtls_x509_crt_free(&ctx->cert);
  mbedtls_pk_free(&ctx->pkey);
#if MBEDTLS_VERSION_NUMBER >= 0x04000000
  if (!mbedtls_svc_key_id_is_null(ctx->psa_key_id)) psa_destroy_key(ctx->psa_key_id);
#else
  mbedtls_ctr_drbg_free(&ctx->ctr_drbg);
  mbedtls_entropy_free(&ctx->entropy);
#endif
  mbedtls_ssl_cookie_free(&ctx->cookie);
  free(ctx);
  return NULL;
}

static xErrno mbedtls_backend_set_role(xDtlsBackendCtx *ctx, xDtlsRole role) {
  if (!ctx) return xErrno_InvalidArg;
  if (role == ctx->role) return xErrno_Ok;

  ctx->role = role;

  /* Tear down old SSL context but keep cert + pkey */
  mbedtls_ssl_free(&ctx->ssl);
  mbedtls_ssl_config_free(&ctx->conf);

  /* Re-initialize */
  mbedtls_ssl_init(&ctx->ssl);
  mbedtls_ssl_config_init(&ctx->conf);
  iobuf_init(&ctx->recv_buf);

  int endpoint = (role == xDtlsRole_Active) ? MBEDTLS_SSL_IS_CLIENT : MBEDTLS_SSL_IS_SERVER;
  int cfg_ret  = mbedtls_ssl_config_defaults(&ctx->conf, endpoint, MBEDTLS_SSL_TRANSPORT_DATAGRAM,
                                             MBEDTLS_SSL_PRESET_DEFAULT);
  if (cfg_ret != 0) return xErrno_SysError;

#if MBEDTLS_VERSION_NUMBER < 0x04000000
  mbedtls_ssl_conf_rng(&ctx->conf, mbedtls_ctr_drbg_random, &ctx->ctr_drbg);
#endif

#if defined(XP2P_MBEDTLS_HAS_TLS_VERSION_API) && XP2P_MBEDTLS_HAS_TLS_VERSION_API
  mbedtls_ssl_conf_min_tls_version(&ctx->conf, MBEDTLS_SSL_VERSION_TLS1_2);
  mbedtls_ssl_conf_max_tls_version(&ctx->conf, MBEDTLS_SSL_VERSION_TLS1_2);
#elif MBEDTLS_VERSION_NUMBER < 0x03000000
  mbedtls_ssl_conf_min_version(&ctx->conf, MBEDTLS_SSL_MAJOR_VERSION_3,
                               MBEDTLS_SSL_MINOR_VERSION_3);
  mbedtls_ssl_conf_max_version(&ctx->conf, MBEDTLS_SSL_MAJOR_VERSION_3,
                               MBEDTLS_SSL_MINOR_VERSION_3);
#endif

  int own_ret = mbedtls_ssl_conf_own_cert(&ctx->conf, &ctx->cert, &ctx->pkey);
  if (own_ret != 0) return xErrno_SysError;

  mbedtls_ssl_conf_authmode(&ctx->conf, MBEDTLS_SSL_VERIFY_OPTIONAL);
  mbedtls_ssl_conf_dtls_cookies(&ctx->conf, NULL, NULL, NULL);

  int setup_ret = mbedtls_ssl_setup(&ctx->ssl, &ctx->conf);
  if (setup_ret != 0) return xErrno_SysError;

  mbedtls_ssl_set_bio(&ctx->ssl, ctx, mbedtls_send_cb, mbedtls_recv_cb, NULL);
  mbedtls_ssl_set_timer_cb(&ctx->ssl, &ctx->timer, mbedtls_timing_set_delay,
                           mbedtls_timing_get_delay);

  ctx->handshake_done = false;
  return xErrno_Ok;
}

static void mbedtls_backend_destroy(xDtlsBackendCtx *ctx) {
  if (!ctx) return;
  mbedtls_ssl_free(&ctx->ssl);
  mbedtls_ssl_config_free(&ctx->conf);
  mbedtls_x509_crt_free(&ctx->cert);
  mbedtls_pk_free(&ctx->pkey);
#if MBEDTLS_VERSION_NUMBER >= 0x04000000
  if (!mbedtls_svc_key_id_is_null(ctx->psa_key_id)) psa_destroy_key(ctx->psa_key_id);
#else
  mbedtls_ctr_drbg_free(&ctx->ctr_drbg);
  mbedtls_entropy_free(&ctx->entropy);
#endif
  mbedtls_ssl_cookie_free(&ctx->cookie);
  free(ctx);
}

static xErrno mbedtls_get_fingerprint(xDtlsBackendCtx *ctx, uint8_t *out) {
  if (!ctx || !out) return xErrno_InvalidArg;
  if (ctx->cert.raw.len == 0) return xErrno_SysError;

  /* SHA-256 of the DER-encoded certificate */
  int ret = xp2p_sha256(ctx->cert.raw.p, ctx->cert.raw.len, out);
  return (ret == 0) ? xErrno_Ok : xErrno_SysError;
}

static xErrno mbedtls_backend_handshake(xDtlsBackendCtx *ctx) {
  if (!ctx) return xErrno_InvalidArg;

  int ret = mbedtls_ssl_handshake(&ctx->ssl);

  if (ret == 0) {
    ctx->handshake_done = true;
    return xErrno_Ok;
  }

  if (ret == MBEDTLS_ERR_SSL_WANT_READ || ret == MBEDTLS_ERR_SSL_WANT_WRITE) {
    return xErrno_Again;
  }

  /* Hello verify request (DTLS cookie) — need to restart */
  if (ret == MBEDTLS_ERR_SSL_HELLO_VERIFY_REQUIRED) {
    mbedtls_ssl_session_reset(&ctx->ssl);
    return xErrno_Again;
  }

  return xErrno_SysError;
}

static xErrno mbedtls_backend_feed_input(xDtlsBackendCtx *ctx, const uint8_t *data, size_t len) {
  if (!ctx || !data) return xErrno_InvalidArg;

  int written = iobuf_write(&ctx->recv_buf, data, len);
  return (written > 0) ? xErrno_Ok : xErrno_SysError;
}

static xErrno mbedtls_backend_encrypt_send(xDtlsBackendCtx *ctx, const uint8_t *data, size_t len) {
  if (!ctx || !data) return xErrno_InvalidArg;

  int ret = mbedtls_ssl_write(&ctx->ssl, data, len);
  if (ret >= 0) return xErrno_Ok;
  if (ret == MBEDTLS_ERR_SSL_WANT_WRITE) return xErrno_Again;
  return xErrno_SysError;
}

static xErrno mbedtls_backend_decrypt_read(xDtlsBackendCtx *ctx, uint8_t *buf, size_t buf_cap,
                                           size_t *out_len) {
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

static xErrno mbedtls_backend_get_remote_fingerprint(xDtlsBackendCtx *ctx, uint8_t *out) {
  if (!ctx || !out) return xErrno_InvalidArg;

  const mbedtls_x509_crt *peer = mbedtls_ssl_get_peer_cert(&ctx->ssl);
  if (!peer || peer->raw.len == 0) return xErrno_SysError;

  int ret = xp2p_sha256(peer->raw.p, peer->raw.len, out);
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
  .set_role               = mbedtls_backend_set_role,
  .get_fingerprint        = mbedtls_get_fingerprint,
  .handshake              = mbedtls_backend_handshake,
  .flush_output           = NULL,
  .feed_input             = mbedtls_backend_feed_input,
  .encrypt_send           = mbedtls_backend_encrypt_send,
  .decrypt_read           = mbedtls_backend_decrypt_read,
  .get_remote_fingerprint = mbedtls_backend_get_remote_fingerprint,
  .is_handshake_done      = mbedtls_backend_is_handshake_done,
};

const xDtlsBackend *xDtlsBackendGet(void) {
  return &g_mbedtls_backend;
}
