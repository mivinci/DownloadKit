/*
 * Copyright 2025 The xKit Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * client.c - Asynchronous HTTP client: libcurl multi-socket + xEventLoop
 */

#include "client_private.h"

#include <stdlib.h>
#include <string.h>

/* ── Forward declarations ──────────────────────────────────────────────── */

static void check_multi_info(struct xHttpClient_ *c);
static int  socket_callback(CURL *easy, curl_socket_t fd, int what,
                            void *userp, void *socketp);
static int  timer_callback(CURLM *multi, long timeout_ms, void *userp);
static void fd_ready_callback(int fd, xEventMask mask, void *arg);
static void on_timeout(void *arg);

/* ── Vtable for normal HTTP requests ───────────────────────────────────── */

static void normal_on_done(struct xHttpReq_ *req, CURLcode result);
static void normal_on_cleanup(struct xHttpReq_ *req);

static const struct xHttpReqVtable normal_vtable = {
  .on_done    = normal_on_done,
  .on_cleanup = normal_on_cleanup,
};

/* ── curl data callbacks ───────────────────────────────────────────────── */

static size_t write_callback(char *ptr, size_t size, size_t nmemb,
                             void *userdata) {
  struct xHttpReq_ *req = (struct xHttpReq_ *)userdata;
  size_t total = size * nmemb;
  if (http_buf_append(&req->body_buf, ptr, total) != 0)
    return 0; /* signal error to curl */
  return total;
}

static size_t header_callback(char *ptr, size_t size, size_t nmemb,
                              void *userdata) {
  struct xHttpReq_ *req = (struct xHttpReq_ *)userdata;
  size_t total = size * nmemb;
  if (http_buf_append(&req->header_buf, ptr, total) != 0)
    return 0;
  return total;
}

/* ── Transfer completion check ─────────────────────────────────────────── */

static void check_multi_info(struct xHttpClient_ *c) {
  CURLMsg *msg;
  int msgs_in_queue;

  while ((msg = curl_multi_info_read(c->multi, &msgs_in_queue)) != NULL) {
    if (msg->msg != CURLMSG_DONE)
      continue;

    CURL *easy = msg->easy_handle;
    CURLcode result = msg->data.result;

    struct xHttpReq_ *req = NULL;
    curl_easy_getinfo(easy, CURLINFO_PRIVATE, &req);
    if (!req) continue;

    /* Dispatch via vtable */
    if (req->vt && req->vt->on_done)
      req->vt->on_done(req, result);
  }
}

/* ── Normal HTTP request handlers ──────────────────────────────────────── */

static void normal_on_done(struct xHttpReq_ *req, CURLcode result) {
  /* Build response */
  xHttpResponse resp;
  memset(&resp, 0, sizeof(resp));
  resp.curl_code = (int)result;

  if (result == CURLE_OK) {
    long code = 0;
    curl_easy_getinfo(req->easy, CURLINFO_RESPONSE_CODE, &code);
    resp.status_code = code;
    resp.curl_error  = NULL;
  } else {
    resp.status_code = 0;
    resp.curl_error  = req->errbuf[0] ? req->errbuf
                                       : curl_easy_strerror(result);
  }

  resp.body        = req->body_buf.data   ? req->body_buf.data   : "";
  resp.body_len    = req->body_buf.len;
  resp.headers     = req->header_buf.data ? req->header_buf.data : "";
  resp.headers_len = req->header_buf.len;

  /* Invoke user callback */
  if (req->on_response)
    req->on_response(&resp, req->arg);

  normal_on_cleanup(req);
}

static void normal_on_cleanup(struct xHttpReq_ *req) {
  /* Only clean up request-specific resources here.
   * curl_multi_remove + curl_easy_cleanup + free(req) are handled
   * by destroy_req() which calls this. */
  http_buf_free(&req->body_buf);
  http_buf_free(&req->header_buf);
  if (req->post_data) free(req->post_data);
  if (req->req_headers) curl_slist_free_all(req->req_headers);
}

/* ── Socket callback (CURLMOPT_SOCKETFUNCTION) ─────────────────────────── */

static int socket_callback(CURL *easy, curl_socket_t fd, int what,
                           void *userp, void *socketp) {
  (void)easy;
  struct xHttpClient_ *c = (struct xHttpClient_ *)userp;
  struct xHttpSocketCtx_ *ctx = (struct xHttpSocketCtx_ *)socketp;

  if (what == CURL_POLL_REMOVE) {
    if (ctx) {
      xEventDel(c->loop, ctx->src);
      free(ctx);
      curl_multi_assign(c->multi, fd, NULL);
    }
    return 0;
  }

  /* Map curl poll flags to xEventMask */
  xEventMask mask = 0;
  if (what == CURL_POLL_IN)    mask = xEvent_Read;
  if (what == CURL_POLL_OUT)   mask = xEvent_Write;
  if (what == CURL_POLL_INOUT) mask = xEvent_Read | xEvent_Write;

  if (!ctx) {
    /* First time seeing this fd — register with event loop */
    ctx = (struct xHttpSocketCtx_ *)calloc(1, sizeof(*ctx));
    if (!ctx) return -1;
    ctx->fd     = (int)fd;
    ctx->client = c;
    ctx->src    = xEventAdd(c->loop, (int)fd, mask, fd_ready_callback, ctx);
    if (!ctx->src) {
      free(ctx);
      return -1;
    }
    curl_multi_assign(c->multi, fd, ctx);
  } else {
    /* Already tracked — just update the event mask */
    xEventMod(c->loop, ctx->src, mask);
  }

  return 0;
}

/* ── fd ready callback (xEventFunc) ────────────────────────────────────── */

static void fd_ready_callback(int fd, xEventMask mask, void *arg) {
  struct xHttpSocketCtx_ *ctx = (struct xHttpSocketCtx_ *)arg;
  struct xHttpClient_ *c = (struct xHttpClient_ *)ctx->client;

  int ev_bitmask = 0;
  if (mask & xEvent_Read)  ev_bitmask |= CURL_CSELECT_IN;
  if (mask & xEvent_Write) ev_bitmask |= CURL_CSELECT_OUT;

  int running = 0;
  curl_multi_socket_action(c->multi, (curl_socket_t)fd, ev_bitmask, &running);
  check_multi_info(c);
}

/* ── Timer callback (CURLMOPT_TIMERFUNCTION) ───────────────────────────── */

static int timer_callback(CURLM *multi, long timeout_ms, void *userp) {
  (void)multi;
  struct xHttpClient_ *c = (struct xHttpClient_ *)userp;

  /* Cancel any existing timer */
  if (c->timer) {
    xEventLoopTimerCancel(c->loop, c->timer);
    c->timer = NULL;
  }

  if (timeout_ms == -1) {
    /* curl says no timeout needed */
    return 0;
  }

  /*
   * For timeout_ms == 0, curl wants immediate action. However, this callback
   * may be invoked from within curl_multi_add_handle(), so calling
   * curl_multi_socket_action() here would be reentrant. Schedule a 1ms
   * timer instead to defer the action to the next event loop iteration.
   */
  if (timeout_ms == 0)
    timeout_ms = 1;

  /* Schedule a new timer */
  c->timer = xEventLoopTimerAfter(c->loop, on_timeout, c,
                                  (uint64_t)timeout_ms);
  return 0;
}

/* ── Timer expiry callback (xEventTimerFunc) ───────────────────────────── */

static void on_timeout(void *arg) {
  struct xHttpClient_ *c = (struct xHttpClient_ *)arg;
  c->timer = NULL; /* timer has fired, handle is now invalid */

  int running = 0;
  curl_multi_socket_action(c->multi, CURL_SOCKET_TIMEOUT, 0, &running);
  check_multi_info(c);
}

/* ── Lifecycle: Create / Destroy ───────────────────────────────────────── */

xHttpClient xHttpClientCreate(xEventLoop loop) {
  if (!loop) return NULL;

  struct xHttpClient_ *c =
      (struct xHttpClient_ *)calloc(1, sizeof(struct xHttpClient_));
  if (!c) return NULL;

  c->multi = curl_multi_init();
  if (!c->multi) {
    free(c);
    return NULL;
  }

  c->loop  = loop;
  c->timer = NULL;

  curl_multi_setopt(c->multi, CURLMOPT_SOCKETFUNCTION, socket_callback);
  curl_multi_setopt(c->multi, CURLMOPT_SOCKETDATA, c);
  curl_multi_setopt(c->multi, CURLMOPT_TIMERFUNCTION, timer_callback);
  curl_multi_setopt(c->multi, CURLMOPT_TIMERDATA, c);

  return (xHttpClient)c;
}

/**
 * @brief Helper: clean up a single request context and its easy handle.
 */
static void destroy_req(struct xHttpClient_ *c, CURL *easy,
                        struct xHttpReq_ *req, int notify) {
  if (req && notify && req->on_response) {
    xHttpResponse resp;
    memset(&resp, 0, sizeof(resp));
    resp.curl_code  = CURLE_ABORTED_BY_CALLBACK;
    resp.curl_error = "Request aborted: client destroyed";
    resp.body       = "";
    resp.headers    = "";
    req->on_response(&resp, req->arg);
  }

  curl_multi_remove_handle(c->multi, easy);
  curl_easy_cleanup(easy);

  if (req && !req->cleaned) {
    req->cleaned = 1;
    /* Use vtable cleanup if available */
    if (req->vt && req->vt->on_cleanup) {
      req->vt->on_cleanup(req);
    } else {
      http_buf_free(&req->body_buf);
      http_buf_free(&req->header_buf);
      if (req->post_data) free(req->post_data);
      if (req->req_headers) curl_slist_free_all(req->req_headers);
      free(req);
    }
  }
}

void xHttpClientDestroy(xHttpClient client) {
  if (!client) return;
  struct xHttpClient_ *c = (struct xHttpClient_ *)client;

  /* Cancel the curl timeout timer if active */
  if (c->timer) {
    xEventLoopTimerCancel(c->loop, c->timer);
    c->timer = NULL;
  }

  /*
   * Drain any already-completed transfers first, then forcibly remove
   * all remaining in-flight handles.
   */
  CURLMsg *msg;
  int msgs_in_queue;

  while ((msg = curl_multi_info_read(c->multi, &msgs_in_queue)) != NULL) {
    if (msg->msg != CURLMSG_DONE) continue;
    CURL *easy = msg->easy_handle;
    struct xHttpReq_ *req = NULL;
    curl_easy_getinfo(easy, CURLINFO_PRIVATE, &req);
    destroy_req(c, easy, req, 1);
  }

  /*
   * Forcibly remove any still-running easy handles. We use
   * curl_multi_remove_handle directly rather than trying to trigger
   * completion, which avoids reentrant socket_callback issues.
   */
  CURL **handles = curl_multi_get_handles(c->multi);
  if (handles) {
    for (int i = 0; handles[i] != NULL; i++) {
      CURL *easy = handles[i];
      struct xHttpReq_ *req = NULL;
      curl_easy_getinfo(easy, CURLINFO_PRIVATE, &req);
      destroy_req(c, easy, req, 1);
    }
    curl_free(handles);
  }

  curl_multi_cleanup(c->multi);
  free(c);
}

/* ── Internal: configure and submit an easy handle ─────────────────────── */

static xErrno http_submit(struct xHttpClient_ *c, struct xHttpReq_ *req) {
  /* Common curl options */
  curl_easy_setopt(req->easy, CURLOPT_WRITEFUNCTION, write_callback);
  curl_easy_setopt(req->easy, CURLOPT_WRITEDATA, req);
  curl_easy_setopt(req->easy, CURLOPT_HEADERFUNCTION, header_callback);
  curl_easy_setopt(req->easy, CURLOPT_HEADERDATA, req);
  curl_easy_setopt(req->easy, CURLOPT_PRIVATE, req);
  curl_easy_setopt(req->easy, CURLOPT_ERRORBUFFER, req->errbuf);
  curl_easy_setopt(req->easy, CURLOPT_NOSIGNAL, 1L);

  CURLMcode mc = curl_multi_add_handle(c->multi, req->easy);
  if (mc != CURLM_OK) {
    curl_easy_cleanup(req->easy);
    http_buf_free(&req->body_buf);
    http_buf_free(&req->header_buf);
    if (req->post_data) free(req->post_data);
    if (req->req_headers) curl_slist_free_all(req->req_headers);
    free(req);
    return xErrno_Unknown;
  }

  return xErrno_Ok;
}

static struct xHttpReq_ *http_req_new(struct xHttpClient_ *c, const char *url,
                                      xHttpResponseFunc on_response,
                                      void *arg) {
  struct xHttpReq_ *req =
      (struct xHttpReq_ *)calloc(1, sizeof(struct xHttpReq_));
  if (!req) return NULL;

  req->easy = curl_easy_init();
  if (!req->easy) {
    free(req);
    return NULL;
  }

  req->vt          = &normal_vtable;  /* set vtable for normal requests */
  req->client      = c;
  req->on_response = on_response;
  req->arg         = arg;
  req->post_data   = NULL;
  req->req_headers = NULL;
  memset(req->errbuf, 0, sizeof(req->errbuf));
  http_buf_init(&req->body_buf);
  http_buf_init(&req->header_buf);

  curl_easy_setopt(req->easy, CURLOPT_URL, url);

  return req;
}

/* ── Public API: GET ───────────────────────────────────────────────────── */

xErrno xHttpClientGet(xHttpClient client, const char *url,
                       xHttpResponseFunc on_response, void *arg) {
  if (!client || !url) return xErrno_Unknown;
  struct xHttpClient_ *c = (struct xHttpClient_ *)client;

  struct xHttpReq_ *req = http_req_new(c, url, on_response, arg);
  if (!req) return xErrno_Unknown;

  curl_easy_setopt(req->easy, CURLOPT_HTTPGET, 1L);

  return http_submit(c, req);
}

/* ── Public API: POST ──────────────────────────────────────────────────── */

xErrno xHttpClientPost(xHttpClient client, const char *url,
                        const char *body, size_t body_len,
                        xHttpResponseFunc on_response, void *arg) {
  if (!client || !url) return xErrno_Unknown;
  struct xHttpClient_ *c = (struct xHttpClient_ *)client;

  struct xHttpReq_ *req = http_req_new(c, url, on_response, arg);
  if (!req) return xErrno_Unknown;

  curl_easy_setopt(req->easy, CURLOPT_POST, 1L);

  if (body && body_len > 0) {
    /* Make a copy of the body so the caller doesn't need to keep it alive */
    req->post_data = (char *)malloc(body_len);
    if (!req->post_data) {
      curl_easy_cleanup(req->easy);
      free(req);
      return xErrno_Unknown;
    }
    memcpy(req->post_data, body, body_len);
    curl_easy_setopt(req->easy, CURLOPT_POSTFIELDS, req->post_data);
    curl_easy_setopt(req->easy, CURLOPT_POSTFIELDSIZE, (long)body_len);
  } else {
    curl_easy_setopt(req->easy, CURLOPT_POSTFIELDSIZE, 0L);
  }

  return http_submit(c, req);
}

/* ── Public API: Do (generic request) ──────────────────────────────────── */

xErrno xHttpClientDo(xHttpClient client, const xHttpRequestConf *config,
                      xHttpResponseFunc on_response, void *arg) {
  if (!client || !config || !config->url) return xErrno_Unknown;
  struct xHttpClient_ *c = (struct xHttpClient_ *)client;

  struct xHttpReq_ *req = http_req_new(c, config->url, on_response, arg);
  if (!req) return xErrno_Unknown;

  /* Method */
  switch (config->method) {
    case xHttpMethod_GET:
      curl_easy_setopt(req->easy, CURLOPT_HTTPGET, 1L);
      break;
    case xHttpMethod_POST:
      curl_easy_setopt(req->easy, CURLOPT_POST, 1L);
      break;
    case xHttpMethod_PUT:
      curl_easy_setopt(req->easy, CURLOPT_CUSTOMREQUEST, "PUT");
      break;
    case xHttpMethod_DELETE:
      curl_easy_setopt(req->easy, CURLOPT_CUSTOMREQUEST, "DELETE");
      break;
    case xHttpMethod_PATCH:
      curl_easy_setopt(req->easy, CURLOPT_CUSTOMREQUEST, "PATCH");
      break;
    case xHttpMethod_HEAD:
      curl_easy_setopt(req->easy, CURLOPT_NOBODY, 1L);
      break;
    default:
      curl_easy_setopt(req->easy, CURLOPT_HTTPGET, 1L);
      break;
  }

  /* Body */
  if (config->body && config->body_len > 0) {
    req->post_data = (char *)malloc(config->body_len);
    if (!req->post_data) {
      curl_easy_cleanup(req->easy);
      free(req);
      return xErrno_Unknown;
    }
    memcpy(req->post_data, config->body, config->body_len);
    curl_easy_setopt(req->easy, CURLOPT_POSTFIELDS, req->post_data);
    curl_easy_setopt(req->easy, CURLOPT_POSTFIELDSIZE,
                     (long)config->body_len);
  }

  /* Custom headers */
  if (config->headers) {
    for (const char **h = config->headers; *h; h++) {
      req->req_headers = curl_slist_append(req->req_headers, *h);
    }
    if (req->req_headers) {
      curl_easy_setopt(req->easy, CURLOPT_HTTPHEADER, req->req_headers);
    }
  }

  /* Per-request timeout */
  if (config->timeout_ms > 0) {
    curl_easy_setopt(req->easy, CURLOPT_TIMEOUT_MS, config->timeout_ms);
  }

  return http_submit(c, req);
}
