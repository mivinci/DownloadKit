/*
 * Copyright 2025 The xKit Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * tcp_private.h - Internal TCP types and helpers
 */

#ifndef XNET_TCP_PRIVATE_H
#define XNET_TCP_PRIVATE_H

#include "tcp.h"
#include "transport_private.h"

/**
 * @brief Internal representation of xTcpConn.
 */
XDEF_STRUCT(xTcpConn_) {
  xSocket    sock;
  xTransport transport;
};

/**
 * @brief Create an xTcpConn from a socket and transport.
 *
 * Used internally by xTcpConnect and xTcpListener after connection
 * establishment is complete.
 *
 * @param sock       The socket (ownership transferred to conn).
 * @param transport  The transport (ownership transferred to conn).
 * @return           A new xTcpConn, or NULL on allocation failure.
 */
xTcpConn xTcpConnCreate_(xSocket sock, xTransport transport);

#endif /* XNET_TCP_PRIVATE_H */
