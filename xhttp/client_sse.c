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

#include <stdlib.h>
#include <string.h>

/* ───────────────────── SSE parser ───────────────────────────────────── */

struct xSseParser_ {
  struct xHttpBuf_ buf;   /* raw incoming data                    */
  size_t           pos;   /* parse position within buf            */

  /* Current event fields (reset after each dispatch) */
  char *event_type;       /* "message" by default                 */
  char *data;             /* accumulated data lines, joined by \n */
  char *id;               /* last event ID                        */
  int   retry;            /* retry delay in ms, -1 if not set     */
};

static void sse_parser_init(struct xSseParser_ *p) {
  memset(p, 0, sizeof(*p));
  p->retry = -1;
}

static void sse_parser_reset_event(struct xSseParser_ *p) {
  free(p->event_type); p->event_type = NULL;
  free(p->data);       p->data       = NULL;
  /* Keep id and retry across events per spec */
}

static void sse_parser_free(struct xSseParser_ *p) {
  http_buf_free(&p->buf);
  free(p->event_type);
  free(p->data);
  free(p->id);
}

/* Find end of line (\n, \r, or \r\n). Returns pointer to the terminator,
 * or NULL if no complete line is found. */
static const char *find_line_end(const char *s, size_t len) {
  for (size_t i = 0; i < len; i++) {
    if (s[i] == '\n' || s[i] == '\r')
      return s + i;
  }
  return NULL;
}

static void parse_sse_field(struct xSseParser_ *p, char *line, size_t len) {
  /* Comment line (starts with ':') — ignore */
  if (len > 0 && line[0] == ':') return;

  /* Find colon separator */
  char *colon = memchr(line, ':', len);
  const char *field;
  const char *value;

  if (!colon) {
    /* Field with no value — treat value as empty string */
    field = line;
    value = "";
  } else {
    *colon = '\0';
    field  = line;
    value  = colon + 1;
    /* Skip single leading space in value (per spec) */
    if (*value == ' ') value++;
  }

  if (strcmp(field, "event") == 0) {
    free(p->event_type);
    p->event_type = strdup(value);
  } else if (strcmp(field, "data") == 0) {
    if (p->data) {
      /* Append \n + new value */
      size_t old_len = strlen(p->data);
      size_t val_len = strlen(value);
      char *tmp = (char *)realloc(p->data, old_len + 1 + val_len + 1);
      if (tmp) {
        p->data = tmp;
        p->data[old_len] = '\n';
        memcpy(p->data + old_len + 1, value, val_len + 1);
      }
    } else {
      p->data = strdup(value);
    }
  } else if (strcmp(field, "id") == 0) {
    /* Ignore if value contains U+0000 (per spec) */
    if (!memchr(value, '\0', strlen(value))) {
      free(p->id);
      p->id = strdup(value);
    }
  } else if (strcmp(field, "retry") == 0) {
    /* Must be all ASCII digits */
    int ok = 1;
    for (const char *c = value; *c; c++) {
      if (*c < '0' || *c > '9') { ok = 0; break; }
    }
    if (ok && *value) p->retry = atoi(value);
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
  if (http_buf_append(&p->buf, data, len) != 0)
    return 0; /* allocation failure — keep going, data lost */

  while (p->pos < p->buf.len) {
    const char *start = p->buf.data + p->pos;
    size_t remaining  = p->buf.len - p->pos;

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
    if (*eol == '\r' && p->pos < p->buf.len && p->buf.data[p->pos] == '\n')
      p->pos++;

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
        free(line);
        if (r != 0) return r; /* user wants to close */
      }
    } else if (line) {
      parse_sse_field(p, line, line_len);
    }

    free(line);
  }

  /* Compact buffer: discard already-parsed bytes */
  if (p->pos > 0) {
    size_t remaining = p->buf.len - p->pos;
    if (remaining > 0)
      memmove(p->buf.data, p->buf.data + p->pos, remaining);
    p->buf.len = remaining;
    p->pos     = 0;
  }

  return 0;
}

/* ───────────────────── SSE request ──────────────────────────────────── */

struct xSseReq_ {
  struct xHttpReq_   base;     /* MUST be first — cast between types */
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
  struct xSseReq_ *req = (struct xSseReq_ *)userdata;
  size_t total = size * nmemb;

  int r = sse_parser_feed(&req->parser, ptr, total,
                           req->on_event, req->base.arg);
  if (r != 0) {
    /* User requested close — return 0 to signal error to curl */
    return 0;
  }
  return total;
}

/* ── Completion ── */

static void sse_on_done(struct xHttpReq_ *req_, CURLcode result) {
  struct xSseReq_ *req = (struct xSseReq_ *)req_;

  if (req->on_done)
    req->on_done((int)result, req->base.arg);

  sse_on_cleanup(req_);
}

static void sse_on_cleanup(struct xHttpReq_ *req_) {
  struct xSseReq_ *req = (struct xSseReq_ *)req_;

  curl_multi_remove_handle(req->base.client->multi, req->base.easy);
  curl_easy_cleanup(req->base.easy);
  if (req->sse_headers) curl_slist_free_all(req->sse_headers);
  sse_parser_free(&req->parser);
  free(req);
}

/* ── Public API ── */

xErrno xHttpClientGetSse(xHttpClient client_, const char *url,
                          xSseEventFunc on_event,
                          xSseDoneFunc on_done,
                          void *arg) {
  if (!client_ || !url || !on_event) return xErrno_InvalidArg;

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
  req->sse_headers = curl_slist_append(req->sse_headers, "Cache-Control: no-cache");
  if (!req->sse_headers) goto fail_easy;

  curl_easy_setopt(easy, CURLOPT_URL, url);
  curl_easy_setopt(easy, CURLOPT_HTTPGET, 1L);
  curl_easy_setopt(easy, CURLOPT_HTTPHEADER, req->sse_headers);
  curl_easy_setopt(easy, CURLOPT_WRITEFUNCTION, sse_write_callback);
  curl_easy_setopt(easy, CURLOPT_WRITEDATA, req);
  curl_easy_setopt(easy, CURLOPT_PRIVATE, &req->base);
  curl_easy_setopt(easy, CURLOPT_ERRORBUFFER, req->base.errbuf);
  curl_easy_setopt(easy, CURLOPT_NOSIGNAL, 1L);

  CURLMcode mc = curl_multi_add_handle(c->multi, easy);
  if (mc != CURLM_OK) goto fail_easy;

  return xErrno_Ok;

fail_easy:
  curl_easy_cleanup(easy);
  req->base.easy = NULL;
fail:
  if (req->sse_headers) curl_slist_free_all(req->sse_headers);
  sse_parser_free(&req->parser);
  free(req);
  return xErrno_SysError;
}
