/*
 * Copyright 2025 The libx++ Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * dns.cpp - Async DNS resolution via spawn_blocking.
 */

#include <xpp/net/dns.h>

#include <xpp/runtime.h>

#include <cstring>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>

namespace xpp {
namespace net {

namespace {

/// Split a "host:port" string into (host, port_string).  Handles:
///   - "1.2.3.4:80"        → ("1.2.3.4", "80")
///   - "[::1]:80"          → ("::1",     "80")
///   - "localhost:80"      → ("localhost", "80")
///   - "example.com:443"
/// Returns false on malformed input (no ':', empty halves, mismatched
/// brackets, etc.).
///
/// host_buf and port_buf must each be at least s.size() + 1 bytes.
bool split_host_port(const char *s, size_t n, char *host_buf, char *port_buf) {
  if (n == 0) return false;

  size_t host_len = 0;
  size_t port_off = 0;

  if (s[0] == '[') {
    // IPv6 literal: find matching ']' then ':'.
    const char *rb = static_cast<const char *>(std::memchr(s, ']', n));
    if (!rb) return false;
    if (rb - s < 2) return false; // "[]:..." is not valid
    host_len = static_cast<size_t>(rb - s) - 1;
    std::memcpy(host_buf, s + 1, host_len);
    host_buf[host_len] = '\0';
    size_t after_rb    = static_cast<size_t>(rb - s) + 1;
    if (after_rb >= n || s[after_rb] != ':') return false;
    port_off = after_rb + 1;
  } else {
    // IPv4 literal or hostname: the LAST ':' separates the port.  We
    // cannot use the first ':' because IPv6 zone IDs / future syntaxes
    // could contain ':' too, but for IPv4/hostname there is at most
    // one ':' so first==last.
    const char *colon = nullptr;
    for (size_t i = n; i > 0; --i) {
      if (s[i - 1] == ':') {
        colon = s + (i - 1);
        break;
      }
    }
    if (!colon) return false;
    host_len = static_cast<size_t>(colon - s);
    if (host_len == 0) return false;
    std::memcpy(host_buf, s, host_len);
    host_buf[host_len] = '\0';
    port_off           = host_len + 1;
  }

  if (port_off >= n) return false; // empty port
  size_t port_len = n - port_off;
  std::memcpy(port_buf, s + port_off, port_len);
  port_buf[port_len] = '\0';
  return true;
}

} // anonymous namespace

Promise<Result<Vec<SocketAddr>, io::Error>> lookup_host(String addr) {
  // The blocking lambda captures `addr` by move.  Parsing happens on
  // the blocking-pool thread to keep the worker thread off the hook
  // for even the tiny strncmp/memchr cost.
  return spawn_blocking([addr = std::move(addr)]() -> Result<Vec<SocketAddr>, io::Error> {
    // NI_MAXHOST (1025) covers any RFC-compliant DNS name (max 253) and
    // common platform service-style hostnames.  Port string fits in 8
    // chars (max 5 digits + NUL).
    char host_buf[NI_MAXHOST];
    char port_buf[8];
    if (addr.len() >= sizeof(host_buf)) {
      return Result<Vec<SocketAddr>, io::Error>(err, io::ErrorKind::ResolveFailed);
    }
    if (!split_host_port(addr.c_str(), addr.len(), host_buf, port_buf)) {
      return Result<Vec<SocketAddr>, io::Error>(err, io::ErrorKind::ResolveFailed);
    }

    struct addrinfo hints;
    std::memset(&hints, 0, sizeof(hints));
    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    struct addrinfo *res = nullptr;
    int              rc  = ::getaddrinfo(host_buf, port_buf, &hints, &res);
    if (rc != 0) {
      if (res) ::freeaddrinfo(res);
      return Result<Vec<SocketAddr>, io::Error>(err, io::ErrorKind::ResolveFailed);
    }

    auto out = Vec<SocketAddr>::with_capacity(4);
    for (struct addrinfo *p = res; p != nullptr; p = p->ai_next) {
      auto opt = SocketAddr::from_sockaddr(p->ai_addr, p->ai_addrlen);
      if (opt.is_some()) out.push(opt.unwrap());
    }
    ::freeaddrinfo(res);

    if (out.is_empty()) return Result<Vec<SocketAddr>, io::Error>(err, io::ErrorKind::NoAddress);
    return Result<Vec<SocketAddr>, io::Error>(ok, std::move(out));
  });
}

} // namespace net
} // namespace xpp
