/*
 * Copyright 2025 The moo Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * ws_connect.c - WebSocket client async connection state machine
 *
 * Implements xWsConnect(): URL parse → DNS → TCP → [TLS] →
 * HTTP Upgrade → xWsConn creation, all fully asynchronous.
 */

#include <x/net/transport_private.h>
#include "ws_handshake_client.h"
#include "ws_private.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <unistd.h>

#include <x/base/log.h>
#include <x/net/dns.h>
#include <x/net/url.h>

/* Default connect timeout: 10 seconds */
#define XWS_DEFAULT_TIMEOUT_MS 10000

/* Max HTTP response header size we'll buffer */
#define XWS_MAX_RESPONSE_SIZE 4096

/* ═══════════════════════════════════════════════════════════════════
 *  Connection phases
 * ═══════════════════════════════════════════════════════════════════
 */

XDEF_ENUM(xWsConnectPhase){
  xWsConnectPhase_Dns,
  xWsConnectPhase_TcpConnect,
  xWsConnectPhase_TlsHandshake,
  xWsConnectPhase_HttpUpgradeWrite,
  xWsConnectPhase_HttpUpgradeRead,
  xWsConnectPhase_Done,
};

/* ═══════════════════════════════════════════════════════════════════
 *  Connector state
 * ═══════════════════════════════════════════════════════════════════
 */

XDEF_STRUCT(xWsConnector) {
  xEventLoop     loop;
  xWsCallbacks   callbacks;
  void          *user_arg;

  /* URL */
  xUrl           url;
  int            use_tls;

  /* TLS config (may be NULL) */
  const xTlsConf *tls_conf;

  /* Pre-created TLS context from conf (may be NULL) */
  xTlsCtx        conf_tls_ctx;

  /* Resolved TLS context (set during handshake) */
  xTlsCtx        tls_ctx;
  int             owns_tls_ctx;

  /* Extra headers */
  const char    *headers;

  /* DNS */
  xDnsQuery      dns_query;
  xDnsResult    *dns_result;
  xDnsAddr      *current_addr; /* Current address being tried */

  /* Socket */
  xSocket        sock;
  int            fd;

  /* Transport */
  xTransport transport;

  /* Timeout */
  xEventTimer    timer;
  int            timeout_ms;

  /* HTTP Upgrade */
  xIOBuffer      write_buf;
  char           expected_accept[64];

  /* Response buffer */
  char           resp_buf[XWS_MAX_RESPONSE_SIZE];
  size_t         resp_len;

  /* Current phase */
  xWsConnectPhase phase;
};

/* ═══════════════════════════════════════════════════════════════════
 *  Forward declarations
 * ═══════════════════════════════════════════════════════════════════
 */

static void connector_destroy(xWsConnector *c);
static void connector_fail(xWsConnector *c, uint16_t code);
static void connector_dns_cb(xDnsResult *result, void *arg);
static void connector_sock_cb(xSocket sock, xEventMask mask,
                              void *arg);
static void connector_timeout_cb(void *arg);
static void connector_do_tcp_connect(xWsConnector *c);
static void connector_try_next_addr(xWsConnector *c);
static void connector_do_tls_handshake(xWsConnector *c);
static void connector_do_http_upgrade(xWsConnector *c);
static void connector_on_writable(xWsConnector *c);
static void connector_on_readable(xWsConnector *c);

/* ═══════════════════════════════════════════════════════════════════
 *  Cleanup
 * ═══════════════════════════════════════════════════════════════════
 */

static void connector_destroy(xWsConnector *c) {
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
  if (c->owns_tls_ctx && c->tls_ctx) {
    xTlsCtxDestroy(c->tls_ctx);
    c->tls_ctx = NULL;
  }
  xIOBufferDeinit(&c->write_buf);
  xUrlFree(&c->url);
  free(c);
}

static void connector_fail(xWsConnector *c, uint16_t code) {
  xWsCallbacks cbs = c->callbacks;
  void *arg = c->user_arg;
  connector_destroy(c);

  /* Fire on_close without ever calling on_open */
  if (cbs.on_close) {
    cbs.on_close(NULL, code, NULL, 0, arg);
  }
}

/* ═══════════════════════════════════════════════════════════════════
 *  Timeout
 * ═══════════════════════════════════════════════════════════════════
 */

static void connector_timeout_cb(void *arg) {
  xWsConnector *c = (xWsConnector *)arg;
  c->timer = NULL;
  connector_fail(c, XWS_CLOSE_ABNORMAL);
}

/* ═══════════════════════════════════════════════════════════════════
 *  DNS callback
 * ═══════════════════════════════════════════════════════════════════
 */

static void connector_dns_cb(xDnsResult *result, void *arg) {
  xWsConnector *c = (xWsConnector *)arg;
  c->dns_query = NULL;

  if (result->error != xErrno_Ok || !result->addrs) {
    xDnsResultFree(result);
    connector_fail(c, XWS_CLOSE_ABNORMAL);
    return;
  }

  c->dns_result = result;
  c->current_addr = result->addrs; /* Start with first address */
  c->phase = xWsConnectPhase_TcpConnect;
  connector_do_tcp_connect(c);
}

/* ═══════════════════════════════════════════════════════════════════
 *  TCP connect
 * ═══════════════════════════════════════════════════════════════════
 */

static void connector_try_next_addr(xWsConnector *c) {
  /* Destroy the failed socket before trying the next address */
  if (c->sock) {
    xSocketDestroy(c->loop, c->sock);
    c->sock = NULL;
  }

  /* Advance to next address */
  c->current_addr = c->current_addr->next;
  if (!c->current_addr) {
    /* No more addresses to try */
    connector_fail(c, XWS_CLOSE_ABNORMAL);
    return;
  }
  connector_do_tcp_connect(c);
}

static void connector_do_tcp_connect(xWsConnector *c) {
  xDnsAddr *addr = c->current_addr;

  /* Create socket */
  c->sock = xSocketCreate(c->loop, addr->family,
                          SOCK_STREAM, 0,
                          xEvent_Write,
                          connector_sock_cb, c);
  if (!c->sock) {
    connector_try_next_addr(c);
    return;
  }

  c->fd = xSocketFd(c->sock);

  /* Initiate async connect */
  int ret = connect(c->fd,
                    (struct sockaddr *)&addr->addr,
                    addr->addrlen);
  if (ret == 0) {
    /* Connected immediately (unlikely for TCP) */
    if (c->use_tls) {
      c->phase = xWsConnectPhase_TlsHandshake;
      connector_do_tls_handshake(c);
    } else {
      connector_do_http_upgrade(c);
    }
  } else if (errno == EINPROGRESS) {
    /* Normal: wait for writable event */
    /* Socket is already watching xEvent_Write */
  } else {
    connector_try_next_addr(c);
  }
}

/* ═══════════════════════════════════════════════════════════════════
 *  TLS handshake
 * ═══════════════════════════════════════════════════════════════════
 */

static void connector_do_tls_handshake(xWsConnector *c) {
  /* Resolve TLS context: prefer tls_ctx, fall back to creating from tls conf */
  if (!c->tls_ctx) {
    if (c->conf_tls_ctx) {
      c->tls_ctx      = c->conf_tls_ctx;
      c->owns_tls_ctx = 0;
    } else {
      /* Create from tls_conf (or defaults if NULL) */
      xTlsConf defaults;
      const xTlsConf *conf = c->tls_conf;
      if (!conf) {
        memset(&defaults, 0, sizeof(defaults));
        conf = &defaults;
      }
      c->tls_ctx = xTlsCtxCreate(conf);
      if (!c->tls_ctx) {
        connector_fail(c, XWS_CLOSE_ABNORMAL);
        return;
      }
      c->owns_tls_ctx = 1;
    }
  }

  /* Extract hostname as NUL-terminated string */
  char hostname[256];
  size_t hlen = c->url.host_len;
  if (hlen >= sizeof(hostname)) hlen = sizeof(hostname) - 1;
  memcpy(hostname, c->url.host, hlen);
  hostname[hlen] = '\0';

  if (xTransportTlsClientInit(&c->transport, c->tls_ctx,
                                  hostname, c->fd) < 0) {
    connector_fail(c, XWS_CLOSE_ABNORMAL);
    return;
  }

  /* Drive the handshake */
  int result = c->transport.handshake(c->transport.ctx);
  switch (result) {
  case xTransportResult_Done:
    connector_do_http_upgrade(c);
    break;
  case xTransportResult_WantRead:
    xSocketSetMask(c->loop, c->sock, xEvent_Read);
    break;
  case xTransportResult_WantWrite:
    xSocketSetMask(c->loop, c->sock, xEvent_Write);
    break;
  default:
    connector_fail(c, XWS_CLOSE_ABNORMAL);
    break;
  }
}

/* ═══════════════════════════════════════════════════════════════════
 *  HTTP Upgrade
 * ═══════════════════════════════════════════════════════════════════
 */

static void connector_do_http_upgrade(xWsConnector *c) {
  c->phase = xWsConnectPhase_HttpUpgradeWrite;

  /* Build the Upgrade request */
  if (xWsClientBuildUpgradeRequest(&c->write_buf, &c->url,
                                   c->headers,
                                   c->expected_accept,
                                   sizeof(c->expected_accept))
      < 0) {
    connector_fail(c, XWS_CLOSE_ABNORMAL);
    return;
  }

  /* Set up plain transport if not using TLS */
  if (!c->use_tls) {
    xTransportPlainInit(&c->transport, c->fd);
  }

  /* Try to write immediately */
  connector_on_writable(c);
}

static void connector_on_writable(xWsConnector *c) {
  if (c->phase == xWsConnectPhase_TcpConnect) {
    /* TCP connect completed: check for errors */
    int err = 0;
    socklen_t errlen = sizeof(err);
    getsockopt(c->fd, SOL_SOCKET, SO_ERROR, &err, &errlen);
    if (err != 0) {
      /* Try next address in the list */
      connector_try_next_addr(c);
      return;
    }

    if (c->use_tls) {
      c->phase = xWsConnectPhase_TlsHandshake;
      connector_do_tls_handshake(c);
    } else {
      connector_do_http_upgrade(c);
    }
    return;
  }

  if (c->phase == xWsConnectPhase_TlsHandshake) {
    int result = c->transport.handshake(c->transport.ctx);
    switch (result) {
    case xTransportResult_Done:
      connector_do_http_upgrade(c);
      break;
    case xTransportResult_WantRead:
      xSocketSetMask(c->loop, c->sock, xEvent_Read);
      break;
    case xTransportResult_WantWrite:
      xSocketSetMask(c->loop, c->sock, xEvent_Write);
      break;
    default:
      connector_fail(c, XWS_CLOSE_ABNORMAL);
      break;
    }
    return;
  }

  if (c->phase == xWsConnectPhase_HttpUpgradeWrite) {
    /* Flush the Upgrade request */
    while (!xIOBufferEmpty(&c->write_buf)) {
      struct iovec iov[16];
      int cnt = xIOBufferReadIov(&c->write_buf, iov, 16);
      if (cnt == 0) break;

      ssize_t n = c->transport.writev(c->transport.ctx,
                                      iov, cnt);
      if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
          xSocketSetMask(c->loop, c->sock, xEvent_Write);
          return;
        }
        connector_fail(c, XWS_CLOSE_ABNORMAL);
        return;
      }
      if (n > 0) xIOBufferConsume(&c->write_buf, (size_t)n);
    }

    /* Request fully sent: switch to reading response */
    c->phase = xWsConnectPhase_HttpUpgradeRead;
    c->resp_len = 0;
    xSocketSetMask(c->loop, c->sock, xEvent_Read);
    return;
  }
}

static void connector_on_readable(xWsConnector *c) {
  if (c->phase == xWsConnectPhase_TlsHandshake) {
    int result = c->transport.handshake(c->transport.ctx);
    switch (result) {
    case xTransportResult_Done:
      connector_do_http_upgrade(c);
      break;
    case xTransportResult_WantRead:
      xSocketSetMask(c->loop, c->sock, xEvent_Read);
      break;
    case xTransportResult_WantWrite:
      xSocketSetMask(c->loop, c->sock, xEvent_Write);
      break;
    default:
      connector_fail(c, XWS_CLOSE_ABNORMAL);
      break;
    }
    return;
  }

  if (c->phase == xWsConnectPhase_HttpUpgradeRead) {
    /* Read response data */
    size_t space = sizeof(c->resp_buf) - c->resp_len - 1;
    if (space == 0) {
      /* Response too large */
      connector_fail(c, XWS_CLOSE_PROTOCOL_ERR);
      return;
    }

    ssize_t n = c->transport.read(
      c->transport.ctx,
      c->resp_buf + c->resp_len, space);
    if (n < 0) {
      if (errno == EAGAIN || errno == EWOULDBLOCK) return;
      connector_fail(c, XWS_CLOSE_ABNORMAL);
      return;
    }
    if (n == 0) {
      /* EOF before complete response */
      connector_fail(c, XWS_CLOSE_ABNORMAL);
      return;
    }

    c->resp_len += (size_t)n;
    c->resp_buf[c->resp_len] = '\0';

    /* Check if we have the complete response headers */
    char *end = strstr(c->resp_buf, "\r\n\r\n");
    if (!end) return; /* Need more data */

    size_t hdr_end = (size_t)(end - c->resp_buf) + 4;

    /* Validate the 101 response */
    if (xWsClientValidateUpgradeResponse(
          c->resp_buf, hdr_end,
          c->expected_accept) < 0) {
      connector_fail(c, XWS_CLOSE_PROTOCOL_ERR);
      return;
    }

    /* Success! Create the xWsConn in client mode */
    c->phase = xWsConnectPhase_Done;

    /* Cancel timeout */
    if (c->timer) {
      xEventLoopTimerCancel(c->loop, c->timer);
      c->timer = NULL;
    }

    /* Transfer ownership of socket and transport */
    xSocket sock = c->sock;
    xTransport transport = c->transport;
    xEventLoop loop = c->loop;
    xWsCallbacks cbs = c->callbacks;
    void *arg = c->user_arg;

    /* Prevent connector_destroy from closing these */
    c->sock = NULL;
    memset(&c->transport, 0, sizeof(c->transport));

    /* Create WS connection (server=NULL → client mode) */
    struct xWsConn_ *ws = xWsConnCreate(
      NULL, loop, sock, transport,
      &cbs, arg, 0);

    if (!ws) {
      /* Restore and clean up */
      if (transport.destroy)
        transport.destroy(transport.ctx);
      xSocketDestroy(loop, sock);
      connector_destroy(c);
      if (cbs.on_close)
        cbs.on_close(NULL, XWS_CLOSE_ABNORMAL, NULL, 0, arg);
      return;
    }

    /* Transfer any extra data after the HTTP headers
     * into the WS read buffer */
    if (c->resp_len > hdr_end) {
      xIOBufferAppend(&ws->read_buf,
                      c->resp_buf + hdr_end,
                      c->resp_len - hdr_end);
    }

#ifdef XHTTP_WS_DEFLATE
    /* Check if server accepted permessage-deflate */
    {
      xWsDeflateParams dp;
      if (xWsClientParseDeflateResponse(
            c->resp_buf, hdr_end, &dp) == 0 &&
          dp.enabled) {
        ws->deflate_params = dp;
        ws->deflate_ctx = xWsDeflateCreate(&dp, 1);
        if (ws->deflate_ctx) {
          ws->parser.allow_rsv1 = 1;
        }
      }
    }
#endif

    /* Clean up connector (socket/transport already transferred) */
    connector_destroy(c);

    /* Fire on_open */
    if (ws->callbacks.on_open) {
      ws->callbacks.on_open((xWsConn)ws, ws->user_arg);
    }
    return;
  }
}

/* ═══════════════════════════════════════════════════════════════════
 *  Socket event handler
 * ═══════════════════════════════════════════════════════════════════
 */

static void connector_sock_cb(xSocket sock, xEventMask mask,
                              void *arg) {
  xWsConnector *c = (xWsConnector *)arg;
  (void)sock;

  if (mask & xEvent_Write) {
    connector_on_writable(c);
  }
  if (mask & xEvent_Read) {
    connector_on_readable(c);
  }
}

/* ═══════════════════════════════════════════════════════════════════
 *  Public API: xWsConnect
 * ═══════════════════════════════════════════════════════════════════
 */

xErrno xWsConnect(xEventLoop loop,
                  const xWsConnectConf *conf,
                  const xWsCallbacks *callbacks,
                  void *arg) {
  if (!loop || !conf || !callbacks) return xErrno_InvalidArg;
  if (!conf->url) return xErrno_InvalidArg;

  /* Parse URL */
  xUrl url;
  memset(&url, 0, sizeof(url));
  if (xUrlParse(conf->url, &url) != xErrno_Ok)
    return xErrno_InvalidArg;

  /* Validate scheme */
  int use_tls = 0;
  if (url.scheme_len == 2 &&
      strncmp(url.scheme, "ws", 2) == 0) {
    use_tls = 0;
  } else if (url.scheme_len == 3 &&
             strncmp(url.scheme, "wss", 3) == 0) {
    use_tls = 1;
  } else {
    xUrlFree(&url);
    return xErrno_InvalidArg;
  }

  /* Allocate connector */
  xWsConnector *c =
    (xWsConnector *)calloc(1, sizeof(xWsConnector));
  if (!c) {
    xUrlFree(&url);
    return xErrno_NoMemory;
  }

  c->loop      = loop;
  c->callbacks = *callbacks;
  c->user_arg  = arg;
  c->url       = url; /* Transfer ownership */
  c->use_tls   = use_tls;
  c->tls_conf     = conf->tls;
  c->conf_tls_ctx  = conf->tls_ctx;
  c->headers       = conf->headers;
  c->phase     = xWsConnectPhase_Dns;

  xIOBufferInit(&c->write_buf);

  /* Set timeout */
  c->timeout_ms = conf->timeout_ms > 0
                    ? conf->timeout_ms
                    : XWS_DEFAULT_TIMEOUT_MS;
  c->timer = xEventLoopTimerAfter(
    loop, connector_timeout_cb, c,
    (uint64_t)c->timeout_ms);

  /* Extract hostname for DNS */
  char hostname[256];
  size_t hlen = c->url.host_len;
  if (hlen >= sizeof(hostname)) hlen = sizeof(hostname) - 1;
  memcpy(hostname, c->url.host, hlen);
  hostname[hlen] = '\0';

  /* Get port string */
  char port_str[8];
  uint16_t port = xUrlPort(&c->url);
  snprintf(port_str, sizeof(port_str), "%u", port);

  /* Start DNS resolution */
  struct addrinfo hints;
  memset(&hints, 0, sizeof(hints));
  hints.ai_family   = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;

  c->dns_query = xDnsResolve(loop, hostname, port_str,
                             &hints, connector_dns_cb, c);
  if (!c->dns_query) {
    connector_destroy(c);
    return xErrno_SysError;
  }

  return xErrno_Ok;
}
