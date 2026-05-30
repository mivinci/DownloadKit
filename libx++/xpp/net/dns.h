/*
 * Copyright 2025 The libx++ Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * dns.h - Async DNS resolution.
 *
 * Wraps getaddrinfo() in spawn_blocking — same approach as Tokio's
 * tokio::net::lookup_host (which also defers the blocking glibc call
 * to a blocking-pool thread).  No separate event-driven resolver is
 * needed: getaddrinfo is called from the runtime's blocking pool and
 * the result is delivered to the caller's worker thread as a Promise.
 *
 * C++11-compatible.
 */

#ifndef XPP_NET_DNS_H
#define XPP_NET_DNS_H

#include <xpp/net/addr.h>
#include <xpp/net/socket.h>
#include <xpp/promise.h>
#include <xpp/result.h>
#include <xpp/string.h>
#include <xpp/vec.h>

namespace xpp {
namespace net {

/**
 * @brief Resolve @p addr to a list of socket addresses.
 *
 * @p addr is a "host:port" string in the same format Tokio's
 * ToSocketAddrs accepts:
 *   - "127.0.0.1:80"      — IPv4 literal
 *   - "[::1]:80"          — IPv6 literal (brackets required)
 *   - "localhost:80"      — hostname (resolved via getaddrinfo)
 *   - "example.com:443"
 *
 * Calls getaddrinfo() on a blocking-pool thread (via spawn_blocking),
 * so async workers stay free to drive other tasks.  IPv4 and IPv6
 * results are both returned, in the order the resolver provided
 * (typically RFC 6724 ordering on glibc).
 *
 * @return Promise resolving to:
 *           - Ok(Vec<SocketAddr>)  on success (always non-empty)
 *           - Err(SocketError::ResolveFailed)  if the input is
 *             malformed OR getaddrinfo failed
 *           - Err(SocketError::NoAddress)      if zero usable addrs
 *
 * @note Caller must be inside a runtime context (block_on / worker / spawn).
 */
Promise<Result<Vec<SocketAddr>, SocketError>> lookup_host(String addr);

/// Convenience overload — accepts a const char*.
inline Promise<Result<Vec<SocketAddr>, SocketError>>
lookup_host(const char *addr) {
  return lookup_host(String(addr));
}

} // namespace net
} // namespace xpp

#endif // XPP_NET_DNS_H
