/*
 * Copyright 2025 The xKit Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * client_private.h - Internal data structures for the xhttp module
 */

#ifndef XHTTP_CLIENT_PRIVATE_H
#define XHTTP_CLIENT_PRIVATE_H

#include <curl/curl.h>
#include <stdlib.h>
#include <string.h>
#include <xhttp/client.h>

/* ───────────────────── Dynamic buffer ───────────────────── */

struct xHttpBuf_ {
  char   *data;
  size_t  len;
  size_t  cap;
};

static inline void http_buf_init(struct xHttpBuf_ *buf) {
  buf->data = NULL;
  buf->len  = 0;
  buf->cap  = 0;
}

static inline void http_buf_free(struct xHttpBuf_ *buf) {
  free(buf->data);
  buf->data = NULL;
  buf->len  = 0;
  buf->cap  = 0;
}

/**
 * @brief Append data to a dynamic buffer, growing as needed.
 * @return 0 on success, -1 on allocation failure.
 */
static inline int http_buf_append(struct xHttpBuf_ *buf, const char *data,
                                  size_t len) {
  if (buf->len + len + 1 > buf->cap) {
    size_t newcap = buf->cap ? buf->cap * 2 : 1024;
    while (newcap < buf->len + len + 1)
      newcap *= 2;
    char *tmp = (char *)realloc(buf->data, newcap);
    if (!tmp) return -1;
    buf->data = tmp;
    buf->cap  = newcap;
  }
  memcpy(buf->data + buf->len, data, len);
  buf->len += len;
  buf->data[buf->len] = '\0'; /* keep NUL-terminated */
  return 0;
}

/* ───────────────────── Per-socket context ───────────────────── */

struct xHttpSocketCtx_ {
  xEventSource  src;     /* event source handle from xEventAdd */
  int           fd;      /* the socket file descriptor         */
  void         *client;  /* back-pointer to xHttpClient_       */
};

/* ───────────────────── Vtable for request polymorphism ───────────────── */

struct xHttpReq_;

/**
 * @brief Virtual table for request completion and cleanup.
 *
 * Different request types (oneshot HTTP, SSE, WebSocket) implement
 * their own handlers.
 */
struct xHttpReqVtable {
  void (*on_done)(struct xHttpReq_ *req, CURLcode result);
  void (*on_cleanup)(struct xHttpReq_ *req);
};

/* ───────────────────── Per-request context ───────────────────── */

struct xHttpReq_ {
  const struct xHttpReqVtable *vt;  /**< vtable for polymorphism      */
  CURL                        *easy;        /* curl easy handle            */
  struct xHttpClient_         *client;      /* back-pointer to client      */
  void                        *arg;         /* user argument               */
  char                         errbuf[CURL_ERROR_SIZE]; /* curl error   */
  int                          cleaned;     /* cleanup already done flag   */

  /* For oneshot HTTP requests */
  xHttpResponseFunc  on_response;  /* completion callback         */
  struct xHttpBuf_   body_buf;      /* response body               */
  struct xHttpBuf_   header_buf;    /* response headers            */
  char              *post_data;     /* copy of POST body (owned)   */
  struct curl_slist *req_headers;   /* custom request headers      */
};

/* ───────────────────── Client internal structure ───────────────────── */

struct xHttpClient_ {
  CURLM       *multi;   /* curl multi handle                   */
  xEventLoop   loop;    /* the event loop we are bound to      */
  xEventTimer  timer;   /* current curl timeout timer, or NULL */
};

#endif /* XHTTP_CLIENT_PRIVATE_H */
