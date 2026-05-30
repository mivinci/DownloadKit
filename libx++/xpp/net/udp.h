/*
 * Copyright 2025 The libx++ Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * udp.h - Async UDP socket.
 *
 * Mirrors Tokio's tokio::net::UdpSocket. Two usage modes:
 *   - One-to-many: bind + send_to / recv_from
 *   - One-to-one:  connect + send / recv
 *
 * All I/O methods return Promises.  Non-blocking fd + ScheduledIo
 * readiness + .then() retry pattern (no custom PromiseNode).
 *
 * C++11-compatible.
 */

#ifndef XPP_NET_UDP_H
#define XPP_NET_UDP_H

#include <xpp/io/error.h>
#include <xpp/io/poll_evented.h>
#include <xpp/net/addr.h>
#include <xpp/net/socket.h>
#include <xpp/promise.h>
#include <xpp/result.h>
#include <xpp/span.h>
#include <xpp/string.h>

#include <sys/types.h>

namespace xpp {
namespace net {

/* ── RecvFromResult ────────────────────────────────────────────────── */

struct RecvFromResult {
  ssize_t    n;    // bytes received, or -1 on error
  SocketAddr addr; // sender address
};

/* ── UdpSocket ─────────────────────────────────────────────────────── */

class UdpSocket {
public:
  UdpSocket(UdpSocket &&o) noexcept;
  UdpSocket &operator=(UdpSocket &&o) noexcept;

  UdpSocket(const UdpSocket &)            = delete;
  UdpSocket &operator=(const UdpSocket &) = delete;

  ~UdpSocket();

  /**
   * @brief Create a UDP socket bound to @p addr.
   *
   * @p addr is a "host:port" string in the same format Tokio's
   * ToSocketAddrs accepts:
   *   - "0.0.0.0:8080"   — IPv4 literal
   *   - "[::]:8080"      — IPv6 literal (brackets required)
   *   - "localhost:0"    — hostname (resolved via DNS)
   * Use port 0 to let the OS pick an ephemeral port.
   *
   * Resolves via lookup_host and tries each resolved address in order
   * until one binds successfully.  Mirrors Tokio's UdpSocket::bind.
   */
  static Promise<Result<UdpSocket, io::Error>> bind(String addr);
  static Promise<Result<UdpSocket, io::Error>> bind(const char *addr) {
    return bind(String(addr));
  }

  /**
   * @brief Create a UDP socket bound to a pre-parsed @p addr.
   *
   * Synchronous fast path — no DNS, no async hop.  Use this when you
   * already hold a SocketAddr.
   */
  static Result<UdpSocket, io::Error> bind_addr(const SocketAddr &addr);

  /**
   * @brief Associate with a single remote peer for send/recv.
   *
   * @p addr is a "host:port" string (see bind() above for format).
   */
  Promise<Result<void, io::Error>> connect(String addr);
  Promise<Result<void, io::Error>> connect(const char *addr) {
    return connect(String(addr));
  }

  /** @brief Associate with a pre-parsed peer address (synchronous). */
  Result<void, io::Error> connect_addr(const SocketAddr &addr);

  /* ── One-to-many (unconnected) ─────────────────────────────────── */

  /** @brief Send a datagram to @p addr. Returns bytes sent. */
  Promise<ssize_t> send_to(Span<const char> buf, const SocketAddr &addr);

  /** @brief Receive a datagram. Returns bytes received + sender address. */
  Promise<RecvFromResult> recv_from(Span<char> buf);

  /* ── One-to-one (connected) ────────────────────────────────────── */

  /** @brief Send to the connected peer. Returns bytes sent. */
  Promise<ssize_t> send(Span<const char> buf);

  /** @brief Receive from the connected peer. Returns bytes received. */
  Promise<ssize_t> recv(Span<char> buf);

  /* ── Accessors ─────────────────────────────────────────────────── */

  /** @brief The locally bound address. */
  SocketAddr local_addr() const;

  /** @brief The underlying file descriptor. */
  int fd() const {
    return m_io ? m_io->fd() : -1;
  }

  /** @brief True after close(). */
  bool is_closed() const {
    return m_io == nullptr;
  }

  /** @brief Close the socket. Wakes any pending I/O. */
  void close();

private:
  UdpSocket(PollEvented *io);

  PollEvented *m_io; // owned, nullable (null after close)
};

} // namespace net
} // namespace xpp

#endif // XPP_NET_UDP_H
