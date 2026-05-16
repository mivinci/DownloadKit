/*
 * Copyright 2025 The libx Authors. All rights reserved.
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
#include <x/base/base.h>
#include <x/buf/buf.h>
#include <x/http/client.h>

/* ───────────────────── Per-socket context ───────────────────── */

XDEF_STRUCT(xHttpSocketCtx_) {
  xEventSource src;    /* event source handle from xEventAdd */
  int          fd;     /* the socket file descriptor         */
  void        *client; /* back-pointer to xHttpClient_       */
};

/* ───────────────────── Vtable for request polymorphism ───────────────── */

struct xHttpReq_;

/**
 * @brief Virtual table for request completion and cleanup.
 *
 * Different request types (oneshot HTTP, SSE, WebSocket) implement
 * their own handlers.
 */
XDEF_STRUCT(xHttpReqVtable) {
  void (*on_done)(struct xHttpReq_ *req, CURLcode result);
  void (*on_cleanup)(struct xHttpReq_ *req);
};

/* ───────────────────── Per-request context ───────────────────── */

XDEF_STRUCT(xHttpReq_) {
  const struct xHttpReqVtable *vt;     /**< vtable for polymorphism      */
  CURL                        *easy;   /* curl easy handle            */
  struct xHttpClient_         *client; /* back-pointer to client      */
  void                        *arg;    /* user argument               */
  char                         errbuf[CURL_ERROR_SIZE]; /* curl error   */
  int                          cleaned; /* cleanup already done flag   */

  /* For oneshot HTTP requests */
  xHttpResponseFunc  on_response; /* completion callback         */
  xBuffer            body_buf;    /* response body               */
  xBuffer            header_buf;  /* response headers            */
  char              *post_data;   /* copy of POST body (owned)   */
  struct curl_slist *req_headers; /* custom request headers      */
  struct xHttpReq_  *next;        /* intrusive list link (client) */
};
/* ───────────────────── Client internal structure ───────────────────── */

XDEF_STRUCT(xHttpClient_) {
  CURLM       *multi;    /* curl multi handle                   */
  xEventLoop   loop;     /* the event loop we are bound to      */
  xEventTimer  timer;    /* current curl timeout timer, or NULL */
  xHttpVersion http_ver; /* default HTTP version for requests   */

  /* TLS configuration (owned copies, freed on destroy) */
  char *tls_ca;           /* CA cert file path, or NULL          */
  char *tls_cert;         /* client certificate path, or NULL    */
  char *tls_key;          /* client private key path, or NULL    */
  char *tls_key_password; /* private key password, or NULL       */
  int   tls_skip_verify;  /* skip peer & host verification       */

  struct xHttpReq_ *reqs; /* linked list of in-flight requests   */
};

#endif /* XHTTP_CLIENT_PRIVATE_H */
