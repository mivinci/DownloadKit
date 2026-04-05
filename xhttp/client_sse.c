/*
 * Copyright 2025 The xKit Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * client_sse.c - SSE (Server-Sent Events) support for xHttpClient
 *
 * Implements xHttpClientGetSse() using the W3C SSE specification:
 * https://html.spec.whatwg.org/multipage/server-sent-events.html
 *
 * Shares the same curl multi handle as the regular HTTP client via
 * the xHttpReqVtable mechanism in client_private.h.
 */

#include "client_private.h"

#include <stddef.h>
#include <stdlib.h>
#include <string.h>

/* ───────────────────── SSE parser ───────────────────────────────────── */

struct xSseParser_ {
  xBuffer buf;   /* raw incoming data                    */
  size_t  pos;   /* parse position within buf            */
  int     error; /* allocation failure occurred          */

  /* Current event fields (reset after each dispatch) */
  char *event_type; /* "message" by default                 */
  char *data;       /* accumulated data lines, joined by \n */
  char *id;         /* last event ID                        */
  int   retry;      /* retry delay in ms, -1 if not set     */
};

static void sse_parser_init(struct xSseParser_ *p) {
  memset(p, 0, sizeof(*p));
  p->buf   = xBufferCreate(4096);
  p->retry = -1;
}

static void sse_parser_reset_event(struct xSseParser_ *p) {
  free(p->event_type);
  p->event_type = NULL;
  free(p->data);
  p->data = NULL;
  /* Keep id and retry across events per spec */
}

static void sse_parser_free(struct xSseParser_ *p) {
  xBufferDestroy(p->buf);
  p->buf = NULL;
  free(p->event_type);
  free(p->data);
  free(p->id);
}

/* Find end of line (\n, \r, or \r\n). Returns pointer to the terminator,
 * or NULL if no complete line is found. */
static const char *find_line_end(const char *s, size_t len) {
  for (size_t i = 0; i < len; i++) {
    if (s[i] == '\n' || s[i] == '\r') return s + i;
  }
  return NULL;
}

static void parse_sse_field(struct xSseParser_ *p, char *line, size_t len) {
  /* Comment line (starts with ':') — ignore */
  if (len > 0 && line[0] == ':') return;

  /* Find colon separator */
  char       *colon = memchr(line, ':', len);
  char       *field;
  const char *value;
  size_t      value_len;

  if (!colon) {
    /* Field with no value — treat value as empty string */
    field     = line;
    value     = "";
    value_len = 0;
  } else {
    *colon    = '\0';
    field     = line;
    value     = colon + 1;
    value_len = len - (size_t)(colon + 1 - line);
    /* Skip single leading space in value (per spec) */
    if (value_len > 0 && *value == ' ') {
      value++;
      value_len--;
    }
  }

  if (strcmp(field, "event") == 0) {
    free(p->event_type);
    p->event_type = strndup(value, value_len);
  } else if (strcmp(field, "data") == 0) {
    if (p->data) {
      /* Append \n + new value */
      size_t old_len = strlen(p->data);
      char  *tmp     = (char *)realloc(p->data, old_len + 1 + value_len + 1);
      if (tmp) {
        p->data          = tmp;
        p->data[old_len] = '\n';
        memcpy(p->data + old_len + 1, value, value_len);
        p->data[old_len + 1 + value_len] = '\0';
      }
    } else {
      p->data = strndup(value, value_len);
    }
  } else if (strcmp(field, "id") == 0) {
    /* Ignore if value contains embedded NUL (per spec: last-event-id
     * must not contain U+0000) — check via field length vs string len */
    if (value_len > 0 && strlen(value) == value_len) {
      free(p->id);
      p->id = strndup(value, value_len);
    }
  } else if (strcmp(field, "retry") == 0) {
    /* Must be all ASCII digits */
    int ok = 1;
    for (size_t i = 0; i < value_len; i++) {
      if (value[i] < '0' || value[i] > '9') {
        ok = 0;
        break;
      }
    }
    if (ok && value_len > 0) p->retry = atoi(value);
  }
  /* Unknown fields are ignored per spec */
}

/* Forward declaration */
struct xSseReq_;

/*
 * Feed raw data into the parser. Dispatches complete events via on_event.
 * Returns 0 to continue, non-zero if the user callback requested close.
 */
static int sse_parser_feed(struct xSseParser_ *p, const char *data, size_t len,
                           xSseEventFunc on_event, void *arg) {
  if (p->error) return -1; /* already failed, abort */

  if (xBufferAppend(&p->buf, data, len) != xErrno_Ok) {
    p->error = 1;
    return -1; /* abort the request */
  }

  const char *buf_data = (const char *)xBufferData(p->buf);
  size_t      buf_len  = xBufferLen(p->buf);

  while (p->pos < buf_len) {
    const char *start     = buf_data + p->pos;
    size_t      remaining = buf_len - p->pos;

    const char *eol = find_line_end(start, remaining);
    if (!eol) break; /* incomplete line — wait for more data */

    size_t line_len = (size_t)(eol - start);

    /* Make a mutable copy of the line for field parsing */
    char *line = (char *)malloc(line_len + 1);
    if (line) {
      memcpy(line, start, line_len);
      line[line_len] = '\0';
    }

    /* Advance past the line terminator (\r\n counts as one) */
    p->pos += line_len + 1;
    if (*eol == '\r' && p->pos < buf_len && buf_data[p->pos] == '\n') p->pos++;

    if (line_len == 0) {
      /* Empty line — dispatch accumulated event */
      if (p->data) {
        xSseEvent ev;
        ev.event = p->event_type ? p->event_type : "message";
        ev.data  = p->data;
        ev.id    = p->id;
        ev.retry = p->retry;

        int r = on_event(&ev, arg);
        sse_parser_reset_event(p);
        if (r != 0) {
          free(line);
          return r; /* user wants to close */
        }
      }
    } else if (line) {
      parse_sse_field(p, line, line_len);
    }

    free(line);
  }

  /* Compact buffer: discard already-parsed bytes */
  if (p->pos > 0) {
    xBufferConsume(p->buf, p->pos);
    p->pos = 0;
  }

  return 0;
}

/* ───────────────────── SSE request ──────────────────────────────────── */

struct xSseReq_ {
  struct xHttpReq_   base;
  xSseEventFunc      on_event;
  xSseDoneFunc       on_done;
  struct xSseParser_ parser;
  struct curl_slist *sse_headers;
};

/* ── Vtable ── */

static void sse_on_done(struct xHttpReq_ *req_, CURLcode result);
static void sse_on_cleanup(struct xHttpReq_ *req_);

static const struct xHttpReqVtable sse_vtable = {
  .on_done    = sse_on_done,
  .on_cleanup = sse_on_cleanup,
};

/* ── curl write callback ── */

static size_t sse_write_callback(char *ptr, size_t size, size_t nmemb,
                                 void *userdata) {
  struct xSseReq_ *req   = (struct xSseReq_ *)userdata;
  size_t           total = size * nmemb;

  int r =
    sse_parser_feed(&req->parser, ptr, total, req->on_event, req->base.arg);
  if (r != 0) {
    /* User requested close — return 0 to signal error to curl */
    return 0;
  }
  return total;
}

/* ── Completion ── */

static void sse_on_done(struct xHttpReq_ *req_, CURLcode result) {
  struct xSseReq_ *req = xContainerOf(req_, struct xSseReq_, base);

  /* Invoke user callback only — cleanup is handled by
   * check_multi_info / destroy_req after on_done returns. */
  if (req->on_done) req->on_done((int)result, req->base.arg);
}

static void sse_on_cleanup(struct xHttpReq_ *req_) {
  struct xSseReq_ *req = xContainerOf(req_, struct xSseReq_, base);
  /* Only clean up request-specific resources here.
   * curl_multi_remove + curl_easy_cleanup + free(req) are handled
   * by destroy_req() which calls this. */
  if (req->sse_headers) curl_slist_free_all(req->sse_headers);
  if (req->base.post_data) free(req->base.post_data);
  sse_parser_free(&req->parser);
}

/* ── Public API ── */

xErrno xHttpClientGetSse(xHttpClient client_, const char *url,
                         xSseEventFunc on_event, xSseDoneFunc on_done,
                         void *arg) {
  xHttpRequestConf config;
  memset(&config, 0, sizeof(config));
  config.url    = url;
  config.method = xHttpMethod_GET;
  return xHttpClientDoSse(client_, &config, on_event, on_done, arg);
}

/* ── Public API: DoSse (generic SSE request) ─────────────────────────── */

xErrno xHttpClientDoSse(xHttpClient client_, const xHttpRequestConf *config,
                        xSseEventFunc on_event, xSseDoneFunc on_done,
                        void *arg) {
  if (!client_ || !config || !config->url || !on_event)
    return xErrno_InvalidArg;

  struct xHttpClient_ *c = (struct xHttpClient_ *)client_;

  struct xSseReq_ *req = (struct xSseReq_ *)calloc(1, sizeof(*req));
  if (!req) return xErrno_NoMemory;

  req->base.vt     = &sse_vtable;
  req->base.client = c;
  req->base.arg    = arg;
  req->on_event    = on_event;
  req->on_done     = on_done;
  sse_parser_init(&req->parser);

  CURL *easy = curl_easy_init();
  if (!easy) goto fail;
  req->base.easy = easy;

  /* SSE-specific headers */
  req->sse_headers = curl_slist_append(NULL, "Accept: text/event-stream");
  req->sse_headers =
    curl_slist_append(req->sse_headers, "Cache-Control: no-cache");
  if (!req->sse_headers) goto fail_easy;

  /* Merge user-provided headers */
  if (config->headers) {
    for (const char **h = config->headers; *h; h++) {
      req->sse_headers = curl_slist_append(req->sse_headers, *h);
    }
  }

  curl_easy_setopt(easy, CURLOPT_URL, config->url);

  /* Method */
  switch (config->method) {
  case xHttpMethod_POST:
    curl_easy_setopt(easy, CURLOPT_POST, 1L);
    break;
  case xHttpMethod_PUT:
    curl_easy_setopt(easy, CURLOPT_CUSTOMREQUEST, "PUT");
    break;
  case xHttpMethod_DELETE:
    curl_easy_setopt(easy, CURLOPT_CUSTOMREQUEST, "DELETE");
    break;
  case xHttpMethod_PATCH:
    curl_easy_setopt(easy, CURLOPT_CUSTOMREQUEST, "PATCH");
    break;
  case xHttpMethod_HEAD:
    curl_easy_setopt(easy, CURLOPT_NOBODY, 1L);
    break;
  default: /* GET */
    curl_easy_setopt(easy, CURLOPT_HTTPGET, 1L);
    break;
  }

  /* Body — make a copy so the caller doesn't need to keep it alive */
  if (config->body && config->body_len > 0) {
    req->base.post_data = (char *)malloc(config->body_len);
    if (!req->base.post_data) goto fail_easy;
    memcpy(req->base.post_data, config->body, config->body_len);
    curl_easy_setopt(easy, CURLOPT_POSTFIELDS, req->base.post_data);
    curl_easy_setopt(easy, CURLOPT_POSTFIELDSIZE, (long)config->body_len);
  }

  /* Per-request timeout */
  if (config->timeout_ms > 0) {
    curl_easy_setopt(easy, CURLOPT_TIMEOUT_MS, config->timeout_ms);
  }

  curl_easy_setopt(easy, CURLOPT_HTTPHEADER, req->sse_headers);
  curl_easy_setopt(easy, CURLOPT_WRITEFUNCTION, sse_write_callback);
  curl_easy_setopt(easy, CURLOPT_WRITEDATA, req);
  curl_easy_setopt(easy, CURLOPT_PRIVATE, &req->base);
  curl_easy_setopt(easy, CURLOPT_ERRORBUFFER, req->base.errbuf);
  curl_easy_setopt(easy, CURLOPT_NOSIGNAL, 1L);

  /* Apply HTTP version: per-request override or client default */
  {
    xHttpVersion ver = config->http_version;
    if (ver == xHttpVersion_Default) ver = c->http_ver;
    if (ver != xHttpVersion_Default) {
      long curl_ver = 0;
      switch (ver) {
      case xHttpVersion_H1:
        curl_ver = CURL_HTTP_VERSION_1_1;
        break;
      case xHttpVersion_H2:
        curl_ver = CURL_HTTP_VERSION_2;
        break;
      case xHttpVersion_H2TLS:
        curl_ver = CURL_HTTP_VERSION_2TLS;
        break;
      case xHttpVersion_H2C:
        curl_ver = CURL_HTTP_VERSION_2_PRIOR_KNOWLEDGE;
        break;
      default:
        break;
      }
      if (curl_ver) curl_easy_setopt(easy, CURLOPT_HTTP_VERSION, curl_ver);
    }
  }

  /* Apply TLS configuration */
  if (c->tls_skip_verify) {
    curl_easy_setopt(easy, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(easy, CURLOPT_SSL_VERIFYHOST, 0L);
  }
  if (c->tls_ca_path) curl_easy_setopt(easy, CURLOPT_CAINFO, c->tls_ca_path);
  if (c->tls_client_cert)
    curl_easy_setopt(easy, CURLOPT_SSLCERT, c->tls_client_cert);
  if (c->tls_client_key)
    curl_easy_setopt(easy, CURLOPT_SSLKEY, c->tls_client_key);
  if (c->tls_key_password)
    curl_easy_setopt(easy, CURLOPT_KEYPASSWD, c->tls_key_password);

  CURLMcode mc = curl_multi_add_handle(c->multi, easy);
  if (mc != CURLM_OK) goto fail_easy;

  return xErrno_Ok;

fail_easy:
  curl_easy_cleanup(easy);
  req->base.easy = NULL;
fail:
  if (req->base.post_data) free(req->base.post_data);
  if (req->sse_headers) curl_slist_free_all(req->sse_headers);
  sse_parser_free(&req->parser);
  free(req);
  return xErrno_SysError;
}
