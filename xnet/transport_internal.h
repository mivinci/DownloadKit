/*
 * Copyright 2025 The xKit Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * transport_internal.h - Internal transport initialization functions
 *
 * This header is intended for cross-module use within the xKit project
 * (e.g. xhttp consuming xnet). It is NOT part of the public API and
 * should not be included by external users.
 */

#ifndef XNET_TRANSPORT_INTERNAL_H
#define XNET_TRANSPORT_INTERNAL_H

#include "transport.h"

/* ───────────────────── Transport initialization ───────────────────── */

/**
 * Initialize a Plain TCP transport for the given file descriptor.
 * The transport's read/writev map directly to read(2)/writev(2).
 * handshake and alpn are set to NULL.
 *
 * @param transport  Transport to initialize (must not be NULL).
 * @param fd         File descriptor for the connection.
 */
void xTransportPlainInit(xTransport *transport, int fd);

/**
 * Initialize a TLS server transport for the given file descriptor.
 * Creates an SSL object in accept mode using the shared server TLS context.
 *
 * @param transport  Transport to initialize (must not be NULL).
 * @param tls_ctx    Server TLS context from xTlsCtxCreate() (must not be NULL).
 * @param fd         File descriptor for the accepted connection.
 */
void xTransportTlsServerInit(xTransport *transport, xTlsCtx tls_ctx, int fd);

/**
 * Initialize a TLS client transport for the given file descriptor.
 * Uses the shared TLS context to create a per-connection SSL object
 * in connect mode.
 *
 * @param transport  Transport to initialize (must not be NULL).
 * @param tls_ctx    Client TLS context from xTlsCtxCreate() (must not be NULL).
 * @param hostname   Server hostname for SNI and verification.
 * @param fd         File descriptor for the TCP connection.
 * @return           0 on success, -1 on error.
 */
int xTransportTlsClientInit(xTransport *transport, xTlsCtx tls_ctx,
                            const char *hostname, int fd);

#endif /* XNET_TRANSPORT_INTERNAL_H */
