/*
 * Copyright 2025 The libx++ Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * tcp.h - Async TCP listener and stream.
 *
 * Uses raw fd + ScheduledIo for readiness, xTransport for I/O
 * (plain TCP today, TLS later via upgrade).  Mirrors Tokio's
 * TcpListener / TcpStream API shape.
 *
 * C++11-compatible.
 */

#ifndef XPP_NET_TCP_H
#define XPP_NET_TCP_H

#include <xpp/arc.h>
#include <xpp/io/poll_evented.h>
#include <xpp/net/addr.h>
#include <xpp/net/socket.h>
#include <xpp/own.h>
#include <xpp/promise.h>
#include <xpp/result.h>
#include <xpp/span.h>
#include <xpp/string.h>

extern "C" {
#include <x/net/transport.h>
}

#include <sys/types.h>

namespace xpp {
namespace net {

class TcpStream;

/* ── TcpListener ───────────────────────────────────────────────────── */

class TcpListener {
public:
  TcpListener(TcpListener &&o) noexcept;
  TcpListener &operator=(TcpListener &&o) noexcept;

  TcpListener(const TcpListener &)            = delete;
  TcpListener &operator=(const TcpListener &) = delete;

  ~TcpListener();

  /**
   * @brief Bind a listening socket on @p addr.
   *
   * @p addr is a "host:port" string in the same format Tokio's
   * ToSocketAddrs accepts:
   *   - "0.0.0.0:8080"   — IPv4 literal
   *   - "[::]:8080"      — IPv6 literal (brackets required)
   *   - "localhost:0"    — hostname (resolved via DNS)
   * Use port 0 for an ephemeral port.  backlog defaults to 128.
   *
   * Resolves the host via lookup_host and tries each resolved address
   * in order until one binds successfully.  Mirrors Tokio's
   * TcpListener::bind.
   */
  static Promise<Result<TcpListener, SocketError>> bind(String addr, int backlog = 128);
  static Promise<Result<TcpListener, SocketError>> bind(const char *addr, int backlog = 128) {
    return bind(String(addr), backlog);
  }

  /**
   * @brief Bind a listening socket on a pre-parsed @p addr.
   *
   * Synchronous fast path — no DNS, no async hop.  Use this when you
   * already hold a SocketAddr (e.g. from accept()).
   */
  static Result<TcpListener, SocketError> bind_addr(const SocketAddr &addr, int backlog = 128);

  /**
   * @brief Accept a new connection (async).
   *
   * Mirrors Tokio's TcpListener::accept(), using readiness-driven
   * retry with EAGAIN handling.
   *
   * @return Promise resolving to:
   *           - Ok(TcpStream) on success
   *           - Err(SocketError::AcceptFailed)
   *           - Err(SocketError::Closed) if the listener has been closed
   */
  Promise<Result<TcpStream, SocketError>> accept();

  /** @brief The locally bound address. */
  SocketAddr local_addr() const;

  void close();
  bool is_closed() const {
    return m_fd < 0;
  }
  int fd() const {
    return m_fd;
  }

private:
  TcpListener(int fd, Arc<ScheduledIo> sio);

  int              m_fd;
  Arc<ScheduledIo> m_sio;
};

/* ── TcpStream ─────────────────────────────────────────────────────── */

class TcpStream {
public:
  TcpStream(TcpStream &&o) noexcept;
  TcpStream &operator=(TcpStream &&o) noexcept;

  TcpStream(const TcpStream &)            = delete;
  TcpStream &operator=(const TcpStream &) = delete;

  ~TcpStream();

  /**
   * @brief Connect to @p addr (async — DNS + TCP handshake).
   *
   * @p addr is a "host:port" string ("127.0.0.1:80", "[::1]:80",
   * "example.com:443").  Resolves the host via lookup_host and
   * attempts connect() on each resolved address in order until one
   * succeeds.  Mirrors Tokio's TcpStream::connect.
   *
   * @return Promise resolving to:
   *           - Ok(TcpStream) on success
   *           - Err(SocketError::ResolveFailed) if DNS failed
   *           - Err(SocketError::NoAddress)     if zero usable addrs
   *           - Err(SocketError::ConnectFailed) if all addrs failed
   */
  static Promise<Result<TcpStream, SocketError>> connect(String addr);
  static Promise<Result<TcpStream, SocketError>> connect(const char *addr) {
    return connect(String(addr));
  }

  /**
   * @brief Connect to a pre-parsed @p addr.
   *
   * Async TCP handshake but no DNS.  Use this when you already hold a
   * SocketAddr.
   *
   * @return Promise resolving to:
   *           - Ok(TcpStream) on success
   *           - Err(SocketError::CreateFailed) if socket() failed
   *           - Err(SocketError::ConnectFailed) if connect failed
   */
  static Promise<Result<TcpStream, SocketError>> connect_addr(const SocketAddr &addr);

  /**
   * @brief Async read. Returns bytes read, 0 on EOF, -1 on error.
   * @warning Caller MUST keep @p buf alive until the returned Promise
   *          resolves.  Span is non-owning; the buffer is not captured
   *          by copy.  Mirrors Tokio's requirement that buffers outlive
   *          the I/O future.
   */
  Promise<ssize_t> read(Span<char> buf);

  /**
   * @brief Async write. Returns bytes written, -1 on error.
   * @warning Caller MUST keep @p buf alive until the returned Promise
   *          resolves.  Span is non-owning; the buffer is not captured
   *          by copy.  Mirrors Tokio's requirement that buffers outlive
   *          the I/O future.
   */
  Promise<ssize_t> write(Span<const char> buf);

  /** @brief Non-blocking read. Caller must ensure readability first. */
  ssize_t try_read(Span<char> buf);

  /** @brief Non-blocking write. Caller must ensure writability first. */
  ssize_t try_write(Span<const char> buf);

  /** @brief Wait until the socket is readable. */
  Promise<void> readable();

  /** @brief Wait until the socket is writable. */
  Promise<void> writable();

  SocketAddr local_addr() const;
  SocketAddr peer_addr() const;

  /** @brief Shut down one or both halves of the connection. */
  void shutdown(Shutdown how = Shutdown::Write);

  void close();
  bool is_closed() const {
    return m_io == nullptr;
  }
  int fd() const {
    return m_io ? m_io->fd() : -1;
  }

  /* ── Socket options ──────────────────────────────────────────────── */

  /** @brief Disable/enable Nagle's algorithm.  Defaults to OS default (off). */
  Result<void, SocketError> set_nodelay(bool on);
  Result<bool, SocketError> nodelay() const;

private:
  /** @brief Construct a TcpStream from a raw fd (internal factory). */
  static TcpStream from_fd(int fd, const SocketAddr &peer);

  TcpStream(PollEvented *io, xTransport transport, SocketAddr peer);

  friend class TcpListener;

  PollEvented *m_io; // owned, nullable (null when stream is empty/bad)
  xTransport   m_transport;
  SocketAddr   m_peer;
};

} // namespace net
} // namespace xpp

#endif // XPP_NET_TCP_H
