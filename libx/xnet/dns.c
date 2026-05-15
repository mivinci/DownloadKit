/*
 * Copyright 2025 The moo Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * dns.c - Asynchronous DNS resolution via thread-pool offload
 */

#include <xnet/dns.h>

#include <xbase/atomic.h>

#include <arpa/inet.h>
#include <stdlib.h>
#include <string.h>

/* ───────────────────── Internal types ───────────────────── */

/**
 * @brief Internal request state, shared between the submitting thread,
 *        the worker thread, and the done callback.
 */
struct xDnsRequest_ {
  /* Inputs (immutable after creation) */
  char           *hostname;
  char           *service;
  struct addrinfo hints;

  /* Callback (immutable after creation) */
  xDnsCallback callback;
  void        *arg;

  /* Work handle for xEventLoopWorkCancel() */
  xEventWork work;

  /* Cancellation flag (fallback when work is already running) */
  int cancelled;
};

/* ───────────────────── Error mapping ───────────────────── */

/**
 * @brief Map getaddrinfo EAI_* error codes to xErrno.
 */
static xErrno eai_to_xerrno(int eai) {
  switch (eai) {
  case 0:
    return xErrno_Ok;
  case EAI_NONAME:
    return xErrno_DnsNotFound;
  case EAI_AGAIN:
    return xErrno_DnsTempFail;
  case EAI_MEMORY:
    return xErrno_NoMemory;
  default:
    return xErrno_DnsError;
  }
}

/* ───────────────────── Result helpers ───────────────────── */

/**
 * @brief Build an xDnsResult from a getaddrinfo result chain.
 *
 * On success, returns a heap-allocated xDnsResult with error == xErrno_Ok
 * and a linked list of xDnsAddr nodes. On failure (OOM while building the
 * list), returns a result with error == xErrno_NoMemory and addrs == NULL.
 */
static xDnsResult *result_from_addrinfo(struct addrinfo *res) {
  xDnsResult *result = (xDnsResult *)calloc(1, sizeof(*result));
  if (!result) return NULL;

  result->error = xErrno_Ok;
  result->addrs = NULL;

  xDnsAddr **tail = &result->addrs;

  for (struct addrinfo *ai = res; ai != NULL; ai = ai->ai_next) {
    xDnsAddr *node = (xDnsAddr *)calloc(1, sizeof(*node));
    if (!node) {
      /* OOM: free what we've built so far */
      xDnsResultFree(result);
      xDnsResult *err_result = (xDnsResult *)calloc(1, sizeof(*err_result));
      if (err_result) {
        err_result->error = xErrno_NoMemory;
        err_result->addrs = NULL;
      }
      return err_result;
    }

    memcpy(&node->addr, ai->ai_addr, ai->ai_addrlen);
    node->addrlen  = ai->ai_addrlen;
    node->family   = ai->ai_family;
    node->socktype = ai->ai_socktype;
    node->protocol = ai->ai_protocol;
    node->next     = NULL;

    *tail = node;
    tail  = &node->next;
  }

  return result;
}

/**
 * @brief Build an error-only xDnsResult (no addresses).
 */
static xDnsResult *result_error(xErrno err) {
  xDnsResult *result = (xDnsResult *)calloc(1, sizeof(*result));
  if (!result) return NULL;
  result->error = err;
  result->addrs = NULL;
  return result;
}

/* ───────────────────── IP literal detection ───────────────────── */

/**
 * @brief Check if hostname is an IP address literal (IPv4 or IPv6).
 *
 * If it is, set AI_NUMERICHOST in the hints to skip actual DNS lookup.
 */
static void detect_ip_literal(const char *hostname, struct addrinfo *hints) {
  unsigned char buf[sizeof(struct in6_addr)];

  if (inet_pton(AF_INET, hostname, buf) == 1 ||
      inet_pton(AF_INET6, hostname, buf) == 1) {
    hints->ai_flags |= AI_NUMERICHOST;
  }
}

/* ───────────────────── Thread-pool callbacks ───────────────────── */

/**
 * @brief Worker function executed on a thread-pool thread.
 *
 * Calls getaddrinfo() and converts the result to an xDnsResult.
 * The returned pointer is passed as `result` to the done callback.
 */
static void *dns_work_fn(void *arg) {
  struct xDnsRequest_ *req = (struct xDnsRequest_ *)arg;

  /* Detect IP literals and set AI_NUMERICHOST to avoid DNS lookup */
  detect_ip_literal(req->hostname, &req->hints);

  struct addrinfo *res = NULL;
  int eai = getaddrinfo(req->hostname, req->service, &req->hints, &res);

  xDnsResult *result;
  if (eai != 0) {
    result = result_error(eai_to_xerrno(eai));
  } else {
    result = result_from_addrinfo(res);
    freeaddrinfo(res);
    if (!result) {
      result = result_error(xErrno_NoMemory);
    }
  }

  return (void *)result;
}

/**
 * @brief Done callback invoked on the event loop thread.
 *
 * If the request was cancelled, frees everything silently.
 * Otherwise, invokes the user callback with the result.
 */
static void dns_done_fn(void *arg, void *work_result) {
  struct xDnsRequest_ *req    = (struct xDnsRequest_ *)arg;
  xDnsResult          *result = (xDnsResult *)work_result;

  if (xAtomicLoad(&req->cancelled, xAtomicAcquire)) {
    /* Cancelled: discard result, do not invoke user callback */
    if (result) xDnsResultFree(result);
  } else {
    /* Deliver result to user */
    if (!result) {
      /* Should not happen, but be defensive */
      result = result_error(xErrno_NoMemory);
    }
    req->callback(result, req->arg);
  }

  /* Clean up the request */
  free(req->hostname);
  free(req->service);
  free(req);
}

/* ───────────────────── Public API ───────────────────── */

xDnsQuery xDnsResolve(xEventLoop loop, const char *hostname,
                      const char *service, const struct addrinfo *hints,
                      xDnsCallback callback, void *arg) {
  if (!loop || !hostname || !hostname[0] || !callback) {
    return NULL;
  }

  struct xDnsRequest_ *req = (struct xDnsRequest_ *)calloc(1, sizeof(*req));
  if (!req) return NULL;

  req->hostname = strdup(hostname);
  if (!req->hostname) goto fail;

  if (service) {
    req->service = strdup(service);
    if (!req->service) goto fail;
  }

  if (hints) {
    req->hints = *hints;
  } else {
    /* Default hints: AF_UNSPEC + SOCK_STREAM */
    memset(&req->hints, 0, sizeof(req->hints));
    req->hints.ai_family   = AF_UNSPEC;
    req->hints.ai_socktype = SOCK_STREAM;
  }

  req->callback  = callback;
  req->arg       = arg;
  req->cancelled = 0;

  xErrno err = xEventLoopSubmit(loop, NULL, dns_work_fn, dns_done_fn, req, &req->work);
  if (err != xErrno_Ok) goto fail;

  return (xDnsQuery)req;

fail:
  if (req) {
    free(req->hostname);
    free(req->service);
    free(req);
  }
  return NULL;
}

void xDnsCancel(xEventLoop loop, xDnsQuery query) {
  if (!loop || !query) return;

  struct xDnsRequest_ *req = (struct xDnsRequest_ *)query;

  /* Fast path: if work_fn has not started yet, cancel it entirely.
   * On success, done_fn will NOT be called, so we clean up here. */
  if (xEventLoopWorkCancel(loop, req->work) == xErrno_Ok) {
    free(req->hostname);
    free(req->service);
    free(req);
    return;
  }

  /* Slow path: work_fn is already running or done.
   * Set the flag so done_fn skips the user callback. */
  xAtomicStore(&req->cancelled, 1, xAtomicRelease);
}

void xDnsResultFree(xDnsResult *result) {
  if (!result) return;

  xDnsAddr *addr = result->addrs;
  while (addr) {
    xDnsAddr *next = addr->next;
    free(addr);
    addr = next;
  }
  free(result);
}
