/*
 * Copyright 2025 The libx Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * tls_mbedtls.c - mbedTLS TLS context management
 *
 * Implements xTlsCtxCreate / xTlsCtxDestroy / xTlsCtxReload /
 * xTlsCtxGetNative for the mbedTLS backend.
 */

#ifdef X_HAS_MBEDTLS

#include "tls_private.h"

/* mbedTLS 3.x+ provides build_info.h; mbedTLS 2.x uses version.h */
#if __has_include(<mbedtls/build_info.h>)
#include <mbedtls/build_info.h>
#else
#include <mbedtls/version.h>
#endif
#include <mbedtls/error.h>
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
#define X_MBEDTLS_SET_MIN_TLS12(conf) \
  mbedtls_ssl_conf_min_tls_version((conf), MBEDTLS_SSL_VERSION_TLS1_2)
#else
/* mbedTLS 2.x */
#define X_MBEDTLS_SET_MIN_TLS12(conf) \
  mbedtls_ssl_conf_min_version((conf), MBEDTLS_SSL_MAJOR_VERSION_3, MBEDTLS_SSL_MINOR_VERSION_3)
#endif

#include <stdlib.h>
#include <x/base/log.h>

/* ═══════════════════════════════════════════════════════════════════
 *  TLS context (server + client)
 * ═══════════════════════════════════════════════════════════════════
 */

XDEF_STRUCT(xTlsCtxMbedTLS_) {
  mbedtls_ssl_config conf;
  mbedtls_x509_crt   cert;
  mbedtls_pk_context pkey;
  mbedtls_x509_crt   ca_cert;
#if MBEDTLS_VERSION_NUMBER < 0x04000000
  mbedtls_entropy_context  entropy;
  mbedtls_ctr_drbg_context ctr_drbg;
#endif
  int          has_ca;
  int          is_server;
  const char **alpn_list; /**< Borrowed pointer to user's ALPN list */
};

xTlsCtx xTlsCtxCreate(const xTlsConf *config) {
  if (!config) return NULL;

  /* Determine mode: server if cert+key provided, client otherwise */
  int is_server = (config->cert && config->key) ? 1 : 0;

  xTlsCtxMbedTLS_ *ctx = (xTlsCtxMbedTLS_ *)calloc(1, sizeof(xTlsCtxMbedTLS_));
  if (!ctx) return NULL;

  mbedtls_ssl_config_init(&ctx->conf);
  mbedtls_x509_crt_init(&ctx->cert);
  mbedtls_pk_init(&ctx->pkey);
  mbedtls_x509_crt_init(&ctx->ca_cert);
  ctx->has_ca    = 0;
  ctx->is_server = is_server;
  ctx->alpn_list = NULL;

  int ret;

#if MBEDTLS_VERSION_NUMBER < 0x04000000
  /* mbedTLS 2.x/3.x: seed the random number generator manually */
  mbedtls_entropy_init(&ctx->entropy);
  mbedtls_ctr_drbg_init(&ctx->ctr_drbg);
  ret = mbedtls_ctr_drbg_seed(&ctx->ctr_drbg, mbedtls_entropy_func, &ctx->entropy, NULL, 0);
  if (ret != 0) {
    xLog(false, "xnet: mbedtls_ctr_drbg_seed failed: -0x%04x", -ret);
    goto fail;
  }
#endif

  /* Configure as TLS server or client */
  ret = mbedtls_ssl_config_defaults(&ctx->conf,
                                    is_server ? MBEDTLS_SSL_IS_SERVER : MBEDTLS_SSL_IS_CLIENT,
                                    MBEDTLS_SSL_TRANSPORT_STREAM, MBEDTLS_SSL_PRESET_DEFAULT);
  if (ret != 0) {
    xLog(false, "xnet: mbedtls_ssl_config_defaults failed: -0x%04x", -ret);
    goto fail;
  }

#if MBEDTLS_VERSION_NUMBER < 0x04000000
  mbedtls_ssl_conf_rng(&ctx->conf, mbedtls_ctr_drbg_random, &ctx->ctr_drbg);
#endif

  /* Set minimum TLS version to 1.2 */
  X_MBEDTLS_SET_MIN_TLS12(&ctx->conf);

  if (is_server) {
    /* ── Server mode ── */

    /* Load certificate */
    ret = mbedtls_x509_crt_parse_file(&ctx->cert, config->cert);
    if (ret != 0) {
      xLog(false, "xnet: failed to load certificate: %s (ret=-0x%04x)", config->cert, -ret);
      goto fail;
    }

    /* Load private key */
#if MBEDTLS_VERSION_NUMBER >= 0x04000000
    ret = mbedtls_pk_parse_keyfile(&ctx->pkey, config->key, NULL);
#elif MBEDTLS_VERSION_NUMBER >= 0x03000000
    ret = mbedtls_pk_parse_keyfile(&ctx->pkey, config->key, NULL, mbedtls_ctr_drbg_random,
                                   &ctx->ctr_drbg);
#else
    ret = mbedtls_pk_parse_keyfile(&ctx->pkey, config->key, NULL);
#endif
    if (ret != 0) {
      xLog(false, "xnet: failed to load private key: %s (ret=-0x%04x)", config->key, -ret);
      goto fail;
    }

    /* Set own certificate and key */
    ret = mbedtls_ssl_conf_own_cert(&ctx->conf, &ctx->cert, &ctx->pkey);
    if (ret != 0) {
      xLog(false, "xnet: mbedtls_ssl_conf_own_cert failed: -0x%04x", -ret);
      goto fail;
    }

    /* Load CA certificate for client verification (optional) */
    if (config->ca) {
      ret = mbedtls_x509_crt_parse_file(&ctx->ca_cert, config->ca);
      if (ret != 0) {
        xLog(false, "xnet: failed to load CA certificate: %s (ret=-0x%04x)", config->ca, -ret);
        goto fail;
      }
      mbedtls_ssl_conf_ca_chain(&ctx->conf, &ctx->ca_cert, NULL);
      ctx->has_ca = 1;
    }

    /* Peer verification mode */
    if (config->skip_verify) {
      mbedtls_ssl_conf_authmode(&ctx->conf, MBEDTLS_SSL_VERIFY_NONE);
    } else {
      mbedtls_ssl_conf_authmode(&ctx->conf, MBEDTLS_SSL_VERIFY_REQUIRED);
    }
  } else {
    /* ── Client mode ── */

    if (!config->skip_verify) {
      /* Load CA certificates */
      mbedtls_x509_crt_init(&ctx->ca_cert);
      if (config->ca) {
        ret = mbedtls_x509_crt_parse_file(&ctx->ca_cert, config->ca);
        if (ret != 0) {
          xLog(false, "xnet: failed to load CA: %s (ret=-0x%04x)", config->ca, -ret);
          goto fail;
        }
      } else {
        /* Load system default CA bundle */
        static const char *ca_paths[] = {
          "/etc/ssl/certs/ca-certificates.crt",
          "/etc/pki/tls/certs/ca-bundle.crt",
          "/usr/local/share/certs/ca-root-nss.crt",
          "/etc/ssl/cert.pem",
          NULL,
        };
        int loaded = 0;
        for (int i = 0; ca_paths[i]; i++) {
          ret = mbedtls_x509_crt_parse_file(&ctx->ca_cert, ca_paths[i]);
          if (ret == 0) {
            loaded = 1;
            break;
          }
        }
        if (!loaded) {
          ret = mbedtls_x509_crt_parse_path(&ctx->ca_cert, "/etc/ssl/certs");
          if (ret == 0) loaded = 1;
        }
        if (!loaded) {
          xLog(false, "xnet: no system CA bundle found for mbedTLS client");
        }
      }
      mbedtls_ssl_conf_ca_chain(&ctx->conf, &ctx->ca_cert, NULL);
      ctx->has_ca = 1;
      mbedtls_ssl_conf_authmode(&ctx->conf, MBEDTLS_SSL_VERIFY_REQUIRED);
    } else {
      mbedtls_ssl_conf_authmode(&ctx->conf, MBEDTLS_SSL_VERIFY_NONE);
    }

    /* Load client certificate for mTLS (optional) */
    if (config->cert && config->key) {
      ret = mbedtls_x509_crt_parse_file(&ctx->cert, config->cert);
      if (ret != 0) {
        xLog(false, "xnet: failed to load client cert: %s (ret=-0x%04x)", config->cert, -ret);
        goto fail;
      }

      const char *pwd = config->key_password;
#if MBEDTLS_VERSION_NUMBER >= 0x04000000
      ret = mbedtls_pk_parse_keyfile(&ctx->pkey, config->key, pwd);
#elif MBEDTLS_VERSION_NUMBER >= 0x03000000
      ret = mbedtls_pk_parse_keyfile(&ctx->pkey, config->key, pwd, mbedtls_ctr_drbg_random,
                                     &ctx->ctr_drbg);
#else
      ret = mbedtls_pk_parse_keyfile(&ctx->pkey, config->key, pwd);
#endif
      if (ret != 0) {
        xLog(false, "xnet: failed to load client key: %s (ret=-0x%04x)", config->key, -ret);
        goto fail;
      }

      ret = mbedtls_ssl_conf_own_cert(&ctx->conf, &ctx->cert, &ctx->pkey);
      if (ret != 0) {
        xLog(false, "xnet: mbedtls_ssl_conf_own_cert failed: -0x%04x", -ret);
        goto fail;
      }
    }
  }

  /* Configure ALPN (parameterized) */
  if (config->alpn) {
    ret = mbedtls_ssl_conf_alpn_protocols(&ctx->conf, config->alpn);
    if (ret != 0) {
      xLog(false, "xnet: mbedtls_ssl_conf_alpn_protocols failed: -0x%04x", -ret);
      goto fail;
    }
    ctx->alpn_list = config->alpn;
  }

  return (xTlsCtx)ctx;

fail:
#if MBEDTLS_VERSION_NUMBER < 0x04000000
  mbedtls_ctr_drbg_free(&ctx->ctr_drbg);
  mbedtls_entropy_free(&ctx->entropy);
#endif
  mbedtls_pk_free(&ctx->pkey);
  mbedtls_x509_crt_free(&ctx->cert);
  mbedtls_x509_crt_free(&ctx->ca_cert);
  mbedtls_ssl_config_free(&ctx->conf);
  free(ctx);
  return NULL;
}

void xTlsCtxDestroy(xTlsCtx raw) {
  if (!raw) return;
  xTlsCtxMbedTLS_ *ctx = (xTlsCtxMbedTLS_ *)raw;
#if MBEDTLS_VERSION_NUMBER < 0x04000000
  mbedtls_ctr_drbg_free(&ctx->ctr_drbg);
  mbedtls_entropy_free(&ctx->entropy);
#endif
  mbedtls_pk_free(&ctx->pkey);
  mbedtls_x509_crt_free(&ctx->cert);
  if (ctx->has_ca) mbedtls_x509_crt_free(&ctx->ca_cert);
  mbedtls_ssl_config_free(&ctx->conf);
  free(ctx);
}

int xTlsCtxReload(xTlsCtx raw, const xTlsConf *config) {
  if (!raw || !config || !config->cert || !config->key) return -1;

  xTlsCtxMbedTLS_ *ctx = (xTlsCtxMbedTLS_ *)raw;
  int              ret;

  /* Reload certificate: free old, parse new */
  mbedtls_x509_crt new_cert;
  mbedtls_x509_crt_init(&new_cert);
  ret = mbedtls_x509_crt_parse_file(&new_cert, config->cert);
  if (ret != 0) {
    xLog(false, "xnet: reload: failed to load certificate: %s (ret=-0x%04x)", config->cert, -ret);
    mbedtls_x509_crt_free(&new_cert);
    return -1;
  }

  /* Reload private key: free old, parse new */
  mbedtls_pk_context new_pkey;
  mbedtls_pk_init(&new_pkey);
#if MBEDTLS_VERSION_NUMBER >= 0x04000000
  ret = mbedtls_pk_parse_keyfile(&new_pkey, config->key, NULL);
#elif MBEDTLS_VERSION_NUMBER >= 0x03000000
  ret =
    mbedtls_pk_parse_keyfile(&new_pkey, config->key, NULL, mbedtls_ctr_drbg_random, &ctx->ctr_drbg);
#else
  ret = mbedtls_pk_parse_keyfile(&new_pkey, config->key, NULL);
#endif
  if (ret != 0) {
    xLog(false, "xnet: reload: failed to load private key: %s (ret=-0x%04x)", config->key, -ret);
    mbedtls_x509_crt_free(&new_cert);
    mbedtls_pk_free(&new_pkey);
    return -1;
  }

  /* Swap in the new cert and key */
  mbedtls_x509_crt_free(&ctx->cert);
  mbedtls_pk_free(&ctx->pkey);
  ctx->cert = new_cert;
  ctx->pkey = new_pkey;

  /* Re-bind own cert to the config */
  ret = mbedtls_ssl_conf_own_cert(&ctx->conf, &ctx->cert, &ctx->pkey);
  if (ret != 0) {
    xLog(false, "xnet: reload: mbedtls_ssl_conf_own_cert failed: -0x%04x", -ret);
    return -1;
  }

  /* Reload CA certificate (optional) */
  if (config->ca) {
    mbedtls_x509_crt new_ca;
    mbedtls_x509_crt_init(&new_ca);
    ret = mbedtls_x509_crt_parse_file(&new_ca, config->ca);
    if (ret != 0) {
      xLog(false, "xnet: reload: failed to load CA: %s (ret=-0x%04x)", config->ca, -ret);
      mbedtls_x509_crt_free(&new_ca);
      return -1;
    }
    if (ctx->has_ca) mbedtls_x509_crt_free(&ctx->ca_cert);
    ctx->ca_cert = new_ca;
    ctx->has_ca  = 1;
    mbedtls_ssl_conf_ca_chain(&ctx->conf, &ctx->ca_cert, NULL);
  }

  /* Update verification mode */
  if (config->skip_verify) {
    mbedtls_ssl_conf_authmode(&ctx->conf, MBEDTLS_SSL_VERIFY_NONE);
  } else {
    mbedtls_ssl_conf_authmode(&ctx->conf, MBEDTLS_SSL_VERIFY_REQUIRED);
  }

  return 0;
}

void *xTlsCtxGetNative(xTlsCtx raw) {
  if (!raw) return NULL;
  xTlsCtxMbedTLS_ *ctx = (xTlsCtxMbedTLS_ *)raw;
  return &ctx->conf;
}

int xTlsCtxIsServer(xTlsCtx raw) {
  if (!raw) return 0;
  xTlsCtxMbedTLS_ *ctx = (xTlsCtxMbedTLS_ *)raw;
  return ctx->is_server;
}

#endif /* X_HAS_MBEDTLS */
