/*
 * Copyright 2025 The libx Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * proto_h3.h - HTTP/3 protocol handler (internal)
 */

#ifndef XHTTP_PROTO_H3_H
#define XHTTP_PROTO_H3_H

struct xHttpConn_;

/**
 * Initialize the HTTP/3 protocol handler for a QUIC connection.
 * Creates an nghttp3 server connection, populates conn->proto vtable.
 * Called after the QUIC handshake completes.
 *
 * Returns 0 on success, -1 on failure.
 */
int xHttpProtoH3Init(struct xHttpConn_ *conn);

#endif /* XHTTP_PROTO_H3_H */
