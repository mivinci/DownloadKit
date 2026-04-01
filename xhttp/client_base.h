/*
 * Copyright 2025 The xKit Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * client_base.h - Internal data structures for the xhttp module
 */

#ifndef XHTTP_CLIENT_BASE_H
#define XHTTP_CLIENT_BASE_H

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

/* ───────────────────── Per-request context ───────────────────── */

struct xHttpReq_ {
  CURL              *easy;        /* curl easy handle                    */
  void              *client;      /* back-pointer to xHttpClient_        */
  xHttpResponseFunc  on_response; /* user completion callback            */
  void              *arg;         /* user argument                       */
  struct xHttpBuf_   body_buf;    /* accumulated response body           */
  struct xHttpBuf_   header_buf;  /* accumulated response headers        */
  char              *post_data;   /* copy of POST body (owned), or NULL  */
  struct curl_slist *req_headers; /* custom request headers, or NULL     */
  char               errbuf[CURL_ERROR_SIZE]; /* curl error buffer       */
};

/* ───────────────────── Client internal structure ───────────────────── */

struct xHttpClient_ {
  CURLM       *multi;   /* curl multi handle                   */
  xEventLoop   loop;    /* the event loop we are bound to      */
  xEventTimer  timer;   /* current curl timeout timer, or NULL */
};

#endif /* XHTTP_CLIENT_BASE_H */
