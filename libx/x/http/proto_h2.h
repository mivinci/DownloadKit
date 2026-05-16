/*
 * Copyright 2025 The moo Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * proto_h2.h - HTTP/2 protocol handler (internal)
 */

#ifndef XHTTP_PROTO_H2_H
#define XHTTP_PROTO_H2_H

struct xHttpConn_;

/**
 * Initialize the HTTP/2 protocol handler for a connection.
 * Allocates an xHttpProtoH2 on the heap, creates an nghttp2 server session,
 * sends the server connection preface (SETTINGS frame), and populates
 * conn->proto.
 *
 * Returns 0 on success, -1 on allocation/initialization failure.
 */
int xHttpProtoH2Init(struct xHttpConn_ *conn);

#endif /* XHTTP_PROTO_H2_H */
