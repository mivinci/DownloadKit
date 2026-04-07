/*
 * Copyright 2025 The xKit Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * transport_plain.c - Plain TCP transport implementation
 *
 * Maps transport read/writev directly to read(2)/writev(2) syscalls.
 */

#include "transport_private.h"

#include <errno.h>
#include <stdlib.h>
#include <sys/uio.h>
#include <unistd.h>

/* ───────────────────── Plain TCP context ───────────────────── */

XDEF_STRUCT(xTransportPlain_) {
  int fd; /**< File descriptor for the connection */
};

/* ───────────────────── vtable callbacks ───────────────────── */

static ssize_t plain_read(void *ctx, void *buf, size_t len) {
  xTransportPlain_ *p = (xTransportPlain_ *)ctx;
  ssize_t           n;
  do {
    n = read(p->fd, buf, len);
  } while (n < 0 && errno == EINTR);
  return n;
}

static ssize_t plain_writev(void *ctx, const struct iovec *iov, int iovcnt) {
  xTransportPlain_ *p = (xTransportPlain_ *)ctx;
  ssize_t           n;
  do {
    n = writev(p->fd, iov, iovcnt);
  } while (n < 0 && errno == EINTR);
  return n;
}

static void plain_destroy(void *ctx) {
  free(ctx);
}

/* ───────────────────── Public API ───────────────────── */

void xTransportPlainInit(xTransport *transport, int fd) {
  if (!transport) return;

  xTransportPlain_ *p =
    (xTransportPlain_ *)malloc(sizeof(xTransportPlain_));
  if (!p) {
    /* Fallback: zero out the transport so callers can detect failure */
    transport->read      = NULL;
    transport->writev    = NULL;
    transport->handshake = NULL;
    transport->alpn      = NULL;
    transport->destroy   = NULL;
    transport->ctx       = NULL;
    return;
  }

  p->fd = fd;

  transport->read      = plain_read;
  transport->writev    = plain_writev;
  transport->handshake = NULL; /* No handshake for plain TCP */
  transport->alpn      = NULL; /* No ALPN for plain TCP */
  transport->destroy   = plain_destroy;
  transport->ctx       = p;
}
