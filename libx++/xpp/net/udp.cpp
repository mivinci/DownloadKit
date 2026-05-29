/*
 * Copyright 2025 The libx++ Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * udp.cpp - Async UDP socket implementation.
 */

#include <xpp/net/udp.h>

#include <cerrno>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

namespace xpp {
namespace net {

/* ── UdpSocket ─────────────────────────────────────────────────────── */

UdpSocket::UdpSocket(PollEvented *io) : m_io(io) {}

UdpSocket::UdpSocket(UdpSocket &&o) noexcept : m_io(o.m_io) {
  o.m_io = nullptr;
}

UdpSocket &UdpSocket::operator=(UdpSocket &&o) noexcept {
  if (this != &o) {
    close();
    m_io   = o.m_io;
    o.m_io = nullptr;
  }
  return *this;
}

UdpSocket::~UdpSocket() {
  close();
}

Result<UdpSocket, SocketError> UdpSocket::bind(const SocketAddr &addr) {
  int family = addr.is_ipv4() ? AF_INET : AF_INET6;
  int fd     = create_socket(family, SOCK_DGRAM);
  if (fd < 0) return Result<UdpSocket, SocketError>(err, SocketError::CreateFailed);

  struct sockaddr_storage ss;
  socklen_t               len;
  addr.to_sockaddr(&ss, &len);

  if (::bind(fd, reinterpret_cast<struct sockaddr *>(&ss), len) < 0) {
    ::close(fd);
    return Result<UdpSocket, SocketError>(err, SocketError::BindFailed);
  }

  return Result<UdpSocket, SocketError>(ok, UdpSocket(new PollEvented(fd)));
}

Result<void, SocketError> UdpSocket::connect(const SocketAddr &addr) {
  struct sockaddr_storage ss;
  socklen_t               len;
  addr.to_sockaddr(&ss, &len);

  if (::connect(m_io->fd(), reinterpret_cast<struct sockaddr *>(&ss), len) < 0)
    return Result<void, SocketError>(err, SocketError::ConnectFailed);
  return Result<void, SocketError>(ok);
}

SocketAddr UdpSocket::local_addr() const {
  struct sockaddr_storage ss;
  socklen_t               len = sizeof(ss);
  ::getsockname(m_io->fd(), reinterpret_cast<struct sockaddr *>(&ss), &len);
  return SocketAddr::from_sockaddr(reinterpret_cast<struct sockaddr *>(&ss), len)
    .unwrap_or(SocketAddr::from(SocketAddrV4::from(Ipv4Addr::UNSPECIFIED, 0)));
}

void UdpSocket::close() {
  delete m_io;
  m_io = nullptr;
}

/* ── One-to-many ───────────────────────────────────────────────────── */

Promise<ssize_t> UdpSocket::send_to(Span<const char> buf, const SocketAddr &addr) {
  struct sockaddr_storage ss;
  socklen_t               len;
  addr.to_sockaddr(&ss, &len);

  ssize_t n =
    ::sendto(m_io->fd(), buf.data(), buf.size(), 0, reinterpret_cast<struct sockaddr *>(&ss), len);
  if (n >= 0 || errno != EAGAIN) return Promise<ssize_t>::resolve(n);

  auto sio = m_io->scheduled_io();
  auto fd  = m_io->fd();
  return _::writable(sio).then([fd, buf, ss, len]() -> ssize_t {
    return ::sendto(fd, buf.data(), buf.size(), 0, reinterpret_cast<const struct sockaddr *>(&ss),
                    len);
  });
}

Promise<RecvFromResult> UdpSocket::recv_from(Span<char> buf) {
  struct sockaddr_storage from;
  socklen_t               from_len = sizeof(from);

  ssize_t n = ::recvfrom(m_io->fd(), buf.data(), buf.size(), 0,
                         reinterpret_cast<struct sockaddr *>(&from), &from_len);
  if (n >= 0) {
    auto a = SocketAddr::from_sockaddr(reinterpret_cast<struct sockaddr *>(&from), from_len);
    return Promise<RecvFromResult>::resolve(RecvFromResult{
      n, a.unwrap_or(SocketAddr::from(SocketAddrV4::from(Ipv4Addr::UNSPECIFIED, 0)))});
  }
  if (errno != EAGAIN)
    return Promise<RecvFromResult>::resolve(
      RecvFromResult{n, SocketAddr::from(SocketAddrV4::from(Ipv4Addr::UNSPECIFIED, 0))});

  auto fd  = m_io->fd();
  auto sio = m_io->scheduled_io();
  return _::readable(sio).then([fd, buf]() -> RecvFromResult {
    struct sockaddr_storage from2;
    socklen_t               len2 = sizeof(from2);
    ssize_t                 n2 =
      ::recvfrom(fd, buf.data(), buf.size(), 0, reinterpret_cast<struct sockaddr *>(&from2), &len2);
    SocketAddr a = SocketAddr::from_sockaddr(reinterpret_cast<struct sockaddr *>(&from2), len2)
                     .unwrap_or(SocketAddr::from(SocketAddrV4::from(Ipv4Addr::UNSPECIFIED, 0)));
    return RecvFromResult{n2, a};
  });
}

/* ── One-to-one ────────────────────────────────────────────────────── */

Promise<ssize_t> UdpSocket::send(Span<const char> buf) {
  return m_io->async_write_op(&::send, buf.data(), buf.size(), 0);
}

Promise<ssize_t> UdpSocket::recv(Span<char> buf) {
  return m_io->async_read_op(&::recv, buf.data(), buf.size(), 0);
}

} // namespace net
} // namespace xpp
