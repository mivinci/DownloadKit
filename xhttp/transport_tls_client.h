/*
 * Copyright 2025 The xKit Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * transport_tls_client.h - TLS client transport initialization
 *
 * Creates a per-connection SSL object using a shared TLS context.
 * Used by the WebSocket client to establish wss:// connections.
 */

#ifndef XHTTP_TRANSPORT_TLS_CLIENT_H
#define XHTTP_TRANSPORT_TLS_CLIENT_H

#include "transport.h"
#include <xnet/tls.h>

/**
 * Initialize a TLS client transport for the given fd.
 *
 * Uses the shared @p tls_ctx to create a per-connection SSL object
 * in connect mode. The context is NOT owned by the transport.
 *
 * @param transport  Transport to initialize (must not be NULL).
 * @param tls_ctx    Shared TLS context from xTlsCtxCreate() (must not be NULL).
 * @param hostname   Server hostname for SNI and verification.
 * @param fd         File descriptor for the TCP connection.
 * @return 0 on success, -1 on error.
 */
int xHttpTlsClientTransportInit(xHttpTransport *transport,
                                xTlsCtx tls_ctx,
                                const char *hostname,
                                int fd);

#endif /* XHTTP_TRANSPORT_TLS_CLIENT_H */
