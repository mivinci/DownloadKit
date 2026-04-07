/*
 * Copyright 2025 The xKit Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * transport_private.h - Internal transport initialization functions
 *
 * These functions are used only within the xhttp module (e.g. server.c)
 * and should not be exposed in the public transport.h header.
 */

#ifndef XHTTP_TRANSPORT_PRIVATE_H
#define XHTTP_TRANSPORT_PRIVATE_H

#include "transport.h"
#include <xhttp/server.h>

/* ───────────────────── Plain TCP transport ───────────────────── */

/**
 * Initialize a Plain TCP transport for the given file descriptor.
 * The transport's read/writev map directly to read(2)/writev(2).
 * handshake and alpn are set to NULL.
 *
 * @param transport  Transport to initialize (must not be NULL).
 * @param fd         File descriptor for the connection.
 */
void xHttpTransportPlainInit(xHttpTransport *transport, int fd);

/* ───────────────────── TLS backends ───────────────────── */

#if defined(XK_HAS_OPENSSL)
void *xHttpTlsCtxCreateOpenSSL(const xTlsConf *config);
void  xHttpTlsCtxDestroyOpenSSL(void *ctx);
void  xHttpTlsTransportInitOpenSSL(xHttpTransport *transport, void *tls_ctx,
                                   int fd);
#elif defined(XK_HAS_MBEDTLS)
void *xHttpTlsCtxCreateMbedTLS(const xTlsConf *config);
void  xHttpTlsCtxDestroyMbedTLS(void *ctx);
void  xHttpTlsTransportInitMbedTLS(xHttpTransport *transport, void *tls_ctx,
                                   int fd);
#endif

#endif /* XHTTP_TRANSPORT_PRIVATE_H */
