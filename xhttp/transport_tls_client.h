/*
 * Copyright 2025 The xKit Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * transport_tls_client.h - TLS client transport initialization
 *
 * Creates a TLS transport configured for client-side connections
 * (SSL_set_connect_state). Used by the WebSocket client to
 * establish wss:// connections.
 */

#ifndef XHTTP_TRANSPORT_TLS_CLIENT_H
#define XHTTP_TRANSPORT_TLS_CLIENT_H

#include "transport.h"
#include <xnet/tls.h>

/**
 * Initialize a TLS client transport for the given fd.
 *
 * Creates an SSL_CTX + SSL object in connect mode. If @p conf
 * is NULL, a default configuration is used (system CA, peer
 * verification enabled).
 *
 * @param transport  Transport to initialize (must not be NULL).
 * @param conf       TLS client configuration, or NULL for defaults.
 * @param hostname   Server hostname for SNI and verification.
 * @param fd         File descriptor for the TCP connection.
 * @return 0 on success, -1 on error.
 */
int xHttpTlsClientTransportInit(xHttpTransport *transport,
                                const xTlsConf *conf,
                                const char *hostname,
                                int fd);

#endif /* XHTTP_TRANSPORT_TLS_CLIENT_H */
