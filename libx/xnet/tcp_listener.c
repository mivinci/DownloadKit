/*
 * Copyright 2025 The moo Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * tcp_listener.c - Async TCP listener with optional TLS
 *
 * Implements xTcpListenerCreate() / xTcpListenerDestroy().
 * Handles accept loop, optional TLS handshake, and xTcpConn creation.
 */

#include "tcp_private.h"
#include "transport_private.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include <xbase/log.h>

/* Default listen backlog */
#define XTCP_DEFAULT_BACKLOG 128

/* ═══════════════════════════════════════════════════════════════════
 *  Listener state
 * ═══════════════════════════════════════════════════════════════════
 */

XDEF_STRUCT(xTcpListener_) {
  xEventLoop       loop;
  xSocket          listen_sock;
  int              listen_fd;
  xTlsCtx          tls_ctx; /**< NULL for plain TCP */
  xTcpListenerFunc callback;
  void            *user_arg;
};

/* ═══════════════════════════════════════════════════════════════════
 *  TLS handshake state (per pending connection)
 * ═══════════════════════════════════════════════════════════════════
 */

XDEF_STRUCT(xTcpPendingConn_) {
  xTcpListener_          *listener;
  xSocket                 sock;
  int                     fd;
  xTransport              transport;
  struct sockaddr_storage addr;
  socklen_t               addrlen;
};

/* ═══════════════════════════════════════════════════════════════════
 *  Forward declarations
 * ═══════════════════════════════════════════════════════════════════
 */

static void listener_on_event(xSocket sock, xEventMask mask, void *arg);
static void pending_conn_on_event(xSocket sock, xEventMask mask, void *arg);
static void pending_conn_destroy(xTcpPendingConn_ *pc);
static void noop_sock_cb(xSocket sock, xEventMask mask, void *arg);

/* ═══════════════════════════════════════════════════════════════════
 *  Pending TLS connection cleanup
 * ═══════════════════════════════════════════════════════════════════
 */

static void noop_sock_cb(xSocket sock, xEventMask mask, void *arg) {
  (void)sock;
  (void)mask;
  (void)arg;
}

static void pending_conn_destroy(xTcpPendingConn_ *pc) {
  if (!pc) return;
  if (pc->transport.destroy) {
    pc->transport.destroy(pc->transport.ctx);
    memset(&pc->transport, 0, sizeof(pc->transport));
  }
  if (pc->sock) {
    xSocketDestroy(pc->listener->loop, pc->sock);
    pc->sock = NULL;
  }
  free(pc);
}

/* ═══════════════════════════════════════════════════════════════════
 *  TLS handshake event handler (per pending connection)
 * ═══════════════════════════════════════════════════════════════════
 */

static void pending_conn_on_event(xSocket sock, xEventMask mask, void *arg) {
  xTcpPendingConn_ *pc = (xTcpPendingConn_ *)arg;
  (void)sock;

  if (!(mask & (xEvent_Read | xEvent_Write))) return;

  int result = pc->transport.handshake(pc->transport.ctx);
  switch (result) {
  case xTransportResult_Done: {
    /* Handshake complete: create xTcpConn and deliver */
    xSocket                 s        = pc->sock;
    xTransport              t        = pc->transport;
    struct sockaddr_storage addr     = pc->addr;
    socklen_t               addrlen  = pc->addrlen;
    xTcpListener_          *listener = pc->listener;

    /* Detach pending-conn callback before transferring ownership.
     * Prevents use-after-free if a stale event fires after pc is freed. */
    xSocketSetCallback(s, noop_sock_cb, NULL);
    xSocketSetMask(listener->loop, s, 0);

    /* Prevent pending_conn_destroy from closing these */
    pc->sock = NULL;
    memset(&pc->transport, 0, sizeof(pc->transport));

    xTcpConn conn = xTcpConnCreate_(s, t);
    if (!conn) {
      /* Failed to allocate: clean up */
      if (t.destroy) t.destroy(t.ctx);
      xSocketDestroy(listener->loop, s);
      free(pc);
      return;
    }

    free(pc);

    listener->callback((xTcpListener)listener, conn, (struct sockaddr *)&addr,
                       addrlen, listener->user_arg);
    break;
  }
  case xTransportResult_WantRead:
    xSocketSetMask(pc->listener->loop, pc->sock, xEvent_Read);
    break;
  case xTransportResult_WantWrite:
    xSocketSetMask(pc->listener->loop, pc->sock, xEvent_Write);
    break;
  default:
    xLog(false, "xnet: TLS handshake failed for accepted connection");
    pending_conn_destroy(pc);
    break;
  }
}

/* ═══════════════════════════════════════════════════════════════════
 *  Accept loop
 * ═══════════════════════════════════════════════════════════════════
 */

static void listener_on_event(xSocket sock, xEventMask mask, void *arg) {
  (void)sock;
  xTcpListener_ *l = (xTcpListener_ *)arg;

  if (!(mask & xEvent_Read)) return;

  /* Accept in a loop to drain all pending connections (edge-triggered) */
  for (;;) {
    struct sockaddr_storage client_addr;
    socklen_t               addr_len = sizeof(client_addr);
    int                     client_fd =
      accept(l->listen_fd, (struct sockaddr *)&client_addr, &addr_len);
    if (client_fd < 0) {
      if (errno == EAGAIN || errno == EWOULDBLOCK) break;
      if (errno == EMFILE || errno == ENFILE) {
        xLog(false, "xnet: accept() failed: %s (fd exhaustion)",
             strerror(errno));
        break;
      }
      break;
    }

    if (l->tls_ctx) {
      /* TLS mode: create pending connection for async handshake */
      xTcpPendingConn_ *pc =
        (xTcpPendingConn_ *)calloc(1, sizeof(xTcpPendingConn_));
      if (!pc) {
        close(client_fd);
        continue;
      }

      pc->listener = l;
      pc->fd       = client_fd;
      pc->addr     = client_addr;
      pc->addrlen  = addr_len;

      /* Initialize TLS server transport */
      xTransportTlsServerInit(&pc->transport, l->tls_ctx, client_fd);
      if (!pc->transport.read) {
        /* Init failed */
        close(client_fd);
        free(pc);
        continue;
      }

      /* Create xSocket for the accepted fd */
      pc->sock =
        xSocketCreateFromFd(l->loop, client_fd, xEvent_Read | xEvent_Write,
                            pending_conn_on_event, pc);
      if (!pc->sock) {
        if (pc->transport.destroy) pc->transport.destroy(pc->transport.ctx);
        close(client_fd);
        free(pc);
        continue;
      }

      /* Drive the first handshake attempt */
      int result = pc->transport.handshake(pc->transport.ctx);
      switch (result) {
      case xTransportResult_Done: {
        /* Handshake completed immediately (unlikely) */
        xSocket    s = pc->sock;
        xTransport t = pc->transport;

        /* Detach pending-conn callback before transferring ownership. */
        xSocketSetCallback(s, noop_sock_cb, NULL);
        xSocketSetMask(l->loop, s, 0);

        pc->sock     = NULL;
        memset(&pc->transport, 0, sizeof(pc->transport));

        xTcpConn conn = xTcpConnCreate_(s, t);
        if (!conn) {
          if (t.destroy) t.destroy(t.ctx);
          xSocketDestroy(l->loop, s);
          free(pc);
          continue;
        }
        free(pc);
        l->callback((xTcpListener)l, conn, (struct sockaddr *)&client_addr,
                    addr_len, l->user_arg);
        break;
      }
      case xTransportResult_WantRead:
        xSocketSetMask(l->loop, pc->sock, xEvent_Read);
        break;
      case xTransportResult_WantWrite:
        xSocketSetMask(l->loop, pc->sock, xEvent_Write);
        break;
      default:
        xLog(false, "xnet: TLS handshake failed for accepted connection");
        pending_conn_destroy(pc);
        break;
      }
    } else {
      /* Plain TCP mode: create xTcpConn immediately */
      xTransport transport;
      memset(&transport, 0, sizeof(transport));
      xTransportPlainInit(&transport, client_fd);
      if (!transport.read) {
        close(client_fd);
        continue;
      }

      xSocket client_sock = xSocketCreateFromFd(l->loop, client_fd, xEvent_Read,
                                                noop_sock_cb, NULL);
      if (!client_sock) {
        if (transport.destroy) transport.destroy(transport.ctx);
        close(client_fd);
        continue;
      }

      xTcpConn conn = xTcpConnCreate_(client_sock, transport);
      if (!conn) {
        if (transport.destroy) transport.destroy(transport.ctx);
        xSocketDestroy(l->loop, client_sock);
        continue;
      }

      l->callback((xTcpListener)l, conn, (struct sockaddr *)&client_addr,
                  addr_len, l->user_arg);
    }
  }
}

/* ═══════════════════════════════════════════════════════════════════
 *  Public API
 * ═══════════════════════════════════════════════════════════════════
 */

xTcpListener xTcpListenerCreate(xEventLoop loop, const char *host,
                                uint16_t port, const xTcpListenerConf *conf,
                                xTcpListenerFunc callback, void *arg) {
  if (!loop || !callback) return NULL;

  /* Ignore SIGPIPE */
  signal(SIGPIPE, SIG_IGN);

  /* Create listening socket */
  int fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) {
    xLog(false, "xnet: socket() failed: %s", strerror(errno));
    return NULL;
  }

  /* SO_REUSEADDR */
  int optval = 1;
  setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval));

  /* SO_REUSEPORT (optional) */
  if (conf && conf->reuseport) {
#ifdef SO_REUSEPORT
    setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &optval, sizeof(optval));
#endif
  }

  /* Bind */
  struct sockaddr_in addr;
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_port   = htons(port);

  if (host) {
    if (inet_pton(AF_INET, host, &addr.sin_addr) != 1) {
      xLog(false, "xnet: invalid bind address: %s", host);
      close(fd);
      return NULL;
    }
  } else {
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
  }

  if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
    xLog(false, "xnet: bind() failed: %s", strerror(errno));
    close(fd);
    return NULL;
  }

  int backlog =
    (conf && conf->backlog > 0) ? conf->backlog : XTCP_DEFAULT_BACKLOG;
  if (listen(fd, backlog) < 0) {
    xLog(false, "xnet: listen() failed: %s", strerror(errno));
    close(fd);
    return NULL;
  }

  /* Allocate listener */
  xTcpListener_ *l = (xTcpListener_ *)calloc(1, sizeof(xTcpListener_));
  if (!l) {
    close(fd);
    return NULL;
  }

  l->loop      = loop;
  l->listen_fd = fd;
  l->tls_ctx   = conf ? conf->tls_ctx : NULL;
  l->callback  = callback;
  l->user_arg  = arg;

  /* Register with event loop */
  l->listen_sock =
    xSocketCreateFromFd(loop, fd, xEvent_Read, listener_on_event, l);
  if (!l->listen_sock) {
    close(fd);
    free(l);
    return NULL;
  }

  return (xTcpListener)l;
}

void xTcpListenerDestroy(xTcpListener listener) {
  if (!listener) return;
  xTcpListener_ *l = (xTcpListener_ *)listener;

  if (l->listen_sock) {
    xSocketDestroy(l->loop, l->listen_sock);
    l->listen_sock = NULL;
    l->listen_fd   = -1; /* fd closed by xSocketDestroy */
  }

  free(l);
}
