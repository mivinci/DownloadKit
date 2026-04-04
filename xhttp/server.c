/*
 * Copyright 2025 The xKit Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * server.c - Asynchronous HTTP/1.1 server implementation
 */

#include "server_private.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <unistd.h>

/* ═══════════════════════════════════════════════════════════════════════════
 *  Forward declarations
 * ═══════════════════════════════════════════════════════════════════════════ */

static void on_listen_event(xSocket sock, xEventMask mask, void *arg);
static void on_conn_event(xSocket sock, xEventMask mask, void *arg);

/* llhttp callbacks */
static int on_url(llhttp_t *parser, const char *at, size_t len);
static int on_header_field(llhttp_t *parser, const char *at, size_t len);
static int on_header_value(llhttp_t *parser, const char *at, size_t len);
static int on_headers_complete(llhttp_t *parser);
static int on_body(llhttp_t *parser, const char *at, size_t len);
static int on_message_complete(llhttp_t *parser);

/* Internal helpers */
static void conn_init_parser(struct xHttpConn_ *conn);
static void conn_reset_request_state(struct xHttpConn_ *conn);
static void conn_dispatch_request(struct xHttpConn_ *conn);
static void conn_write_ready(struct xHttpConn_ *conn);
static void conn_after_response(struct xHttpConn_ *conn);
static void conn_try_flush(struct xHttpConn_ *conn);
static void route_free_segments(struct xHttpRouteSegment_ *segs, int count);
int xHttpConnFlushWriteInternal(struct xHttpConn_ *conn);


/* ═══════════════════════════════════════════════════════════════════════════
 *  HTTP status reason phrases
 * ═══════════════════════════════════════════════════════════════════════════ */

const char *xHttpStatusReason(int code) {
  switch (code) {
    case 200: return "OK";
    case 201: return "Created";
    case 204: return "No Content";
    case 301: return "Moved Permanently";
    case 302: return "Found";
    case 304: return "Not Modified";
    case 400: return "Bad Request";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 408: return "Request Timeout";
    case 413: return "Content Too Large";
    case 431: return "Request Header Fields Too Large";
    case 500: return "Internal Server Error";
    case 502: return "Bad Gateway";
    case 503: return "Service Unavailable";
    default:  return "Unknown";
  }
}



/* ═══════════════════════════════════════════════════════════════════════════
 *  Server lifecycle (Task 4)
 * ═══════════════════════════════════════════════════════════════════════════ */

xHttpServer xHttpServerCreate(xEventLoop loop) {
  if (!loop) return NULL;

  struct xHttpServer_ *s = (struct xHttpServer_ *)calloc(1, sizeof(*s));
  if (!s) return NULL;

  s->loop             = loop;
  s->listen_sock      = NULL;
  s->listen_fd        = -1;
  s->routes           = NULL;
  s->routes_tail      = NULL;
  s->conns            = NULL;
  s->idle_timeout_ms  = XHTTP_DEFAULT_IDLE_TIMEOUT_MS;
  s->max_header_size  = XHTTP_DEFAULT_MAX_HEADER_SIZE;
  s->max_body_size    = XHTTP_DEFAULT_MAX_BODY_SIZE;

  return (xHttpServer)s;
}

xErrno xHttpServerListen(xHttpServer server,
                          const char *host, uint16_t port) {
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
  xSocket sock = xSocketCreateFromFd(s->loop, fd, xEvent_Read,
                                      on_listen_event, s);
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
  s->idle_timeout_ms = timeout_ms;
  return xErrno_Ok;
}

xErrno xHttpServerSetMaxHeaderSize(xHttpServer server, size_t max_size) {
  if (!server) return xErrno_InvalidArg;
  if (max_size == 0) return xErrno_InvalidArg;
  struct xHttpServer_ *s = (struct xHttpServer_ *)server;
  s->max_header_size = max_size;
  return xErrno_Ok;
}

xErrno xHttpServerSetMaxBodySize(xHttpServer server, size_t max_size) {
  if (!server) return xErrno_InvalidArg;
  if (max_size == 0) return xErrno_InvalidArg;
  struct xHttpServer_ *s = (struct xHttpServer_ *)server;
  s->max_body_size = max_size;
  return xErrno_Ok;
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  Connection accept & management (Task 5)
 * ═══════════════════════════════════════════════════════════════════════════ */

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
    socklen_t addr_len = sizeof(client_addr);
    int client_fd = accept(s->listen_fd,
                           (struct sockaddr *)&client_addr, &addr_len);
    if (client_fd < 0) {
      if (errno == EAGAIN || errno == EWOULDBLOCK) break;
      if (errno == EMFILE || errno == ENFILE) {
        /* fd exhaustion: log warning and continue */
        fprintf(stderr, "xhttp: accept() failed: %s (fd exhaustion)\n",
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

    /* Initialize response writer defaults */
    conn->writer.status_code = 200;
    conn->writer.headers     = NULL;
    conn->writer.headers_tail = NULL;
    conn->writer.sent        = 0;
    conn->writer.streaming   = 0;
    conn->writer.conn        = conn;

    /* Initialize llhttp parser */
    conn_init_parser(conn);

    /* Wrap accepted fd in xSocket */
    xSocket client_sock = xSocketCreateFromFd(
        s->loop, client_fd, xEvent_Read, on_conn_event, conn);
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
 * Initialize the llhttp parser and settings for a connection.
 */
static void conn_init_parser(struct xHttpConn_ *conn) {
  memset(&conn->parser_settings, 0, sizeof(conn->parser_settings));
  conn->parser_settings.on_url              = on_url;
  conn->parser_settings.on_header_field     = on_header_field;
  conn->parser_settings.on_header_value     = on_header_value;
  conn->parser_settings.on_headers_complete = on_headers_complete;
  conn->parser_settings.on_body             = on_body;
  conn->parser_settings.on_message_complete = on_message_complete;

  llhttp_init(&conn->parser, HTTP_REQUEST, &conn->parser_settings);
  conn->parser.data = conn;
}

/**
 * Reset per-request parsing state (for keep-alive reuse).
 */
static void conn_reset_request_state(struct xHttpConn_ *conn) {
  xBufferReset(conn->url);
  xBufferReset(conn->header_field);
  xBufferReset(conn->headers_raw);
  xBufferReset(conn->body);

  conn->header_bytes = 0;

  /* Reset response writer */
  struct xHttpHeader_ *h = conn->writer.headers;
  while (h) {
    struct xHttpHeader_ *next = h->next;
    free(h->key);
    free(h->value);
    free(h);
    h = next;
  }
  conn->writer.status_code  = 200;
  conn->writer.headers      = NULL;
  conn->writer.headers_tail = NULL;
  conn->writer.sent         = 0;
  conn->writer.streaming    = 0;

  /* Reset parser for next request */
  llhttp_reset(&conn->parser);
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
  if (conn->prev) conn->prev->next = conn->next;
  else            s->conns = conn->next;
  if (conn->next) conn->next->prev = conn->prev;

  /* Destroy socket */
  if (conn->sock) {
    xSocketDestroy(s->loop, conn->sock);
    conn->sock = NULL;
  }

  /* Free buffers */
  xIOBufferDeinit(&conn->read_buf);
  xIOBufferDeinit(&conn->write_buf);

  /* Free request parsing state */
  xBufferDestroy(conn->url);
  xBufferDestroy(conn->header_field);
  xBufferDestroy(conn->headers_raw);
  xBufferDestroy(conn->body);

  /* Free response headers */
  struct xHttpHeader_ *h = conn->writer.headers;
  while (h) {
    struct xHttpHeader_ *next = h->next;
    free(h->key);
    free(h->value);
    free(h);
    h = next;
  }

  free(conn);
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  Connection I/O event handler
 * ═══════════════════════════════════════════════════════════════════════════ */

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
    /* Read in a loop for edge-triggered mode */
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
      break; /* Got data; process below */
    }

    /* Feed accumulated data to llhttp parser */
    size_t buf_len = xIOBufferLen(&conn->read_buf);
    if (buf_len > 0) {
      /* Copy read buffer to a contiguous buffer for parsing */
      char *linear = (char *)malloc(buf_len);
      if (!linear) {
        xHttpConnSendError(conn, 500, "Internal Server Error");
        conn_after_response(conn);
        return;
      }
      xIOBufferCopyTo(&conn->read_buf, linear);
      xIOBufferConsume(&conn->read_buf, buf_len);

      enum llhttp_errno err = llhttp_execute(&conn->parser, linear, buf_len);
      free(linear);

      /* Handle deferred error from llhttp callbacks */
      if (conn->pending_error) {
        int code = conn->pending_error;
        const char *reason = conn->pending_error_reason;
        conn->pending_error = 0;
        conn->pending_error_reason = NULL;
        xHttpConnSendError(conn, code, reason);
        conn_after_response(conn);
        return;
      }

      /* Handle completed request (deferred from on_message_complete) */
      if (conn->request_complete) {
        conn->request_complete = 0;
        conn_dispatch_request(conn);
        /* conn may have been freed by dispatch (e.g. Connection: close),
         * so we must not access conn after this point. */
        return;
      }

      if (err != HPE_OK && err != HPE_PAUSED) {
        /* Parse error: send 400 and close */
        xHttpConnSendError(conn, 400, "Bad Request");
        conn_after_response(conn);
        return;
      }
    }
  }
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  HTTP request parsing — llhttp callbacks (Task 6)
 * ═══════════════════════════════════════════════════════════════════════════ */

static int on_url(llhttp_t *parser, const char *at, size_t len) {
  struct xHttpConn_ *conn = (struct xHttpConn_ *)parser->data;
  conn->header_bytes += len;

  if (!conn->url) conn->url = xBufferCreate(256);
  if (!conn->url) return HPE_INTERNAL;
  if (xBufferAppend(&conn->url, at, len) != xErrno_Ok) return HPE_INTERNAL;
  return HPE_OK;
}

static int on_header_field(llhttp_t *parser, const char *at, size_t len) {
  struct xHttpConn_ *conn = (struct xHttpConn_ *)parser->data;
  conn->header_bytes += len;

  if (conn->header_bytes > conn->server->max_header_size) {
    conn->pending_error = 431;
    conn->pending_error_reason = "Request Header Fields Too Large";
    return HPE_USER;
  }

  /* Accumulate into raw headers */
  if (!conn->headers_raw) conn->headers_raw = xBufferCreate(512);
  if (!conn->headers_raw) return HPE_INTERNAL;
  if (xBufferAppend(&conn->headers_raw, at, len) != xErrno_Ok)
    return HPE_INTERNAL;

  /* Track current field for potential use */
  if (conn->header_field) {
    xBufferReset(conn->header_field);
  } else {
    conn->header_field = xBufferCreate(128);
    if (!conn->header_field) return HPE_INTERNAL;
  }
  if (xBufferAppend(&conn->header_field, at, len) != xErrno_Ok)
    return HPE_INTERNAL;

  return HPE_OK;
}

static int on_header_value(llhttp_t *parser, const char *at, size_t len) {
  struct xHttpConn_ *conn = (struct xHttpConn_ *)parser->data;
  conn->header_bytes += len;

  if (conn->header_bytes > conn->server->max_header_size) {
    conn->pending_error = 431;
    conn->pending_error_reason = "Request Header Fields Too Large";
    return HPE_USER;
  }

  /* Append ": " + value + "\r\n" to raw headers */
  if (xBufferAppend(&conn->headers_raw, ": ", 2) != xErrno_Ok)
    return HPE_INTERNAL;
  if (xBufferAppend(&conn->headers_raw, at, len) != xErrno_Ok)
    return HPE_INTERNAL;
  if (xBufferAppend(&conn->headers_raw, "\r\n", 2) != xErrno_Ok)
    return HPE_INTERNAL;

  return HPE_OK;
}

static int on_headers_complete(llhttp_t *parser) {
  struct xHttpConn_ *conn = (struct xHttpConn_ *)parser->data;

  /* Determine keep-alive from HTTP version and headers */
  conn->keep_alive = llhttp_should_keep_alive(parser);

  return HPE_OK;
}

static int on_body(llhttp_t *parser, const char *at, size_t len) {
  struct xHttpConn_ *conn = (struct xHttpConn_ *)parser->data;

  size_t cur_len = conn->body ? xBufferLen(conn->body) : 0;
  if (cur_len + len > conn->server->max_body_size) {
    conn->pending_error = 413;
    conn->pending_error_reason = "Content Too Large";
    return HPE_USER;
  }

  if (!conn->body) conn->body = xBufferCreate(1024);
  if (!conn->body) return HPE_INTERNAL;
  if (xBufferAppend(&conn->body, at, len) != xErrno_Ok) return HPE_INTERNAL;

  return HPE_OK;
}

static int on_message_complete(llhttp_t *parser) {
  struct xHttpConn_ *conn = (struct xHttpConn_ *)parser->data;

  /* Set flag to dispatch after llhttp_execute returns.
   * We must not call dispatch from within a callback because
   * the response flush may reset/close the parser. */
  conn->request_complete = 1;

  /* Pause the parser so we can process the response before parsing
   * the next pipelined request */
  llhttp_pause(parser);

  return HPE_OK;
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
 * ═══════════════════════════════════════════════════════════════════════════ */

/**
 * Parse a route pattern like "/users/:id/posts" into an array of segments.
 * Returns the number of segments, or -1 on error.
 * Caller must free the returned array (each segment's text/param is strdup'd).
 */
static int route_parse_segments(const char *path,
                                struct xHttpRouteSegment_ **out) {
  /* Count segments first */
  int count = 0;
  const char *p = path;
  while (*p) {
    if (*p == '/') { p++; continue; }
    count++;
    while (*p && *p != '/') p++;
  }

  if (count == 0) {
    /* Root path "/" — zero segments, matches only "/" */
    *out = NULL;
    return 0;
  }

  struct xHttpRouteSegment_ *segs =
      (struct xHttpRouteSegment_ *)calloc((size_t)count,
                                          sizeof(struct xHttpRouteSegment_));
  if (!segs) return -1;

  int i = 0;
  p = path;
  while (*p) {
    if (*p == '/') { p++; continue; }
    const char *start = p;
    while (*p && *p != '/') p++;
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
static int route_match(const struct xHttpRoute_ *route,
                       const char *url,
                       struct xHttpParam_ *params, int *param_count) {
  *param_count = 0;

  /* Split URL into segments and compare with route segments */
  const char *p = url;
  int seg_idx = 0;

  while (*p == '/') p++; /* skip leading slashes */

  for (seg_idx = 0; seg_idx < route->segment_count; seg_idx++) {
    if (*p == '\0') return 0; /* URL has fewer segments than route */

    const char *seg_start = p;
    while (*p && *p != '/') p++;
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

    while (*p == '/') p++; /* skip slashes between segments */
  }

  /* URL must have no trailing segments */
  if (*p != '\0') return 0;

  return 1;
}

xErrno xHttpServerRoute(xHttpServer server,
                         const char *method, const char *path,
                         xHttpHandlerFunc handler, void *arg) {
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

const char *xHttpRequestParam(const xHttpRequest *req,
                               const char *name,
                               size_t *len) {
  if (!req || !name || !req->params_) return NULL;

  const struct xHttpParam_ *params =
      (const struct xHttpParam_ *)req->params_;
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
  struct xHttpServer_ *s = conn->server;

  /* Get method string from llhttp */
  const char *method_str = llhttp_method_name(
      (llhttp_method_t)conn->parser.method);

  /* Build the xHttpRequest */
  xHttpRequest req;
  req.method      = method_str;
  req.url         = conn->url ? (const char *)xBufferData(conn->url) : "/";
  req.headers     = conn->headers_raw
                        ? (const char *)xBufferData(conn->headers_raw) : "";
  req.headers_len = conn->headers_raw ? xBufferLen(conn->headers_raw) : 0;
  req.body        = conn->body
                        ? (const char *)xBufferData(conn->body) : NULL;
  req.body_len    = conn->body ? xBufferLen(conn->body) : 0;
  req.params_     = NULL;

  /* Search for matching route (segment-by-segment) */
  int path_matched = 0;
  struct xHttpRoute_ *r = s->routes;
  struct xHttpParam_ params[XHTTP_MAX_PARAMS + 1]; /* +1 for sentinel */
  int param_count = 0;

  while (r) {
    if (route_match(r, req.url, params, &param_count)) {
      path_matched = 1;
      /* Check method match (NULL method matches all) */
      if (!r->method || strcasecmp(r->method, method_str) == 0) {
        /* Terminate params array with a sentinel */
        params[param_count].name  = NULL;
        params[param_count].value = NULL;
        params[param_count].value_len = 0;
        req.params_ = params;

        /* Match found: call handler */
        r->handler((xHttpResponseWriter)&conn->writer, &req, r->arg);

        /* If handler didn't send a response, send default 200 OK */
        if (!conn->writer.sent && !conn->writer.streaming) {
          xHttpResponseSend((xHttpResponseWriter)&conn->writer, NULL, 0);
        }

        /* If handler was streaming but didn't call End, end it now */
        if (conn->writer.streaming && !conn->writer.sent) {
          xHttpResponseEnd((xHttpResponseWriter)&conn->writer);
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
 * ═══════════════════════════════════════════════════════════════════════════ */

void xHttpResponseSetStatus(xHttpResponseWriter writer, int code) {
  if (!writer) return;
  struct xHttpResponseWriter_ *w = (struct xHttpResponseWriter_ *)writer;
  w->status_code = code;
}

xErrno xHttpResponseSetHeader(xHttpResponseWriter writer,
                                const char *key, const char *value) {
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

xErrno xHttpResponseSend(xHttpResponseWriter writer,
                           const char *body, size_t body_len) {
  if (!writer) return xErrno_InvalidArg;
  struct xHttpResponseWriter_ *w = (struct xHttpResponseWriter_ *)writer;

  /* Only send once, and cannot mix with streaming */
  if (w->sent || w->streaming) return xErrno_InvalidState;
  w->sent = 1;

  struct xHttpConn_ *conn = w->conn;
  xIOBuffer *wb = &conn->write_buf;

  /* Status line: "HTTP/1.1 <code> <reason>\r\n" */
  char status_line[64];
  int slen = snprintf(status_line, sizeof(status_line),
                      "HTTP/1.1 %d %s\r\n",
                      w->status_code, xHttpStatusReason(w->status_code));
  xIOBufferAppend(wb, status_line, (size_t)slen);

  /* Content-Length header */
  char cl_buf[48];
  int cl_len = snprintf(cl_buf, sizeof(cl_buf),
                        "Content-Length: %zu\r\n", body_len);
  xIOBufferAppend(wb, cl_buf, (size_t)cl_len);

  /* Connection header */
  if (conn->keep_alive) {
    xIOBufferAppendStr(wb, "Connection: keep-alive\r\n");
  } else {
    xIOBufferAppendStr(wb, "Connection: close\r\n");
  }

  /* User-set headers */
  struct xHttpHeader_ *h = w->headers;
  while (h) {
    xIOBufferAppendStr(wb, h->key);
    xIOBufferAppendStr(wb, ": ");
    xIOBufferAppendStr(wb, h->value);
    xIOBufferAppendStr(wb, "\r\n");
    h = h->next;
  }

  /* End of headers */
  xIOBufferAppendStr(wb, "\r\n");

  /* Body */
  if (body && body_len > 0) {
    xIOBufferAppend(wb, body, body_len);
  }

  /* Try to flush immediately (but don't close the connection yet;
   * the caller will handle lifecycle via conn_after_response) */
  conn_try_flush(conn);

  return xErrno_Ok;
}

/* ── Streaming response API ─────────────────────────────────────────── */

/**
 * Flush the response headers for streaming mode (no Content-Length).
 */
static void conn_flush_stream_headers(struct xHttpResponseWriter_ *w) {
  struct xHttpConn_ *conn = w->conn;
  xIOBuffer *wb = &conn->write_buf;

  /* Status line */
  char status_line[64];
  int slen = snprintf(status_line, sizeof(status_line),
                      "HTTP/1.1 %d %s\r\n",
                      w->status_code, xHttpStatusReason(w->status_code));
  xIOBufferAppend(wb, status_line, (size_t)slen);

  /* Transfer-Encoding: chunked is not used; we rely on
   * Connection: close to signal end-of-body for simplicity. */
  xIOBufferAppendStr(wb, "Connection: close\r\n");

  /* User-set headers */
  struct xHttpHeader_ *h = w->headers;
  while (h) {
    xIOBufferAppendStr(wb, h->key);
    xIOBufferAppendStr(wb, ": ");
    xIOBufferAppendStr(wb, h->value);
    xIOBufferAppendStr(wb, "\r\n");
    h = h->next;
  }

  /* End of headers */
  xIOBufferAppendStr(wb, "\r\n");
}

xErrno xHttpResponseWrite(xHttpResponseWriter writer,
                           const char *data, size_t len) {
  if (!writer) return xErrno_InvalidArg;
  struct xHttpResponseWriter_ *w = (struct xHttpResponseWriter_ *)writer;

  /* Cannot mix with Send */
  if (w->sent) return xErrno_InvalidState;

  struct xHttpConn_ *conn = w->conn;

  /* First call: flush headers and enter streaming mode */
  if (!w->streaming) {
    w->streaming = 1;
    conn->keep_alive = 0; /* streaming responses always close */
    conn_flush_stream_headers(w);
  }

  /* Append data */
  if (data && len > 0) {
    xIOBufferAppend(&conn->write_buf, data, len);
  }

  /* Try to flush immediately */
  conn_try_flush(conn);

  return xErrno_Ok;
}

void xHttpResponseEnd(xHttpResponseWriter writer) {
  if (!writer) return;
  struct xHttpResponseWriter_ *w = (struct xHttpResponseWriter_ *)writer;

  /* Only meaningful in streaming mode, and only once */
  if (!w->streaming || w->sent) return;
  w->sent = 1;

  /* Flush any remaining data */
  conn_try_flush(w->conn);
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
  /* If response already sent, just close */
  if (conn->writer.sent) {
    conn->keep_alive = 0;
    return;
  }

  /* Build a simple HTML error body */
  char body[256];
  int body_len = snprintf(body, sizeof(body),
                          "<html><body><h1>%d %s</h1></body></html>\r\n",
                          status_code, reason);

  conn->writer.status_code = status_code;
  conn->keep_alive = 0; /* Close after error */

  xHttpResponseSetHeader((xHttpResponseWriter)&conn->writer,
                          "Content-Type", "text/html");
  xHttpResponseSend((xHttpResponseWriter)&conn->writer,
                     body, (size_t)body_len);
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
