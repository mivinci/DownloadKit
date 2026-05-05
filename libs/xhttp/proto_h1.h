/*
 * Copyright 2025 The moo Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * proto_h1.h - HTTP/1.1 protocol handler (internal)
 */

#ifndef XHTTP_PROTO_H1_H
#define XHTTP_PROTO_H1_H

struct xHttpConn_;

/**
 * Initialize the HTTP/1.1 protocol handler for a connection.
 * Allocates an xHttpProtoH1 on the heap and populates conn->proto.
 *
 * Returns 0 on success, -1 on allocation failure.
 */
int xHttpProtoH1Init(struct xHttpConn_ *conn);

#endif /* XHTTP_PROTO_H1_H */
