/*
 * Copyright 2025 The libx Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * ws_handshake_client.c - Client-side WebSocket upgrade handshake
 */

#include "ws_handshake_client.h"
#include "ws_crypto.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <x/base/compat.h>

#ifdef __linux__
#include <sys/random.h>
#else
#include <stdlib.h>
#endif

/* RFC 6455 §4.2.2: magic GUID for Sec-WebSocket-Accept */
static const char WS_GUID[] = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";

/* ─────────────────── Random bytes ─────────────────── */

static void ws_client_random(uint8_t *buf, size_t len) {
#ifdef __linux__
  (void)getrandom(buf, len, 0);
#else
  arc4random_buf(buf, len);
#endif
}

/* ─────────────────── Build Upgrade request ─────────────────── */

int xWsClientBuildUpgradeRequest(xIOBuffer *io, const xUrl *url, const char *headers,
                                 char *accept_out, size_t accept_sz) {
  if (!io || !url || !accept_out) return -1;

  /* Generate random 16-byte nonce and Base64-encode it */
  uint8_t nonce[16];
  ws_client_random(nonce, sizeof(nonce));

  char ws_key[32]; /* Base64 of 16 bytes = 24 chars + NUL */
  int  key_len = xWsBase64Encode(nonce, sizeof(nonce), ws_key, sizeof(ws_key));
  if (key_len < 0) return -1;

  /* Compute expected Sec-WebSocket-Accept:
   * Base64(SHA1(ws_key + GUID)) */
  char   concat[256];
  size_t kl = (size_t)key_len;
  if (kl + sizeof(WS_GUID) > sizeof(concat)) return -1;
  memcpy(concat, ws_key, kl);
  memcpy(concat + kl, WS_GUID, sizeof(WS_GUID));

  unsigned char sha1[XWS_SHA1_DIGEST_SIZE];
  xWsSHA1((const unsigned char *)concat, kl + sizeof(WS_GUID) - 1, sha1);

  if (xWsBase64Encode(sha1, XWS_SHA1_DIGEST_SIZE, accept_out, accept_sz) < 0) return -1;

  /* Build request path */
  const char *path     = "/";
  size_t      path_len = 1;
  if (url->path && url->path_len > 0) {
    path     = url->path;
    path_len = url->path_len;
  }

  /* Build Host header value */
  char host_buf[512];
  int  host_len;
  if (url->port && url->port_len > 0) {
    host_len = snprintf(host_buf, sizeof(host_buf), "%.*s:%.*s", (int)url->host_len, url->host,
                        (int)url->port_len, url->port);
  } else {
    host_len = snprintf(host_buf, sizeof(host_buf), "%.*s", (int)url->host_len, url->host);
  }
  if (host_len < 0 || (size_t)host_len >= sizeof(host_buf)) return -1;

  /* Build the full request */
  char req[2048];
  int  req_len;

#ifdef XHTTP_WS_DEFLATE
  /* Build permessage-deflate extension offer */
  char ext_offer[128];
  xWsDeflateBuildClientOffer(ext_offer, sizeof(ext_offer));
  char ext_hdr[192];
  snprintf(ext_hdr, sizeof(ext_hdr), "Sec-WebSocket-Extensions: %s\r\n", ext_offer);
#else
  const char *ext_hdr = "";
#endif

  /* Include query string if present */
  if (url->query && url->query_len > 0) {
    req_len = snprintf(req, sizeof(req),
                       "GET %.*s?%.*s HTTP/1.1\r\n"
                       "Host: %s\r\n"
                       "Upgrade: websocket\r\n"
                       "Connection: Upgrade\r\n"
                       "Sec-WebSocket-Key: %s\r\n"
                       "Sec-WebSocket-Version: 13\r\n"
                       "%s"
                       "%s"
                       "\r\n",
                       (int)path_len, path, (int)url->query_len, url->query, host_buf, ws_key,
                       ext_hdr, headers ? headers : "");
  } else {
    req_len = snprintf(req, sizeof(req),
                       "GET %.*s HTTP/1.1\r\n"
                       "Host: %s\r\n"
                       "Upgrade: websocket\r\n"
                       "Connection: Upgrade\r\n"
                       "Sec-WebSocket-Key: %s\r\n"
                       "Sec-WebSocket-Version: 13\r\n"
                       "%s"
                       "%s"
                       "\r\n",
                       (int)path_len, path, host_buf, ws_key, ext_hdr, headers ? headers : "");
  }

  if (req_len < 0 || (size_t)req_len >= sizeof(req)) return -1;

  if (xIOBufferAppend(io, req, (size_t)req_len) != xErrno_Ok) return -1;

  return 0;
}

/* ─────────────────── Validate 101 response ─────────────────── */

/**
 * Simple header finder for response headers.
 * Case-insensitive key match.
 */
static const char *find_resp_header(const char *data, size_t len, const char *key,
                                    size_t *val_len) {
  size_t      key_len = strlen(key);
  const char *p       = data;
  const char *end     = data + len;

  while (p < end) {
    const char *eol = (const char *)memmem(p, (size_t)(end - p), "\r\n", 2);
    if (!eol) eol = end;

    size_t line_len = (size_t)(eol - p);
    if (line_len > key_len + 1 && strncasecmp(p, key, key_len) == 0 && p[key_len] == ':') {
      const char *val = p + key_len + 1;
      while (val < eol && *val == ' ')
        val++;
      *val_len = (size_t)(eol - val);
      return val;
    }

    p = (eol < end) ? eol + 2 : end;
  }

  *val_len = 0;
  return NULL;
}

static int header_token_match(const char *value, size_t vlen, const char *token) {
  size_t      tlen = strlen(token);
  const char *p    = value;
  const char *end  = value + vlen;

  while (p < end) {
    while (p < end && (*p == ' ' || *p == ',' || *p == '\t'))
      p++;
    if (p >= end) break;

    const char *te = p;
    while (te < end && *te != ',' && *te != ' ')
      te++;

    if ((size_t)(te - p) == tlen && strncasecmp(p, token, tlen) == 0) return 1;

    p = te;
  }
  return 0;
}

int xWsClientValidateUpgradeResponse(const char *data, size_t len, const char *expected_accept) {
  if (!data || len < 12) return -1;

  /* Check "HTTP/1.1 101" status line */
  if (strncmp(data, "HTTP/1.1 101", 12) != 0 && strncmp(data, "HTTP/1.0 101", 12) != 0) return -1;

  /* Find end of status line */
  const char *hdr_start = (const char *)memmem(data, len, "\r\n", 2);
  if (!hdr_start) return -1;
  hdr_start += 2;
  size_t hdr_len = len - (size_t)(hdr_start - data);

  /* Check Upgrade: websocket */
  size_t      vlen;
  const char *upgrade = find_resp_header(hdr_start, hdr_len, "Upgrade", &vlen);
  if (!upgrade || !header_token_match(upgrade, vlen, "websocket")) return -1;

  /* Check Connection: Upgrade */
  const char *conn = find_resp_header(hdr_start, hdr_len, "Connection", &vlen);
  if (!conn || !header_token_match(conn, vlen, "Upgrade")) return -1;

  /* Check Sec-WebSocket-Accept */
  const char *accept = find_resp_header(hdr_start, hdr_len, "Sec-WebSocket-Accept", &vlen);
  if (!accept) return -1;

  size_t expect_len = strlen(expected_accept);
  if (vlen != expect_len || memcmp(accept, expected_accept, expect_len) != 0) return -1;

  return 0;
}

/* ─────────────────── Parse deflate from response ─────────────────── */

#ifdef XHTTP_WS_DEFLATE
int xWsClientParseDeflateResponse(const char *data, size_t len, xWsDeflateParams *params) {
  if (!data || !params) return -1;

  /* Find end of status line */
  const char *hdr_start = (const char *)memmem(data, len, "\r\n", 2);
  if (!hdr_start) return -1;
  hdr_start += 2;
  size_t hdr_len = len - (size_t)(hdr_start - data);

  /* Look for Sec-WebSocket-Extensions header */
  size_t      vlen;
  const char *ext = find_resp_header(hdr_start, hdr_len, "Sec-WebSocket-Extensions", &vlen);
  if (!ext || vlen == 0) return -1;

  return xWsDeflateParseOffer(ext, vlen, params);
}
#endif
