/*
 * Copyright 2025 The moo Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * dns.h - Asynchronous DNS resolution via thread-pool offload
 */

#ifndef XNET_DNS_H
#define XNET_DNS_H

#include <x/base/base.h>
#include <x/base/error.h>
#include <x/base/event.h>

#include <netdb.h>
#include <sys/socket.h>

/**
 * @brief A single resolved address entry (linked list node).
 */
XDEF_STRUCT(xDnsAddr) {
  struct sockaddr_storage addr; /**< Resolved socket address              */
  socklen_t               addrlen;  /**< Length of the address            */
  int                     family;   /**< Address family (AF_INET / AF_INET6) */
  int                     socktype; /**< Socket type (SOCK_STREAM / SOCK_DGRAM) */
  int                     protocol; /**< Protocol (IPPROTO_TCP / IPPROTO_UDP) */
  xDnsAddr               *next;     /**< Next address in the list, or NULL */
};

/**
 * @brief DNS resolution result.
 *
 * On success, `error` is xErrno_Ok and `addrs` points to a linked list
 * of resolved addresses. On failure, `error` is set and `addrs` is NULL.
 */
XDEF_STRUCT(xDnsResult) {
  xErrno    error; /**< xErrno_Ok on success, or an error code */
  xDnsAddr *addrs; /**< Linked list of resolved addresses, or NULL */
};

/**
 * @brief Opaque handle to a pending DNS query.
 *
 * Returned by xDnsResolve(). Can be passed to xDnsCancel() to cancel
 * the query before the callback fires.
 */
XDEF_HANDLE(xDnsQuery);

/**
 * @brief Callback invoked when DNS resolution completes.
 *
 * Always called on the event loop thread. The callee takes ownership of
 * `result` and must call xDnsResultFree() when done.
 *
 * @param result  The resolution result (never NULL).
 * @param arg     User-provided argument from xDnsResolve().
 */
typedef void (*xDnsCallback)(xDnsResult *result, void *arg);

/**
 * @brief Asynchronously resolve a hostname to one or more addresses.
 *
 * The actual getaddrinfo() call is offloaded to the event loop's thread
 * pool. When resolution completes, `callback` is invoked on the event
 * loop thread with the result.
 *
 * @param loop      Event loop (must not be NULL).
 * @param hostname  Hostname to resolve (must not be NULL or empty).
 * @param service   Service name or port string, or NULL for address-only.
 * @param hints     Optional addrinfo hints (family, socktype, etc.).
 *                  If NULL, defaults to AF_UNSPEC + SOCK_STREAM.
 * @param callback  Completion callback (must not be NULL).
 * @param arg       Argument forwarded to callback.
 * @return          A query handle, or NULL on invalid arguments.
 */
XCAPI(xDnsQuery) xDnsResolve(xEventLoop loop, const char *hostname,
                             const char *service,
                             const struct addrinfo *hints,
                             xDnsCallback callback, void *arg);

/**
 * @brief Cancel a pending DNS query.
 *
 * If the query is still in progress, the callback will not be invoked.
 * If the query has already completed, this is a no-op.
 * Passing NULL is safe (no-op).
 *
 * @param loop   The event loop.
 * @param query  Query handle returned by xDnsResolve(), or NULL.
 */
XCAPI(void) xDnsCancel(xEventLoop loop, xDnsQuery query);

/**
 * @brief Free a DNS resolution result.
 *
 * Releases all memory associated with the result, including the address
 * list. Passing NULL is safe (no-op).
 *
 * @param result  Result to free, or NULL.
 */
XCAPI(void) xDnsResultFree(xDnsResult *result);

#endif /* XNET_DNS_H */
