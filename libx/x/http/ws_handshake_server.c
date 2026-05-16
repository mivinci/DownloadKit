/*
 * Copyright 2025 The libx Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * ws_handshake_server.c - WebSocket upgrade handshake (RFC 6455 §4.2)
 */

#include "server_private.h"
#include "ws_crypto.h"
#include "ws_private.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <x/base/compat.h>

/* RFC 6455 §4.2.2: magic GUID for Sec-WebSocket-Accept */
static const char WS_GUID[] = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";

/* ───────────────────── Header helpers ───────────────────── */

/**
 * Find a header value in raw headers ("Key: Value\r\n" format).
 * Case-insensitive key match. Returns a pointer to the value
 * (within the headers string) and sets *len to the value length.
 * Returns NULL if not found.
 */
static const char *find_header(const char *headers, size_t headers_len, const char *key,
                               size_t *len) {
  size_t      key_len = strlen(key);
  const char *p       = headers;
  const char *end     = headers + headers_len;

  while (p < end) {
    /* Find end of this header line */
    const char *eol = (const char *)memmem(p, (size_t)(end - p), "\r\n", 2);
    if (!eol) eol = end;

    /* Check if this line starts with "key: " */
    size_t line_len = (size_t)(eol - p);
    if (line_len > key_len + 2 && strncasecmp(p, key, key_len) == 0 && p[key_len] == ':') {
      /* Skip ": " */
      const char *val = p + key_len + 1;
      while (val < eol && *val == ' ')
        val++;
      *len = (size_t)(eol - val);
      return val;
    }

    p = (eol < end) ? eol + 2 : end;
  }

  *len = 0;
  return NULL;
}

/**
 * Check if a header value contains a token (case-insensitive).
 * Handles comma-separated lists like "keep-alive, Upgrade".
 */
static int header_contains_token(const char *value, size_t value_len, const char *token) {
  size_t      token_len = strlen(token);
  const char *p         = value;
  const char *end       = value + value_len;

  while (p < end) {
    /* Skip whitespace and commas */
    while (p < end && (*p == ' ' || *p == ',' || *p == '\t'))
      p++;
    if (p >= end) break;

    /* Find end of this token */
    const char *tok_end = p;
    while (tok_end < end && *tok_end != ',' && *tok_end != ' ')
      tok_end++;

    size_t tok_len = (size_t)(tok_end - p);
    if (tok_len == token_len && strncasecmp(p, token, token_len) == 0) {
      return 1;
    }

    p = tok_end;
  }
  return 0;
}

/* ───────────────────── Handshake ───────────────────── */

/**
 * Upgrade an HTTP connection to WebSocket.
 *
 * Called from a regular xHttpHandlerFunc. On success the HTTP
 * connection is hijacked and a new xWsConn is created.
 */
xErrno xWsUpgrade(xHttpResponseWriter writer, const xHttpRequest *req,
                  const xWsCallbacks *callbacks, void *arg) {
  if (!writer || !req || !callbacks) return xErrno_InvalidArg;

  /* Recover the internal connection from the writer handle.
   * xHttpResponseWriter is a pointer to xHttpResponseWriter_,
   * which contains a back-pointer to the stream, which contains
   * a back-pointer to the connection. */
  struct xHttpResponseWriter_ *w      = (struct xHttpResponseWriter_ *)writer;
  struct xHttpStream_         *stream = w->stream;
  struct xHttpConn_           *conn   = stream->conn;
  struct xHttpServer_         *s      = conn->server;

  /* 1. Method must be GET */
  if (strcmp(req->method, "GET") != 0) {
    xHttpConnSendError(conn, 405, "Method Not Allowed");
    return xErrno_InvalidArg;
  }

  /* 2. Check required headers */
  size_t val_len;

  /* Upgrade: websocket */
  const char *upgrade = find_header(req->headers, req->headers_len, "Upgrade", &val_len);
  if (!upgrade || !header_contains_token(upgrade, val_len, "websocket")) {
    xHttpConnSendError(conn, 400, "Missing Upgrade: websocket");
    return xErrno_InvalidArg;
  }

  /* Connection: Upgrade */
  const char *connection = find_header(req->headers, req->headers_len, "Connection", &val_len);
  if (!connection || !header_contains_token(connection, val_len, "Upgrade")) {
    xHttpConnSendError(conn, 400, "Missing Connection: Upgrade");
    return xErrno_InvalidArg;
  }

  /* Sec-WebSocket-Version: 13 */
  const char *version =
    find_header(req->headers, req->headers_len, "Sec-WebSocket-Version", &val_len);
  if (!version || val_len != 2 || version[0] != '1' || version[1] != '3') {
    xHttpConnSendError(conn, 400, "Unsupported WebSocket version");
    return xErrno_InvalidArg;
  }

  /* Sec-WebSocket-Key */
  size_t      key_len;
  const char *ws_key = find_header(req->headers, req->headers_len, "Sec-WebSocket-Key", &key_len);
  if (!ws_key || key_len == 0 || key_len > 128) {
    xHttpConnSendError(conn, 400, "Missing Sec-WebSocket-Key");
    return xErrno_InvalidArg;
  }

  /* 3. Compute Sec-WebSocket-Accept:
   *    Base64(SHA1(key + GUID)) */
  char concat[256];
  if (key_len + sizeof(WS_GUID) > sizeof(concat)) {
    xHttpConnSendError(conn, 400, "Key too long");
    return xErrno_InvalidArg;
  }
  memcpy(concat, ws_key, key_len);
  memcpy(concat + key_len, WS_GUID, sizeof(WS_GUID)); /* includes NUL */

  unsigned char sha1_digest[XWS_SHA1_DIGEST_SIZE];
  xWsSHA1((const unsigned char *)concat, key_len + sizeof(WS_GUID) - 1, sha1_digest);

  char accept_value[64];
  xWsBase64Encode(sha1_digest, XWS_SHA1_DIGEST_SIZE, accept_value, sizeof(accept_value));

#ifdef XHTTP_WS_DEFLATE
  /* 3b. Check for permessage-deflate extension offer */
  xWsDeflateParams deflate_params;
  memset(&deflate_params, 0, sizeof(deflate_params));
  int has_deflate = 0;
  {
    size_t      ext_len;
    const char *ext_val =
      find_header(req->headers, req->headers_len, "Sec-WebSocket-Extensions", &ext_len);
    if (ext_val && ext_len > 0) {
      has_deflate = (xWsDeflateParseOffer(ext_val, ext_len, &deflate_params) == 0);
    }
  }
#endif

  /* 4. Send 101 Switching Protocols response */
  char ext_resp_hdr[256];
  ext_resp_hdr[0] = '\0';
#ifdef XHTTP_WS_DEFLATE
  if (has_deflate) {
    char ext_val[192];
    if (xWsDeflateBuildServerResponse(&deflate_params, ext_val, sizeof(ext_val)) > 0) {
      snprintf(ext_resp_hdr, sizeof(ext_resp_hdr), "Sec-WebSocket-Extensions: %s\r\n", ext_val);
    }
  }
#endif

  char response[768];
  int  resp_len = snprintf(response, sizeof(response),
                           "HTTP/1.1 101 Switching Protocols\r\n"
                            "Upgrade: websocket\r\n"
                            "Connection: Upgrade\r\n"
                            "Sec-WebSocket-Accept: %s\r\n"
                            "%s"
                            "\r\n",
                           accept_value, ext_resp_hdr);

  if (resp_len < 0 || (size_t)resp_len >= sizeof(response)) {
    xHttpConnSendError(conn, 500, "Internal Server Error");
    return xErrno_SysError;
  }

  /* Write the 101 response through the transport */
  xIOBufferAppend(&conn->write_buf, response, (size_t)resp_len);
  xHttpConnTryFlush(conn);

  /* 5. Hijack the connection */
  xSocket    hijacked_sock      = conn->sock;
  xTransport hijacked_transport = conn->transport;

  /* Prevent xHttpConnHijack from destroying these */
  conn->sock              = NULL;
  conn->transport.ctx     = NULL;
  conn->transport.destroy = NULL;

  xHttpConnHijack(conn);

  /* 6. Create WebSocket connection */
  struct xWsConn_ *ws = xWsConnCreate(s, s->loop, hijacked_sock, hijacked_transport, callbacks, arg,
                                      s->idle_timeout_ms);

  if (!ws) {
    /* Failed to create WS conn: clean up */
    if (hijacked_transport.destroy) {
      hijacked_transport.destroy(hijacked_transport.ctx);
    }
    xSocketDestroy(s->loop, hijacked_sock);
    return xErrno_NoMemory;
  }

  /* Transfer any remaining data from the HTTP read buffer.
   * xIOBufferAppendIOBuffer properly moves block references
   * and clears the source buffer. */
  if (!xIOBufferEmpty(&conn->read_buf)) {
    xIOBufferAppendIOBuffer(&ws->read_buf, &conn->read_buf);
  }

#ifdef XHTTP_WS_DEFLATE
  /* Initialize deflate context if negotiated */
  if (has_deflate && deflate_params.enabled) {
    ws->deflate_params = deflate_params;
    ws->deflate_ctx    = xWsDeflateCreate(&deflate_params, 0);
    if (ws->deflate_ctx) {
      ws->parser.allow_rsv1 = 1;
    }
  }
#endif

  /* NOTE: conn is NOT freed here. xHttpConnHijack() already
   * removed it from the server's connection list and destroyed
   * the stream/proto. The dispatch path (conn_dispatch_request)
   * will detect conn->hijacked and skip post-response handling.
   * conn will be freed by xHttpConnClose() which checks the
   * hijacked flag — we clear the socket/transport so Close
   * just frees the struct and buffers. */

  /* 7. Fire on_open callback */
  if (ws->callbacks.on_open) {
    ws->callbacks.on_open((xWsConn)ws, ws->user_arg);
  }

  return xErrno_Ok;
}
