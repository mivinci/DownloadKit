/*
 * Copyright 2025 The xKit Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * tcp_connect.c - Async TCP connector state machine
 *
 * Implements xTcpConnect(): DNS resolve → socket create →
 * non-blocking connect → [TLS handshake] → callback.
 */

#include "tcp_private.h"
#include "transport_private.h"

#include <errno.h>
#include <netinet/tcp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include <xbase/log.h>
#include <xnet/dns.h>

/* Default connect timeout: 10 seconds */
#define XTCP_DEFAULT_TIMEOUT_MS 10000

/* ═══════════════════════════════════════════════════════════════════
 *  Connection phases
 * ═══════════════════════════════════════════════════════════════════
 */

XDEF_ENUM(xTcpConnectPhase_){
  xTcpConnectPhase_Dns,
  xTcpConnectPhase_TcpConnect,
  xTcpConnectPhase_TlsHandshake,
  xTcpConnectPhase_Done,
};

/* ═══════════════════════════════════════════════════════════════════
 *  Connector state
 * ═══════════════════════════════════════════════════════════════════
 */

XDEF_STRUCT(xTcpConnector_) {
  xEventLoop      loop;
  xTcpConnectFunc callback;
  void           *user_arg;

  /* Config */
  xTcpConnectConf conf;
  char           *host;
  uint16_t        port;

  /* DNS */
  xDnsQuery   dns_query;
  xDnsResult *dns_result;

  /* Socket */
  xSocket sock;
  int     fd;

  /* Transport */
  xTransport transport;

  /* Timeout */
  xEventTimer timer;

  /* Current phase */
  xTcpConnectPhase_ phase;
};

/* ═══════════════════════════════════════════════════════════════════
 *  Forward declarations
 * ═══════════════════════════════════════════════════════════════════
 */

static void connector_destroy(xTcpConnector_ *c);
static void connector_fail(xTcpConnector_ *c, xErrno err);
static void connector_succeed(xTcpConnector_ *c);
static void connector_dns_cb(xDnsResult *result, void *arg);
static void connector_sock_cb(xSocket sock, xEventMask mask, void *arg);
static void connector_timeout_cb(void *arg);
static void connector_do_tcp_connect(xTcpConnector_ *c);
static void connector_do_tls_handshake(xTcpConnector_ *c);
static void connector_on_writable(xTcpConnector_ *c);
static void connector_on_readable(xTcpConnector_ *c);

/* No-op callback to detach connector from socket before ownership transfer */
static void noop_sock_cb(xSocket sock, xEventMask mask, void *arg) {
  (void)sock;
  (void)mask;
  (void)arg;
}

/* ═══════════════════════════════════════════════════════════════════
 *  Cleanup
 * ═══════════════════════════════════════════════════════════════════
 */

static void connector_destroy(xTcpConnector_ *c) {
  if (!c) return;

  if (c->timer) {
    xEventLoopTimerCancel(c->loop, c->timer);
    c->timer = NULL;
  }
  if (c->dns_query) {
    xDnsCancel(c->loop, c->dns_query);
    c->dns_query = NULL;
  }
  if (c->dns_result) {
    xDnsResultFree(c->dns_result);
    c->dns_result = NULL;
  }
  if (c->transport.destroy) {
    c->transport.destroy(c->transport.ctx);
    memset(&c->transport, 0, sizeof(c->transport));
  }
  if (c->sock) {
    xSocketDestroy(c->loop, c->sock);
    c->sock = NULL;
  }
  free(c->host);
  free(c);
}

static void connector_fail(xTcpConnector_ *c, xErrno err) {
  xTcpConnectFunc cb  = c->callback;
  void           *arg = c->user_arg;
  connector_destroy(c);
  cb(NULL, err, arg);
}

static void connector_succeed(xTcpConnector_ *c) {
  /* Apply socket options */
  if (c->conf.nodelay) {
    int val = 1;
    setsockopt(c->fd, IPPROTO_TCP, TCP_NODELAY, &val, sizeof(val));
  }
  if (c->conf.keepalive) {
    int val = 1;
    setsockopt(c->fd, SOL_SOCKET, SO_KEEPALIVE, &val, sizeof(val));
  }

  /* Cancel timeout */
  if (c->timer) {
    xEventLoopTimerCancel(c->loop, c->timer);
    c->timer = NULL;
  }

  /* Detach connector callback before transferring ownership.
   * The socket still references connector_sock_cb(arg=c), but c will
   * be freed below.  Replace with a no-op to prevent use-after-free
   * if the event loop dispatches a stale event in the same iteration. */
  xSocketSetCallback(c->sock, noop_sock_cb, NULL);
  xSocketSetMask(c->loop, c->sock, 0);

  /* Transfer socket and transport to xTcpConn */
  xSocket    sock      = c->sock;
  xTransport transport = c->transport;
  c->sock              = NULL;
  memset(&c->transport, 0, sizeof(c->transport));

  xTcpConn conn = xTcpConnCreate_(sock, transport);
  if (!conn) {
    /* Restore and clean up */
    if (transport.destroy) transport.destroy(transport.ctx);
    xSocketDestroy(c->loop, sock);
    xTcpConnectFunc cb  = c->callback;
    void           *arg = c->user_arg;
    connector_destroy(c);
    cb(NULL, xErrno_NoMemory, arg);
    return;
  }

  xTcpConnectFunc cb  = c->callback;
  void           *arg = c->user_arg;
  connector_destroy(c);
  cb(conn, xErrno_Ok, arg);
}

/* ═══════════════════════════════════════════════════════════════════
 *  Timeout
 * ═══════════════════════════════════════════════════════════════════
 */

static void connector_timeout_cb(void *arg) {
  xTcpConnector_ *c = (xTcpConnector_ *)arg;
  c->timer          = NULL;
  connector_fail(c, xErrno_Timeout);
}

/* ═══════════════════════════════════════════════════════════════════
 *  DNS callback
 * ═══════════════════════════════════════════════════════════════════
 */

static void connector_dns_cb(xDnsResult *result, void *arg) {
  xTcpConnector_ *c = (xTcpConnector_ *)arg;
  c->dns_query      = NULL;

  if (result->error != xErrno_Ok || !result->addrs) {
    xErrno err = result->error != xErrno_Ok ? result->error : xErrno_DnsError;
    xDnsResultFree(result);
    connector_fail(c, err);
    return;
  }

  c->dns_result = result;
  c->phase      = xTcpConnectPhase_TcpConnect;
  connector_do_tcp_connect(c);
}

/* ═══════════════════════════════════════════════════════════════════
 *  TCP connect
 * ═══════════════════════════════════════════════════════════════════
 */

static void connector_do_tcp_connect(xTcpConnector_ *c) {
  xDnsAddr *addr = c->dns_result->addrs;

  /* Create socket */
  c->sock = xSocketCreate(c->loop, addr->family, SOCK_STREAM, 0, xEvent_Write,
                          connector_sock_cb, c);
  if (!c->sock) {
    connector_fail(c, xErrno_SysError);
    return;
  }

  c->fd = xSocketFd(c->sock);

  /* Initiate async connect */
  int ret = connect(c->fd, (struct sockaddr *)&addr->addr, addr->addrlen);
  if (ret == 0) {
    /* Connected immediately (unlikely for TCP) */
    if (c->conf.tls) {
      c->phase = xTcpConnectPhase_TlsHandshake;
      connector_do_tls_handshake(c);
    } else {
      xTransportPlainInit(&c->transport, c->fd);
      if (!c->transport.read) {
        connector_fail(c, xErrno_NoMemory);
        return;
      }
      connector_succeed(c);
    }
  } else if (errno == EINPROGRESS) {
    /* Normal: wait for writable event */
  } else {
    connector_fail(c, xErrno_SysError);
  }
}

/* ═══════════════════════════════════════════════════════════════════
 *  TLS handshake
 * ═══════════════════════════════════════════════════════════════════
 */

static void connector_do_tls_handshake(xTcpConnector_ *c) {
  if (xTransportTlsClientInit(&c->transport, c->conf.tls, c->host, c->fd) < 0) {
    connector_fail(c, xErrno_SysError);
    return;
  }

  /* Drive the handshake */
  int result = c->transport.handshake(c->transport.ctx);
  switch (result) {
  case xTransportResult_Done:
    connector_succeed(c);
    break;
  case xTransportResult_WantRead:
    xSocketSetMask(c->loop, c->sock, xEvent_Read);
    break;
  case xTransportResult_WantWrite:
    xSocketSetMask(c->loop, c->sock, xEvent_Write);
    break;
  default:
    connector_fail(c, xErrno_SysError);
    break;
  }
}

/* ═══════════════════════════════════════════════════════════════════
 *  Socket event handlers
 * ═══════════════════════════════════════════════════════════════════
 */

static void connector_on_writable(xTcpConnector_ *c) {
  if (c->phase == xTcpConnectPhase_TcpConnect) {
    /* TCP connect completed: check for errors */
    int       err    = 0;
    socklen_t errlen = sizeof(err);
    getsockopt(c->fd, SOL_SOCKET, SO_ERROR, &err, &errlen);
    if (err != 0) {
      connector_fail(c, xErrno_SysError);
      return;
    }

    if (c->conf.tls) {
      c->phase = xTcpConnectPhase_TlsHandshake;
      connector_do_tls_handshake(c);
    } else {
      xTransportPlainInit(&c->transport, c->fd);
      if (!c->transport.read) {
        connector_fail(c, xErrno_NoMemory);
        return;
      }
      connector_succeed(c);
    }
    return;
  }

  if (c->phase == xTcpConnectPhase_TlsHandshake) {
    int result = c->transport.handshake(c->transport.ctx);
    switch (result) {
    case xTransportResult_Done:
      connector_succeed(c);
      break;
    case xTransportResult_WantRead:
      xSocketSetMask(c->loop, c->sock, xEvent_Read);
      break;
    case xTransportResult_WantWrite:
      xSocketSetMask(c->loop, c->sock, xEvent_Write);
      break;
    default:
      connector_fail(c, xErrno_SysError);
      break;
    }
    return;
  }
}

static void connector_on_readable(xTcpConnector_ *c) {
  if (c->phase == xTcpConnectPhase_TlsHandshake) {
    int result = c->transport.handshake(c->transport.ctx);
    switch (result) {
    case xTransportResult_Done:
      connector_succeed(c);
      break;
    case xTransportResult_WantRead:
      xSocketSetMask(c->loop, c->sock, xEvent_Read);
      break;
    case xTransportResult_WantWrite:
      xSocketSetMask(c->loop, c->sock, xEvent_Write);
      break;
    default:
      connector_fail(c, xErrno_SysError);
      break;
    }
    return;
  }
}

static void connector_sock_cb(xSocket sock, xEventMask mask, void *arg) {
  xTcpConnector_ *c = (xTcpConnector_ *)arg;
  (void)sock;

  if (mask & xEvent_Write) connector_on_writable(c);
  if (mask & xEvent_Read) connector_on_readable(c);
}

/* ═══════════════════════════════════════════════════════════════════
 *  Public API: xTcpConnect
 * ═══════════════════════════════════════════════════════════════════
 */

xErrno xTcpConnect(xEventLoop loop, const char *host, uint16_t port,
                   const xTcpConnectConf *conf, xTcpConnectFunc callback,
                   void *arg) {
  if (!loop || !host || !callback) return xErrno_InvalidArg;

  xTcpConnector_ *c = (xTcpConnector_ *)calloc(1, sizeof(xTcpConnector_));
  if (!c) return xErrno_NoMemory;

  c->loop     = loop;
  c->callback = callback;
  c->user_arg = arg;
  c->port     = port;
  c->phase    = xTcpConnectPhase_Dns;

  /* Copy host string */
  c->host = strdup(host);
  if (!c->host) {
    free(c);
    return xErrno_NoMemory;
  }

  /* Copy config (or use defaults) */
  if (conf) {
    c->conf = *conf;
  } else {
    memset(&c->conf, 0, sizeof(c->conf));
  }

  /* Set timeout */
  int timeout_ms =
    c->conf.timeout_ms > 0 ? c->conf.timeout_ms : XTCP_DEFAULT_TIMEOUT_MS;
  c->timer =
    xEventLoopTimerAfter(loop, connector_timeout_cb, c, (uint64_t)timeout_ms);

  /* Start DNS resolution */
  char port_str[8];
  snprintf(port_str, sizeof(port_str), "%u", port);

  struct addrinfo hints;
  memset(&hints, 0, sizeof(hints));
  hints.ai_family   = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;

  c->dns_query = xDnsResolve(loop, host, port_str, &hints, connector_dns_cb, c);
  if (!c->dns_query) {
    connector_destroy(c);
    return xErrno_SysError;
  }

  return xErrno_Ok;
}
