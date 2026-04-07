/*
 * Copyright 2025 The xKit Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * tcp_conn.c - xTcpConn connection resource management
 */

#include "tcp_private.h"

#include <stdlib.h>
#include <string.h>

/* ───────────────────── Internal constructor ───────────────────── */

xTcpConn xTcpConnCreate_(xSocket sock, xTransport transport) {
  xTcpConn_ *c = (xTcpConn_ *)calloc(1, sizeof(xTcpConn_));
  if (!c) return NULL;
  c->sock      = sock;
  c->transport = transport;
  return (xTcpConn)c;
}

/* ───────────────────── Public API ───────────────────── */

xTransport *xTcpConnTransport(xTcpConn conn) {
  if (!conn) return NULL;
  xTcpConn_ *c = (xTcpConn_ *)conn;
  return &c->transport;
}

xSocket xTcpConnSocket(xTcpConn conn) {
  if (!conn) return NULL;
  xTcpConn_ *c = (xTcpConn_ *)conn;
  return c->sock;
}

void xTcpConnClose(xEventLoop loop, xTcpConn conn) {
  if (!conn) return;
  xTcpConn_ *c = (xTcpConn_ *)conn;

  /* 1. Destroy transport (e.g. SSL object) */
  if (c->transport.destroy) {
    c->transport.destroy(c->transport.ctx);
    memset(&c->transport, 0, sizeof(c->transport));
  }

  /* 2. Destroy socket (closes fd) */
  if (c->sock) {
    xSocketDestroy(loop, c->sock);
    c->sock = NULL;
  }

  /* 3. Free the conn shell */
  free(c);
}

xSocket xTcpConnTakeSocket(xTcpConn conn) {
  if (!conn) return NULL;
  xTcpConn_ *c = (xTcpConn_ *)conn;
  xSocket sock = c->sock;
  c->sock = NULL;
  return sock;
}

xTransport xTcpConnTakeTransport(xTcpConn conn) {
  xTransport zero;
  memset(&zero, 0, sizeof(zero));
  if (!conn) return zero;
  xTcpConn_ *c = (xTcpConn_ *)conn;
  xTransport t = c->transport;
  memset(&c->transport, 0, sizeof(c->transport));
  return t;
}
