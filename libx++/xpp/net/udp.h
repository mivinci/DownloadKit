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

#include <xpp/arc.h>
#include <xpp/net/addr.h>
#include <xpp/promise.h>
#include <xpp/result.h>
#include <xpp/scheduled_io.h>
#include <xpp/span.h>

#include <cstdint>
#include <sys/types.h>

namespace xpp {
namespace net {

/* ── SocketError ───────────────────────────────────────────────────── */

enum class SocketError : uint8_t {
  CreateFailed,
  BindFailed,
  ConnectFailed,
  AddrFamilyMismatch,
};

const char *socket_error_message(SocketError e) noexcept;

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
   * Use port 0 to let the OS pick an ephemeral port.
   */
  static Result<UdpSocket, SocketError> bind(const SocketAddr &addr);

  /** @brief Associate with a single remote address for send/recv. */
  Result<void, SocketError> connect(const SocketAddr &addr);

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
  int fd() const { return m_fd; }

  /** @brief True after close(). */
  bool is_closed() const { return m_fd < 0; }

  /** @brief Close the socket. Wakes any pending I/O. */
  void close();

private:
  UdpSocket(int fd, Arc<ScheduledIo> sio);

  int                m_fd;
  Arc<ScheduledIo> m_sio;
};

} // namespace net
} // namespace xpp

#endif // XPP_NET_UDP_H
