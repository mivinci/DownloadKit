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
#include <sys/uio.h>

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

ssize_t xTcpConnRecv(xTcpConn conn, void *buf, size_t len) {
  if (!conn) return -1;
  xTcpConn_ *c = (xTcpConn_ *)conn;
  if (!c->transport.read) return -1;
  return c->transport.read(c->transport.ctx, buf, len);
}

ssize_t xTcpConnSend(xTcpConn conn, const char *buf, size_t len) {
  if (!conn) return -1;
  xTcpConn_ *c = (xTcpConn_ *)conn;
  if (!c->transport.writev) return -1;
  struct iovec iov = {.iov_base = (void *)buf, .iov_len = len};
  return c->transport.writev(c->transport.ctx, &iov, 1);
}

ssize_t xTcpConnSendIov(xTcpConn conn, const struct iovec *iov, int iovcnt) {
  if (!conn) return -1;
  xTcpConn_ *c = (xTcpConn_ *)conn;
  if (!c->transport.writev) return -1;
  return c->transport.writev(c->transport.ctx, iov, iovcnt);
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
  xTcpConn_ *c    = (xTcpConn_ *)conn;
  xSocket    sock = c->sock;
  c->sock         = NULL;
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

/* ───────────────────── I/O adapters ───────────────────── */

xReader xTcpConnReader(xTcpConn conn) {
  xReader r = {NULL, NULL};
  if (!conn) return r;
  xTcpConn_ *c = (xTcpConn_ *)conn;
  r.read       = c->transport.read;
  r.ctx        = c->transport.ctx;
  return r;
}

xWriter xTcpConnWriter(xTcpConn conn) {
  xWriter w = {NULL, NULL};
  if (!conn) return w;
  xTcpConn_ *c = (xTcpConn_ *)conn;
  w.writev     = c->transport.writev;
  w.ctx        = c->transport.ctx;
  return w;
}
