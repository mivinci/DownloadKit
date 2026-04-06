/*
 * Copyright 2025 The xKit Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * tls.h - TLS configuration types shared across xKit modules
 */

#ifndef XNET_TLS_H
#define XNET_TLS_H

#include <xbase/base.h>

/**
 * @brief TLS configuration for client-side connections.
 *
 * Controls how a client verifies the server's TLS certificate and
 * optionally presents a client certificate for mutual TLS (mTLS).
 *
 * Zero-initialize for defaults: system CA bundle, peer and host
 * verification enabled, no client certificate.
 */
XDEF_STRUCT(xTlsClientConf) {
  const char *ca;           /**< Path to CA cert file (NULL = system default) */
  const char *cert;         /**< Path to client certificate (NULL = none)     */
  const char *key;          /**< Path to client private key (NULL = none)     */
  const char *key_password; /**< Private key password (NULL = none)           */
  int         skip_verify;  /**< If non-zero, skip peer & host verification   */
};

/**
 * @brief TLS configuration for server-side listeners.
 *
 * Provides the certificate, private key, and optional CA for client
 * verification. Used by any server that needs to accept TLS connections.
 */
XDEF_STRUCT(xTlsServerConf) {
  const char *cert;         /**< Path to PEM certificate file (required)      */
  const char *key;          /**< Path to PEM private key file (required)      */
  const char *ca;           /**< Path to CA certificate file (optional)       */
  int         verify_peer;  /**< Peer verification: 0=none, 1=optional, 2=required */
};

#endif /* XNET_TLS_H */
