/*
 * Copyright 2025 The xKit Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * tls_private.h - Internal TLS helpers (not part of the public API)
 */

#ifndef XNET_TLS_PRIVATE_H
#define XNET_TLS_PRIVATE_H

#include <xnet/tls.h>

/**
 * @brief Get the native TLS context pointer.
 *
 * Returns the underlying SSL_CTX* (OpenSSL) or mbedtls_ssl_config*
 * (mbedTLS) from the opaque xTlsCtx handle. Used internally by
 * transport layers to create per-connection SSL objects.
 *
 * @param ctx  TLS context handle (must not be NULL).
 * @return     Native TLS context pointer, or NULL.
 */
void *xTlsCtxGetNative(xTlsCtx ctx);

/**
 * @brief Check if the TLS context is in server mode.
 *
 * @param ctx  TLS context handle (must not be NULL).
 * @return     Non-zero if server mode, 0 if client mode.
 */
int xTlsCtxIsServer(xTlsCtx ctx);

#endif /* XNET_TLS_PRIVATE_H */
