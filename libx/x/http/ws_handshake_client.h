/*
 * Copyright 2025 The libx Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * ws_handshake_client.h - Client-side WebSocket upgrade handshake
 *
 * Internal API for building the HTTP Upgrade request and
 * validating the 101 Switching Protocols response.
 */

#ifndef XHTTP_WS_HANDSHAKE_CLIENT_H
#define XHTTP_WS_HANDSHAKE_CLIENT_H

#include <x/buf/io.h>
#include <x/net/url.h>

#ifdef XHTTP_WS_DEFLATE
#include "ws_deflate.h"
#endif

/**
 * Build the HTTP/1.1 Upgrade request and append it to @p io.
 *
 * Generates a random Sec-WebSocket-Key and stores the expected
 * Sec-WebSocket-Accept value in @p accept_out (must be >= 64
 * bytes).
 *
 * @param io          Output buffer for the request.
 * @param url         Parsed URL (host, port, path used).
 * @param headers     Extra headers (may be NULL).
 * @param accept_out  Buffer to receive expected Accept value.
 * @param accept_sz   Size of accept_out buffer.
 * @return 0 on success, -1 on error.
 */
int xWsClientBuildUpgradeRequest(xIOBuffer *io,
                                 const xUrl *url,
                                 const char *headers,
                                 char *accept_out,
                                 size_t accept_sz);

/**
 * Validate a 101 Switching Protocols response.
 *
 * Checks status code, Upgrade, Connection, and
 * Sec-WebSocket-Accept headers.
 *
 * @param data            Response data (NUL-terminated).
 * @param len             Length of response data.
 * @param expected_accept Expected Sec-WebSocket-Accept value.
 * @return 0 if valid, -1 on protocol error.
 */
int xWsClientValidateUpgradeResponse(const char *data,
                                     size_t len,
                                     const char *expected_accept);

#ifdef XHTTP_WS_DEFLATE
/**
 * Parse permessage-deflate from the server's 101 response.
 *
 * @param data   Response data.
 * @param len    Response length.
 * @param params Output deflate parameters.
 * @return 0 if extension was negotiated, -1 otherwise.
 */
int xWsClientParseDeflateResponse(const char *data,
                                  size_t len,
                                  xWsDeflateParams *params);
#endif

#endif /* XHTTP_WS_HANDSHAKE_CLIENT_H */
