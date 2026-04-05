/*
 * Copyright 2025 The xKit Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * server.c - Asynchronous HTTP/1.1 server implementation
 */

#include "proto_h1.h"
#include "proto_h2.h"
#include "server_private.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <xbase/log.h>
#include <sys/socket.h>
#include <unistd.h>

/* ═══════════════════════════════════════════════════════════════════════════
 *  Forward declarations
 * ═══════════════════════════════════════════════════════════════════════════
 */

static void on_listen_event(xSocket sock, xEventMask mask, void *arg);
static void on_conn_event(xSocket sock, xEventMask mask, void *arg);

/* Internal helpers */
static void conn_init_parser(struct xHttpConn_ *conn);
static void conn_reset_request_state(struct xHttpConn_ *conn);
static void conn_dispatch_request(struct xHttpConn_ *conn);
static void conn_write_ready(struct xHttpConn_ *conn);
static void conn_after_response(struct xHttpConn_ *conn);
static void conn_try_flush(struct xHttpConn_ *conn);
static void route_free_segments(struct xHttpRouteSegment_ *segs, int count);
int         xHttpConnFlushWriteInternal(struct xHttpConn_ *conn);

/* ═══════════════════════════════════════════════════════════════════════════
 *  HTTP status reason phrases
 * ═══════════════════════════════════════════════════════════════════════════
 */

const char *xHttpStatusReason(int code) {
  switch (code) {
  case 200:
    return "OK";
  case 201:
    return "Created";
  case 204:
    return "No Content";
  case 301:
    return "Moved Permanently";
  case 302:
    return "Found";
  case 304:
    return "Not Modified";
  case 400:
    return "Bad Request";
  case 403:
    return "Forbidden";
  case 404:
    return "Not Found";
  case 405:
    return "Method Not Allowed";
  case 408:
    return "Request Timeout";
  case 413:
    return "Content Too Large";
  case 431:
    return "Request Header Fields Too Large";
  case 500:
    return "Internal Server Error";
  case 502:
    return "Bad Gateway";
  case 503:
    return "Service Unavailable";
  default:
    return "Unknown";
  }
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  Server lifecycle (Task 4)
 * ═══════════════════════════════════════════════════════════════════════════
 */

xHttpServer xHttpServerCreate(xEventLoop loop) {
  if (!loop) return NULL;

  struct xHttpServer_ *s = (struct xHttpServer_ *)calloc(1, sizeof(*s));
  if (!s) return NULL;

  s->loop            = loop;
  s->listen_sock     = NULL;
  s->listen_fd       = -1;
  s->routes          = NULL;
  s->routes_tail     = NULL;
  s->conns           = NULL;
  s->idle_timeout_ms = XHTTP_DEFAULT_IDLE_TIMEOUT_MS;
  s->max_header_size = XHTTP_DEFAULT_MAX_HEADER_SIZE;
  s->max_body_size   = XHTTP_DEFAULT_MAX_BODY_SIZE;

  return (xHttpServer)s;
}

xErrno xHttpServerListen(xHttpServer server, const char *host, uint16_t port) {
  if (!server) return xErrno_InvalidArg;
  struct xHttpServer_ *s = (struct xHttpServer_ *)server;

  /* Create listening socket */
  int fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) return xErrno_SysError;

  /* SO_REUSEADDR */
  int optval = 1;
  setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval));

  /* Bind */
  struct sockaddr_in addr;
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_port   = htons(port);

  if (host) {
    if (inet_pton(AF_INET, host, &addr.sin_addr) != 1) {
      close(fd);
      return xErrno_InvalidArg;
    }
  } else {
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
  }

  if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
    close(fd);
    return xErrno_SysError;
  }

  if (listen(fd, SOMAXCONN) < 0) {
    close(fd);
    return xErrno_SysError;
  }

  /* Wrap in xSocket (sets non-blocking + registers with event loop) */
  xSocket sock =
    xSocketCreateFromFd(s->loop, fd, xEvent_Read, on_listen_event, s);
  if (!sock) {
    close(fd);
    return xErrno_SysError;
  }

  s->listen_sock = sock;
  s->listen_fd   = fd;

  return xErrno_Ok;
}

void xHttpServerDestroy(xHttpServer server) {
  if (!server) return;
  struct xHttpServer_ *s = (struct xHttpServer_ *)server;

  /* Close all active connections */
  while (s->conns) {
    xHttpConnClose(s->conns);
  }

  /* Close listening socket */
  if (s->listen_sock) {
    xSocketDestroy(s->loop, s->listen_sock);
    s->listen_sock = NULL;
    s->listen_fd   = -1;
  }

  /* Free routes */
  struct xHttpRoute_ *r = s->routes;
  while (r) {
    struct xHttpRoute_ *next = r->next;
    free((void *)r->method);
    free((void *)r->path);
    route_free_segments(r->segments, r->segment_count);
    free(r);
    r = next;
  }

  free(s);
}

/* ── Configuration ─────────────────────────────────────────────────────── */

xErrno xHttpServerSetIdleTimeout(xHttpServer server, int timeout_ms) {
  if (!server) return xErrno_InvalidArg;
  if (timeout_ms <= 0) return xErrno_InvalidArg;
  struct xHttpServer_ *s = (struct xHttpServer_ *)server;
  s->idle_timeout_ms     = timeout_ms;
  return xErrno_Ok;
}

xErrno xHttpServerSetMaxHeaderSize(xHttpServer server, size_t max_size) {
  if (!server) return xErrno_InvalidArg;
  if (max_size == 0) return xErrno_InvalidArg;
  struct xHttpServer_ *s = (struct xHttpServer_ *)server;
  s->max_header_size     = max_size;
  return xErrno_Ok;
}

xErrno xHttpServerSetMaxBodySize(xHttpServer server, size_t max_size) {
  if (!server) return xErrno_InvalidArg;
  if (max_size == 0) return xErrno_InvalidArg;
  struct xHttpServer_ *s = (struct xHttpServer_ *)server;
  s->max_body_size       = max_size;
  return xErrno_Ok;
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  Connection accept & management (Task 5)
 * ═══════════════════════════════════════════════════════════════════════════
 */

/**
 * Accept callback: called when the listening socket has a new connection.
 */
static void on_listen_event(xSocket sock, xEventMask mask, void *arg) {
  (void)sock;
  struct xHttpServer_ *s = (struct xHttpServer_ *)arg;

  if (!(mask & xEvent_Read)) return;

  /* Accept in a loop to drain all pending connections (edge-triggered) */
  for (;;) {
    struct sockaddr_in client_addr;
    socklen_t          addr_len = sizeof(client_addr);
    int                client_fd =
      accept(s->listen_fd, (struct sockaddr *)&client_addr, &addr_len);
    if (client_fd < 0) {
      if (errno == EAGAIN || errno == EWOULDBLOCK) break;
      if (errno == EMFILE || errno == ENFILE) {
        /* fd exhaustion: log warning and continue */
        xLog(false, "xhttp: accept() failed: %s (fd exhaustion)",
             strerror(errno));
        break;
      }
      /* Other errors: stop accepting this round */
      break;
    }

    /* Create connection */
    struct xHttpConn_ *conn =
      (struct xHttpConn_ *)calloc(1, sizeof(struct xHttpConn_));
    if (!conn) {
      close(client_fd);
      continue;
    }

    conn->server = s;
    xIOBufferInit(&conn->read_buf);
    xIOBufferInit(&conn->write_buf);
    conn->keep_alive = 1; /* HTTP/1.1 default */
    conn->writing    = 0;

    /* Initialize protocol handler (also creates the implicit stream) */
    conn_init_parser(conn);

    /* Wrap accepted fd in xSocket */
    xSocket client_sock =
      xSocketCreateFromFd(s->loop, client_fd, xEvent_Read, on_conn_event, conn);
    if (!client_sock) {
      xIOBufferDeinit(&conn->read_buf);
      xIOBufferDeinit(&conn->write_buf);
      free(conn);
      close(client_fd);
      continue;
    }
    conn->sock = client_sock;

    /* Set idle timeout */
    if (s->idle_timeout_ms > 0) {
      xSocketSetTimeout(conn->sock, s->idle_timeout_ms, 0);
    }

    /* Add to active connections list */
    conn->prev = NULL;
    conn->next = s->conns;
    if (s->conns) s->conns->prev = conn;
    s->conns = conn;
  }
}

/**
 * Create a new stream for a connection.
 * For H1, stream_id is always 0 (implicit stream).
 * For H2, stream_id is assigned by nghttp2.
 */
struct xHttpStream_ *xHttpStreamCreate(struct xHttpConn_ *conn,
                                       int32_t            stream_id) {
  struct xHttpStream_ *stream =
    (struct xHttpStream_ *)calloc(1, sizeof(struct xHttpStream_));
  if (!stream) return NULL;

  stream->conn      = conn;
  stream->stream_id = stream_id;

  /* Initialize response writer defaults */
  stream->writer.status_code  = 200;
  stream->writer.headers      = NULL;
  stream->writer.headers_tail = NULL;
  stream->writer.sent         = 0;
  stream->writer.streaming    = 0;
  stream->writer.stream       = stream;

  return stream;
}

/**
 * Destroy a stream, freeing all its resources.
 */
void xHttpStreamDestroy(struct xHttpStream_ *stream) {
  if (!stream) return;

  /* Free request parsing state */
  xBufferDestroy(stream->url);
  xBufferDestroy(stream->header_field);
  xBufferDestroy(stream->headers_raw);
  xBufferDestroy(stream->body);

  /* Free response headers */
  struct xHttpHeader_ *h = stream->writer.headers;
  while (h) {
    struct xHttpHeader_ *next = h->next;
    free(h->key);
    free(h->value);
    free(h);
    h = next;
  }

  free(stream);
}

/**
 * Reset a stream for reuse (keep-alive).
 * Resets request parsing state and response writer, but keeps the stream alive.
 */
void xHttpStreamReset(struct xHttpStream_ *stream) {
  if (!stream) return;

  xBufferReset(stream->url);
  xBufferReset(stream->header_field);
  xBufferReset(stream->headers_raw);
  xBufferReset(stream->body);

  stream->header_bytes         = 0;
  stream->request_complete     = 0;
  stream->pending_error        = 0;
  stream->pending_error_reason = NULL;

  /* Reset response writer */
  struct xHttpHeader_ *h = stream->writer.headers;
  while (h) {
    struct xHttpHeader_ *next = h->next;
    free(h->key);
    free(h->value);
    free(h);
    h = next;
  }
  stream->writer.status_code  = 200;
  stream->writer.headers      = NULL;
  stream->writer.headers_tail = NULL;
  stream->writer.sent         = 0;
  stream->writer.streaming    = 0;
}

/**
 * Initialize the protocol handler for a connection.
 * Protocol detection is deferred until first data arrives.
 */
static void conn_init_parser(struct xHttpConn_ *conn) {
  conn->proto_detected = 0;
  /* Zero out the vtable; will be populated after protocol detection */
  memset(&conn->proto, 0, sizeof(conn->proto));
}

/**
 * Reset per-request parsing state (for keep-alive reuse).
 */
static void conn_reset_request_state(struct xHttpConn_ *conn) {
  if (conn->stream) {
    xHttpStreamReset(conn->stream);
  }

  /* Reset parser for next request */
  conn->proto.reset(conn);
}

void xHttpConnResetParser(struct xHttpConn_ *conn) {
  conn_reset_request_state(conn);
}

/**
 * Close and free a connection, removing it from the server's list.
 */
void xHttpConnClose(struct xHttpConn_ *conn) {
  if (!conn) return;
  struct xHttpServer_ *s = conn->server;

  /* Remove from doubly-linked list */
  if (conn->prev)
    conn->prev->next = conn->next;
  else
    s->conns = conn->next;
  if (conn->next) conn->next->prev = conn->prev;

  /* Destroy socket */
  if (conn->sock) {
    xSocketDestroy(s->loop, conn->sock);
    conn->sock = NULL;
  }

  /* Free buffers */
  xIOBufferDeinit(&conn->read_buf);
  xIOBufferDeinit(&conn->write_buf);

  /* Destroy protocol handler (only if initialized) */
  if (conn->proto_detected && conn->proto.destroy) {
    conn->proto.destroy(conn);
  }

  /* Destroy stream (frees request buffers and response headers) */
  if (conn->stream) {
    xHttpStreamDestroy(conn->stream);
    conn->stream = NULL;
  }

  free(conn);
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  Connection I/O event handler
 * ═══════════════════════════════════════════════════════════════════════════
 */

static void on_conn_event(xSocket sock, xEventMask mask, void *arg) {
  struct xHttpConn_ *conn = (struct xHttpConn_ *)arg;
  (void)sock;

  /* Idle timeout */
  if (mask & xEvent_Timeout) {
    xHttpConnClose(conn);
    return;
  }

  /* Writable: flush pending write data */
  if (mask & xEvent_Write) {
    conn_write_ready(conn);
  }

  /* Readable: read data and feed to parser */
  if (mask & xEvent_Read) {
    /*
     * NOTE: We intentionally read only one chunk per event instead of
     * draining the socket until EAGAIN. A full drain loop would be the
     * textbook approach for edge-triggered I/O, but the current
     * downstream code has two issues that make it unsafe:
     *
     *  1. Memory: all data is linearized into a malloc'd buffer before
     *     being fed to the parser, so draining a large request body
     *     would cause peak memory usage of ~2× body size.
     *
     *  2. Consumption: xIOBufferConsume() discards the entire read_buf
     *     after on_data(), but the parser (llhttp / nghttp2) may only
     *     consume a portion of it (e.g. H1 pipelining, partial H2
     *     frames). Draining amplifies this data-loss risk.
     *
     * Until the feed path is refactored to parse incrementally (read a
     * chunk → feed → consume only what was parsed → repeat), we keep
     * the single-read approach. This means a request whose body exceeds
     * XIOBUFFER_BLOCK_SIZE (8 KB) may require multiple event-loop
     * iterations to be fully received under edge-triggered mode.
     */
    for (;;) {
      ssize_t n = xIOBufferReadFd(&conn->read_buf, xSocketFd(conn->sock));
      if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) break;
        /* Read error: close connection */
        xHttpConnClose(conn);
        return;
      }
      if (n == 0) {
        /* Client closed connection */
        xHttpConnClose(conn);
        return;
      }
      break; /* Got data; process below (see NOTE above) */
    }

    /* Feed accumulated data to protocol handler */
    size_t buf_len = xIOBufferLen(&conn->read_buf);
    if (buf_len > 0) {
      /* Protocol auto-detection (Prior Knowledge) */
      if (!conn->proto_detected) {
        /* HTTP/2 connection preface: 24 bytes "PRI *
         * HTTP/2.0\r\n\r\nSM\r\n\r\n" */
        static const char   h2_magic[]   = "PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n";
        static const size_t h2_magic_len = 24;

        /* Copy data to linear buffer for inspection */
        char *peek = (char *)malloc(buf_len);
        if (!peek) {
          xHttpConnClose(conn);
          return;
        }
        xIOBufferCopyTo(&conn->read_buf, peek);

        if (buf_len >= h2_magic_len) {
          /* Enough data to decide */
          if (memcmp(peek, h2_magic, h2_magic_len) == 0) {
            xHttpProtoH2Init(conn);
          } else {
            xHttpProtoH1Init(conn);
          }
          conn->proto_detected = 1;
        } else {
          /* Not enough data yet; check if prefix still matches */
          if (memcmp(peek, h2_magic, buf_len) == 0) {
            /* Could still be H2; wait for more data */
            free(peek);
            return;
          } else {
            /* Definitely not H2; use H1 */
            xHttpProtoH1Init(conn);
            conn->proto_detected = 1;
          }
        }
        free(peek);
      }

      /* Copy read buffer to a contiguous buffer for parsing */
      buf_len      = xIOBufferLen(&conn->read_buf);
      char *linear = (char *)malloc(buf_len);
      if (!linear) {
        xHttpConnSendError(conn, 500, "Internal Server Error");
        conn_after_response(conn);
        return;
      }
      xIOBufferCopyTo(&conn->read_buf, linear);
      xIOBufferConsume(&conn->read_buf, buf_len);

      int rc = conn->proto.on_data(conn, linear, buf_len);
      free(linear);

      if (rc < 0) {
        /* Parse error or deferred error from callbacks */
        if (conn->stream && conn->stream->pending_error) {
          int         code            = conn->stream->pending_error;
          const char *reason          = conn->stream->pending_error_reason;
          conn->stream->pending_error = 0;
          conn->stream->pending_error_reason = NULL;
          xHttpConnSendError(conn, code, reason);
        } else {
          xHttpConnSendError(conn, 400, "Bad Request");
        }
        conn_after_response(conn);
        return;
      }

      if (rc > 0) {
        /* Request complete: dispatch */
        conn->stream->request_complete = 0;
        conn_dispatch_request(conn);
        /* conn may have been freed by dispatch (e.g. Connection: close),
         * so we must not access conn after this point. */
        return;
      }

      /* rc == 0: need more data, continue */
    }
  }
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  Routing (Task 7)
 *
 *  Current implementation: segment-by-segment matching on a linked list.
 *  Supports exact segments and ":param" segments (e.g. "/users/:id").
 *
 *  TODO: If the number of routes becomes a bottleneck, replace the
 *  linear scan with a radix tree (compressed trie) for O(path-length)
 *  lookup.  The public API (xHttpServerRoute / xHttpRequestParam) is
 *  designed to be compatible with such an upgrade.
 * ═══════════════════════════════════════════════════════════════════════════
 */

/**
 * Parse a route pattern like "/users/:id/posts" into an array of segments.
 * Returns the number of segments, or -1 on error.
 * Caller must free the returned array (each segment's text/param is strdup'd).
 */
static int route_parse_segments(const char                 *path,
                                struct xHttpRouteSegment_ **out) {
  /* Count segments first */
  int         count = 0;
  const char *p     = path;
  while (*p) {
    if (*p == '/') {
      p++;
      continue;
    }
    count++;
    while (*p && *p != '/')
      p++;
  }

  if (count == 0) {
    /* Root path "/" — zero segments, matches only "/" */
    *out = NULL;
    return 0;
  }

  struct xHttpRouteSegment_ *segs = (struct xHttpRouteSegment_ *)calloc(
    (size_t)count, sizeof(struct xHttpRouteSegment_));
  if (!segs) return -1;

  int i = 0;
  p     = path;
  while (*p) {
    if (*p == '/') {
      p++;
      continue;
    }
    const char *start = p;
    while (*p && *p != '/')
      p++;
    size_t len = (size_t)(p - start);

    if (start[0] == ':' && len > 1) {
      /* Param segment: skip the leading ':' */
      segs[i].text  = NULL;
      segs[i].param = strndup(start + 1, len - 1);
      if (!segs[i].param) goto fail;
    } else {
      /* Static segment */
      segs[i].text  = strndup(start, len);
      segs[i].param = NULL;
      if (!segs[i].text) goto fail;
    }
    i++;
  }

  *out = segs;
  return count;

fail:
  for (int j = 0; j < i; j++) {
    free((void *)segs[j].text);
    free((void *)segs[j].param);
  }
  free(segs);
  return -1;
}

static void route_free_segments(struct xHttpRouteSegment_ *segs, int count) {
  if (!segs) return;
  for (int i = 0; i < count; i++) {
    free((void *)segs[i].text);
    free((void *)segs[i].param);
  }
  free(segs);
}

/**
 * Match a request URL against a route's pre-parsed segments.
 * On success, fills params[] and returns 1.  On failure returns 0.
 */
static int route_match(const struct xHttpRoute_ *route, const char *url,
                       struct xHttpParam_ *params, int *param_count) {
  *param_count = 0;

  /* Split URL into segments and compare with route segments */
  const char *p       = url;
  int         seg_idx = 0;

  while (*p == '/')
    p++; /* skip leading slashes */

  for (seg_idx = 0; seg_idx < route->segment_count; seg_idx++) {
    if (*p == '\0') return 0; /* URL has fewer segments than route */

    const char *seg_start = p;
    while (*p && *p != '/')
      p++;
    size_t seg_len = (size_t)(p - seg_start);

    const struct xHttpRouteSegment_ *rs = &route->segments[seg_idx];
    if (rs->param) {
      /* Param segment: matches any non-empty string */
      if (seg_len == 0) return 0;
      if (*param_count >= XHTTP_MAX_PARAMS) return 0;
      params[*param_count].name      = rs->param;
      params[*param_count].value     = seg_start;
      params[*param_count].value_len = seg_len;
      (*param_count)++;
    } else {
      /* Static segment: exact match */
      if (seg_len != strlen(rs->text) ||
          memcmp(seg_start, rs->text, seg_len) != 0) {
        return 0;
      }
    }

    while (*p == '/')
      p++; /* skip slashes between segments */
  }

  /* URL must have no trailing segments */
  if (*p != '\0') return 0;

  return 1;
}

xErrno xHttpServerRoute(xHttpServer server, const char *method,
                        const char *path, xHttpHandlerFunc handler, void *arg) {
  if (!server || !path || !handler) return xErrno_InvalidArg;
  struct xHttpServer_ *s = (struct xHttpServer_ *)server;

  struct xHttpRoute_ *route =
    (struct xHttpRoute_ *)calloc(1, sizeof(struct xHttpRoute_));
  if (!route) return xErrno_NoMemory;

  route->method  = method ? strdup(method) : NULL;
  route->path    = strdup(path);
  route->handler = handler;
  route->arg     = arg;
  route->next    = NULL;

  if (!route->path) {
    free((void *)route->method);
    free(route);
    return xErrno_NoMemory;
  }

  /* Pre-parse route pattern into segments */
  int seg_count = route_parse_segments(path, &route->segments);
  if (seg_count < 0) {
    free((void *)route->method);
    free((void *)route->path);
    free(route);
    return xErrno_NoMemory;
  }
  route->segment_count = seg_count;

  /* Append to tail */
  if (s->routes_tail) {
    s->routes_tail->next = route;
  } else {
    s->routes = route;
  }
  s->routes_tail = route;

  return xErrno_Ok;
}

/* ── xHttpRequestParam ─────────────────────────────────────────────────── */

const char *xHttpRequestParam(const xHttpRequest *req, const char *name,
                              size_t *len) {
  if (!req || !name || !req->params_) return NULL;

  const struct xHttpParam_ *params = (const struct xHttpParam_ *)req->params_;
  /* Walk the params array; terminated by name == NULL */
  for (int i = 0; params[i].name; i++) {
    if (strcmp(params[i].name, name) == 0) {
      if (len) *len = params[i].value_len;
      return params[i].value;
    }
  }
  return NULL;
}

/**
 * Dispatch a parsed request to the matching route handler.
 */
static void conn_dispatch_request(struct xHttpConn_ *conn) {
  struct xHttpServer_ *s      = conn->server;
  struct xHttpStream_ *stream = conn->stream;

  /* Get method string from protocol handler */
  const char *method_str = conn->proto.method(stream);

  /* Ensure buffers are null-terminated for C string usage.
   * xBuffer doesn't auto-terminate, so we append a '\0' sentinel.
   * This is safe because xBufferAppend will grow if needed. */
  if (stream->url) xBufferAppend(&stream->url, "", 1);
  if (stream->headers_raw) xBufferAppend(&stream->headers_raw, "", 1);
  if (stream->body) xBufferAppend(&stream->body, "", 1);

  /* Build the xHttpRequest from stream state */
  xHttpRequest req;
  req.method = method_str;
  req.url    = stream->url ? (const char *)xBufferData(stream->url) : "/";
  req.headers =
    stream->headers_raw ? (const char *)xBufferData(stream->headers_raw) : "";
  req.headers_len =
    stream->headers_raw ? xBufferLen(stream->headers_raw) - 1 : 0;
  req.body     = stream->body ? (const char *)xBufferData(stream->body) : NULL;
  req.body_len = stream->body ? xBufferLen(stream->body) - 1 : 0;
  req.params_  = NULL;

  /* Search for matching route (segment-by-segment) */
  int                 path_matched = 0;
  struct xHttpRoute_ *r            = s->routes;
  struct xHttpParam_  params[XHTTP_MAX_PARAMS + 1]; /* +1 for sentinel */
  int                 param_count = 0;

  while (r) {
    if (route_match(r, req.url, params, &param_count)) {
      path_matched = 1;
      /* Check method match (NULL method matches all) */
      if (!r->method || strcasecmp(r->method, method_str) == 0) {
        /* Terminate params array with a sentinel */
        params[param_count].name      = NULL;
        params[param_count].value     = NULL;
        params[param_count].value_len = 0;
        req.params_                   = params;

        /* Match found: call handler */
        r->handler((xHttpResponseWriter)&stream->writer, &req, r->arg);

        /* If handler didn't send a response, send default 200 OK */
        if (!stream->writer.sent && !stream->writer.streaming) {
          xHttpResponseSend((xHttpResponseWriter)&stream->writer, NULL, 0);
        }

        /* If handler was streaming but didn't call End, end it now */
        if (stream->writer.streaming && !stream->writer.sent) {
          xHttpResponseEnd((xHttpResponseWriter)&stream->writer);
        }

        /* conn_after_response may close the connection, so don't
         * access conn after this call */
        conn_after_response(conn);
        return;
      }
    }
    r = r->next;
  }

  /* No match */
  if (path_matched) {
    /* Path matched but method didn't: 405 */
    xHttpConnSendError(conn, 405, "Method Not Allowed");
  } else {
    /* No path match: 404 */
    xHttpConnSendError(conn, 404, "Not Found");
  }
  /* Handle lifecycle (may close conn) */
  conn_after_response(conn);
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  Response building & sending (Task 8)
 * ═══════════════════════════════════════════════════════════════════════════
 */

void xHttpResponseSetStatus(xHttpResponseWriter writer, int code) {
  if (!writer) return;
  struct xHttpResponseWriter_ *w = (struct xHttpResponseWriter_ *)writer;
  w->status_code                 = code;
}

xErrno xHttpResponseSetHeader(xHttpResponseWriter writer, const char *key,
                              const char *value) {
  if (!writer || !key || !value) return xErrno_InvalidArg;
  struct xHttpResponseWriter_ *w = (struct xHttpResponseWriter_ *)writer;

  struct xHttpHeader_ *h =
    (struct xHttpHeader_ *)calloc(1, sizeof(struct xHttpHeader_));
  if (!h) return xErrno_NoMemory;

  h->key   = strdup(key);
  h->value = strdup(value);
  h->next  = NULL;

  if (!h->key || !h->value) {
    free(h->key);
    free(h->value);
    free(h);
    return xErrno_NoMemory;
  }

  /* Append to tail */
  if (w->headers_tail) {
    w->headers_tail->next = h;
  } else {
    w->headers = h;
  }
  w->headers_tail = h;

  return xErrno_Ok;
}

xErrno xHttpResponseSend(xHttpResponseWriter writer, const char *body,
                         size_t body_len) {
  if (!writer) return xErrno_InvalidArg;
  struct xHttpResponseWriter_ *w = (struct xHttpResponseWriter_ *)writer;

  /* Only send once, and cannot mix with streaming */
  if (w->sent || w->streaming) return xErrno_InvalidState;
  w->sent = 1;

  struct xHttpStream_ *stream = w->stream;
  struct xHttpConn_   *conn   = stream->conn;

  /* Delegate to protocol-specific response serialization */
  conn->proto.send_response(stream, w->status_code, w->headers, body, body_len);

  /* Try to flush immediately (but don't close the connection yet;
   * the caller will handle lifecycle via conn_after_response) */
  conn_try_flush(conn);

  return xErrno_Ok;
}

xErrno xHttpResponseWrite(xHttpResponseWriter writer, const char *data,
                          size_t len) {
  if (!writer) return xErrno_InvalidArg;
  struct xHttpResponseWriter_ *w = (struct xHttpResponseWriter_ *)writer;

  /* Cannot mix with Send */
  if (w->sent) return xErrno_InvalidState;

  struct xHttpStream_ *stream = w->stream;
  struct xHttpConn_   *conn   = stream->conn;

  /* Delegate to protocol-specific write_data */
  conn->proto.write_data(stream, data, len);

  /* Try to flush immediately */
  conn_try_flush(conn);

  return xErrno_Ok;
}

void xHttpResponseEnd(xHttpResponseWriter writer) {
  if (!writer) return;
  struct xHttpResponseWriter_ *w = (struct xHttpResponseWriter_ *)writer;

  /* Only meaningful in streaming mode, and only once */
  if (!w->streaming || w->sent) return;

  struct xHttpStream_ *stream = w->stream;
  struct xHttpConn_   *conn   = stream->conn;

  /* Delegate to protocol-specific end_stream */
  conn->proto.end_stream(stream);

  /* Flush any remaining data */
  conn_try_flush(conn);
}

/**
 * Try to write pending data to the socket without lifecycle management.
 * Does NOT close the connection or reset parser state.
 */
static void conn_try_flush(struct xHttpConn_ *conn) {
  if (xIOBufferEmpty(&conn->write_buf)) return;

  ssize_t n = xIOBufferWriteFd(&conn->write_buf, xSocketFd(conn->sock));
  if (n < 0) {
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
      /* Register for write events (backpressure) */
      if (!conn->writing) {
        conn->writing = 1;
        xSocketSetMask(conn->server->loop, conn->sock,
                       xEvent_Read | xEvent_Write);
      }
      return;
    }
    /* Write error: mark for close */
    conn->keep_alive = 0;
    return;
  }

  /* Check if there's more to write */
  if (!xIOBufferEmpty(&conn->write_buf)) {
    if (!conn->writing) {
      conn->writing = 1;
      xSocketSetMask(conn->server->loop, conn->sock,
                     xEvent_Read | xEvent_Write);
    }
  } else {
    if (conn->writing) {
      conn->writing = 0;
      xSocketSetMask(conn->server->loop, conn->sock, xEvent_Read);
    }
  }
}

/**
 * Send a simple error response (used internally for 400, 404, 405, etc.)
 */
void xHttpConnSendError(struct xHttpConn_ *conn, int status_code,
                        const char *reason) {
  struct xHttpStream_ *stream = conn->stream;

  /* If response already sent, just close */
  if (stream->writer.sent) {
    conn->keep_alive = 0;
    return;
  }

  /* Build a simple HTML error body */
  char body[256];
  int  body_len =
    snprintf(body, sizeof(body), "<html><body><h1>%d %s</h1></body></html>\r\n",
             status_code, reason);

  stream->writer.status_code = status_code;

  /* Close connection after error for H1; H2 connections stay open
   * (only the individual stream is closed by nghttp2). */
  if (!stream->stream_id) {
    conn->keep_alive = 0; /* H1: close after error */
  }

  xHttpResponseSetHeader((xHttpResponseWriter)&stream->writer, "Content-Type",
                         "text/html");
  xHttpResponseSend((xHttpResponseWriter)&stream->writer, body,
                    (size_t)body_len);
}

/**
 * Flush the write buffer to the socket.
 * Returns 1 if the connection was closed, 0 otherwise.
 */
int xHttpConnFlushWriteInternal(struct xHttpConn_ *conn) {
  /* First, try to write any pending data */
  conn_try_flush(conn);

  if (xIOBufferEmpty(&conn->write_buf)) {
    /* All data written; handle lifecycle */
    if (!conn->keep_alive) {
      xHttpConnClose(conn);
      return 1;
    } else {
      conn_reset_request_state(conn);
      /* Reset idle timeout for next request */
      if (conn->server->idle_timeout_ms > 0) {
        xSocketSetTimeout(conn->sock, conn->server->idle_timeout_ms, 0);
      }
    }
  }
  /* If there's still data, backpressure is already set up by conn_try_flush */
  return 0;
}

void xHttpConnFlushWrite(struct xHttpConn_ *conn) {
  xHttpConnFlushWriteInternal(conn);
}

/**
 * Called when write events fire: continue flushing the write buffer.
 */
static void conn_write_ready(struct xHttpConn_ *conn) {
  xHttpConnFlushWrite(conn);
}

/**
 * Post-response handling: decide whether to keep-alive or close.
 * This is called after the handler has finished and response is queued.
 * Note: conn may be freed after this call if keep_alive is false.
 */
static void conn_after_response(struct xHttpConn_ *conn) {
  /* H2 connections: stream lifecycle is managed by nghttp2 callbacks.
   * Don't reset or close — just return. The caller (h2_on_data) will
   * handle session_send and flushing after all dispatches complete. */
  if (conn->proto_detected && conn->proto.reset != NULL) {
    /* Check if this is H2 by testing if should_keep_alive always returns 1
     * (H2 connections are always persistent). A cleaner approach would be
     * a protocol type flag, but this works for now. */
    if (conn->proto.should_keep_alive && conn->proto.should_keep_alive(conn) &&
        conn->keep_alive) {
      /* Could be H1 keep-alive or H2. Distinguish by checking if
       * conn->stream was created by H2 (stream_id > 0). */
      if (conn->stream && conn->stream->stream_id > 0) {
        /* H2 stream: don't reset, nghttp2 manages lifecycle */
        return;
      }
    }
  }

  if (xIOBufferEmpty(&conn->write_buf)) {
    /* All data already flushed synchronously. Handle lifecycle now. */
    if (!conn->keep_alive) {
      xHttpConnClose(conn);
    } else {
      conn_reset_request_state(conn);
      if (conn->server->idle_timeout_ms > 0) {
        xSocketSetTimeout(conn->sock, conn->server->idle_timeout_ms, 0);
      }
    }
  } else if (!conn->writing) {
    /* There's still data to write; try to flush.
     * xHttpConnFlushWriteInternal handles lifecycle after flush. */
    xHttpConnFlushWriteInternal(conn);
  }
  /* If conn->writing is true, the write event handler will take over. */
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  Public wrappers for internal functions (used by proto_h2.c)
 * ═══════════════════════════════════════════════════════════════════════════
 */

void xHttpConnDispatchRequest(struct xHttpConn_ *conn) {
  conn_dispatch_request(conn);
}

void xHttpConnTryFlush(struct xHttpConn_ *conn) {
  conn_try_flush(conn);
}
