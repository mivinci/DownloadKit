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
 * @brief Opaque handle to a server-level TLS context.
 *
 * Created by xTlsCtxCreate(), shared across all connections accepted
 * by a listener. Destroyed by xTlsCtxDestroy().
 */
XDEF_HANDLE(xTlsCtx);

/**
 * @brief Unified TLS configuration for both client and server.
 *
 * Controls certificate loading, peer verification, and optional ALPN
 * negotiation. Used by TCP connectors, TCP listeners, HTTP clients,
 * HTTP servers, and WebSocket clients.
 *
 * Zero-initialize for secure defaults: system CA bundle, peer
 * verification enabled, no client/server certificate, no ALPN.
 *
 * Server-side usage:
 *   - `cert` and `key` are required (server certificate + private key).
 *   - `ca` is optional (for client certificate verification / mTLS).
 *   - `alpn` is optional (e.g. {"h2", "http/1.1", NULL}).
 *
 * Client-side usage:
 *   - `cert` and `key` are optional (for mutual TLS / mTLS).
 *   - `ca` overrides the system CA bundle.
 *   - `key_password` provides the private key passphrase.
 */
XDEF_STRUCT(xTlsConf) {
  const char  *cert;         /**< Path to PEM certificate file (NULL = none)       */
  const char  *key;          /**< Path to PEM private key file (NULL = none)       */
  const char  *ca;           /**< Path to CA cert file (NULL = system default)     */
  const char  *key_password; /**< Private key password (NULL = none)               */
  const char **alpn;         /**< NULL-terminated ALPN protocol list (NULL = none) */
  int          skip_verify;  /**< If non-zero, skip peer & host verification       */
};

/**
 * @brief Backward-compatible aliases for the unified xTlsConf.
 */
typedef xTlsConf xTlsClientConf;
typedef xTlsConf xTlsServerConf;

#endif /* XNET_TLS_H */
