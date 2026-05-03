/*
 * Copyright 2025 The xKit Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * url.c - Lightweight URL parser implementation
 */

#include "url.h"

#include <stdlib.h>
#include <string.h>

/* ───────────────── helpers ───────────────── */

/**
 * Check if two strings match (case-insensitive, bounded).
 */
static int scheme_eq(const char *a, size_t alen, const char *b) {
  size_t blen = strlen(b);
  if (alen != blen) return 0;
  for (size_t i = 0; i < alen; i++) {
    char ca = a[i];
    char cb = b[i];
    /* Cheap ASCII tolower */
    if (ca >= 'A' && ca <= 'Z') ca += 32;
    if (cb >= 'A' && cb <= 'Z') cb += 32;
    if (ca != cb) return 0;
  }
  return 1;
}

/* ───────────────── xUrlParse ───────────────── */

xErrno xUrlParse(const char *raw, xUrl *url) {
  if (!raw || !url) return xErrno_InvalidArg;

  memset(url, 0, sizeof(*url));

  /* Own a copy so the caller can discard the original */
  char *copy = strdup(raw);
  if (!copy) return xErrno_NoMemory;
  url->raw_ = copy;

  const char *p = copy;

  /* ── scheme ── */
  const char *colon = strstr(p, "://");
  if (!colon || colon == p) {
    xUrlFree(url);
    return xErrno_InvalidArg;
  }

  url->scheme     = p;
  url->scheme_len = (size_t)(colon - p);
  p               = colon + 3; /* skip "://" */

  /* ── authority: [userinfo@]host[:port] ── */
  /* Find the end of the authority section */
  const char *authority = p;
  const char *auth_end  = p;
  while (*auth_end && *auth_end != '/' && *auth_end != '?' &&
         *auth_end != '#') {
    auth_end++;
  }

  if (authority == auth_end) {
    xUrlFree(url);
    return xErrno_InvalidArg;
  }

  /* Check for userinfo */
  const char *at = NULL;
  for (const char *s = authority; s < auth_end; s++) {
    if (*s == '@') {
      at = s;
      break;
    }
  }

  const char *host_start;
  if (at) {
    url->userinfo     = authority;
    url->userinfo_len = (size_t)(at - authority);
    host_start        = at + 1;
  } else {
    host_start = authority;
  }

  /* Check for port (scan from the right for ':') */
  const char *port_colon = NULL;
  if (*host_start == '[') {
    /* IPv6 literal: [::1]:8080 */
    const char *bracket =
      memchr(host_start, ']', (size_t)(auth_end - host_start));
    if (!bracket) {
      xUrlFree(url);
      return xErrno_InvalidArg;
    }
    url->host     = host_start + 1; /* skip '[' */
    url->host_len = (size_t)(bracket - host_start - 1);
    if (bracket + 1 < auth_end && bracket[1] == ':') {
      port_colon = bracket + 1;
    }
  } else {
    /* Scan backwards for the last ':' */
    for (const char *s = auth_end - 1; s >= host_start; s--) {
      if (*s == ':') {
        port_colon = s;
        break;
      }
    }
    if (port_colon) {
      url->host     = host_start;
      url->host_len = (size_t)(port_colon - host_start);
    } else {
      url->host     = host_start;
      url->host_len = (size_t)(auth_end - host_start);
    }
  }

  if (url->host_len == 0) {
    xUrlFree(url);
    return xErrno_InvalidArg;
  }

  if (port_colon) {
    url->port     = port_colon + 1;
    url->port_len = (size_t)(auth_end - port_colon - 1);
  }

  p = auth_end;

  /* ── path ── */
  if (*p == '/') {
    url->path            = p;
    const char *path_end = p;
    while (*path_end && *path_end != '?' && *path_end != '#') {
      path_end++;
    }
    url->path_len = (size_t)(path_end - p);
    p             = path_end;
  }

  /* ── query ── */
  if (*p == '?') {
    p++; /* skip '?' */
    url->query        = p;
    const char *q_end = p;
    while (*q_end && *q_end != '#') {
      q_end++;
    }
    url->query_len = (size_t)(q_end - p);
    p              = q_end;
  }

  /* ── fragment ── */
  if (*p == '#') {
    p++; /* skip '#' */
    url->fragment     = p;
    url->fragment_len = strlen(p);
  }

  return xErrno_Ok;
}

/* ───────────────── xUrlFree ───────────────── */

void xUrlFree(xUrl *url) {
  if (!url) return;
  free(url->raw_);
  memset(url, 0, sizeof(*url));
}

/* ───────────────── xUrlPort ───────────────── */

uint16_t xUrlPort(const xUrl *url) {
  if (!url) return 0;

  /* Explicit port in the URL */
  if (url->port && url->port_len > 0) {
    /* Convert bounded string to integer */
    uint16_t port = 0;
    for (size_t i = 0; i < url->port_len; i++) {
      char c = url->port[i];
      if (c < '0' || c > '9') return 0;
      unsigned next = (unsigned)port * 10 + (unsigned)(c - '0');
      if (next > 65535) return 0;
      port = (uint16_t)next;
    }
    return port;
  }

  /* Default port by scheme */
  if (scheme_eq(url->scheme, url->scheme_len, "http") ||
      scheme_eq(url->scheme, url->scheme_len, "ws")) {
    return 80;
  }
  if (scheme_eq(url->scheme, url->scheme_len, "https") ||
      scheme_eq(url->scheme, url->scheme_len, "wss")) {
    return 443;
  }

  return 0;
}
