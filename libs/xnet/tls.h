/*
 * Copyright 2025 The moo Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * tls.h - TLS configuration types shared across moo modules
 */

#ifndef XNET_TLS_H
#define XNET_TLS_H

#include <xbase/base.h>

/**
 * @brief Opaque handle to a TLS context.
 *
 * Created by xTlsCtxCreate(), shared across all connections on a
 * listener or connector. Destroyed by xTlsCtxDestroy().
 *
 * Automatically selects server or client mode based on the
 * configuration: if cert and key are provided, server mode is used;
 * otherwise, client mode is used.
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

/* ───────────────────── TLS context management ───────────────────── */

/**
 * @brief Create a TLS context.
 *
 * Loads the certificate, private key, and optional CA. The mode
 * (server or client) is determined automatically:
 *   - If conf->cert and conf->key are both non-NULL, server mode.
 *   - Otherwise, client mode.
 *
 * The returned context is shared across all connections on a
 * listener or connector.
 *
 * @param conf  TLS configuration (must not be NULL).
 * @return      TLS context handle, or NULL on failure.
 */
XCAPI(xTlsCtx) xTlsCtxCreate(const xTlsConf *conf);

/**
 * @brief Destroy a TLS context.
 *
 * Releases all resources associated with the context.
 * Safe to call with NULL (no-op).
 *
 * @param ctx  TLS context returned by xTlsCtxCreate(), or NULL.
 */
XCAPI(void) xTlsCtxDestroy(xTlsCtx ctx);

/**
 * @brief Hot-reload certificates for an existing TLS context.
 *
 * Atomically replaces the certificate, private key, and optional CA
 * in the given context. Existing connections are not affected; only
 * new connections will use the updated certificates.
 *
 * @param ctx   TLS context to reload (must not be NULL).
 * @param conf  New TLS configuration (must not be NULL, cert and
 *              key must not be NULL).
 * @return      0 on success, -1 on failure (context unchanged).
 */
XCAPI(int) xTlsCtxReload(xTlsCtx ctx, const xTlsConf *conf);

#endif /* XNET_TLS_H */
