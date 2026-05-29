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

/* ── SocketError ───────────────────────────────────────────────────── */

const char *socket_error_message(SocketError e) noexcept {
  switch (e) {
  case SocketError::CreateFailed:       return "failed to create socket";
  case SocketError::BindFailed:         return "failed to bind socket";
  case SocketError::ConnectFailed:      return "failed to connect socket";
  case SocketError::AddrFamilyMismatch: return "address family mismatch";
  default:                              return "unknown socket error";
  }
}

/* ── Helpers ───────────────────────────────────────────────────────── */

namespace {

int set_nonblocking(int fd) {
  int flags = fcntl(fd, F_GETFL, 0);
  if (flags < 0) return -1;
  return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

} // anonymous namespace

/* ── UdpSocket ─────────────────────────────────────────────────────── */

UdpSocket::UdpSocket(int fd, Arc<ScheduledIo> sio)
    : m_fd(fd), m_sio(std::move(sio)) {}

UdpSocket::UdpSocket(UdpSocket &&o) noexcept
    : m_fd(o.m_fd), m_sio(std::move(o.m_sio)) {
  o.m_fd = -1;
}

UdpSocket &UdpSocket::operator=(UdpSocket &&o) noexcept {
  if (this != &o) {
    close();
    m_fd    = o.m_fd;
    m_sio   = std::move(o.m_sio);
    o.m_fd  = -1;
  }
  return *this;
}

UdpSocket::~UdpSocket() {
  close();
}

Result<UdpSocket, SocketError> UdpSocket::bind(const SocketAddr &addr) {
  int family = addr.is_ipv4() ? AF_INET : AF_INET6;
  int fd     = ::socket(family, SOCK_DGRAM, 0);
  if (fd < 0) return Result<UdpSocket, SocketError>(err, SocketError::CreateFailed);

  if (set_nonblocking(fd) < 0) {
    ::close(fd);
    return Result<UdpSocket, SocketError>(err, SocketError::CreateFailed);
  }

  struct sockaddr_storage ss;
  socklen_t               len;
  addr.to_sockaddr(&ss, &len);

  if (::bind(fd, reinterpret_cast<struct sockaddr *>(&ss), len) < 0) {
    ::close(fd);
    return Result<UdpSocket, SocketError>(err, SocketError::BindFailed);
  }

  auto sio = Arc<ScheduledIo>::make(fd);
  return Result<UdpSocket, SocketError>(ok, UdpSocket(fd, sio));
}

Result<void, SocketError> UdpSocket::connect(const SocketAddr &addr) {
  struct sockaddr_storage ss;
  socklen_t               len;
  addr.to_sockaddr(&ss, &len);

  if (::connect(m_fd, reinterpret_cast<struct sockaddr *>(&ss), len) < 0)
    return Result<void, SocketError>(err, SocketError::ConnectFailed);
  return Result<void, SocketError>(ok);
}

SocketAddr UdpSocket::local_addr() const {
  struct sockaddr_storage ss;
  socklen_t               len = sizeof(ss);
  ::getsockname(m_fd, reinterpret_cast<struct sockaddr *>(&ss), &len);
  return SocketAddr::from_sockaddr(reinterpret_cast<struct sockaddr *>(&ss), len)
      .unwrap_or(SocketAddr::from(SocketAddrV4::from(Ipv4Addr::UNSPECIFIED, 0)));
}

void UdpSocket::close() {
  if (m_fd >= 0) {
    m_sio->close_fd();
    m_fd = -1;
  }
}

/* ── One-to-many ───────────────────────────────────────────────────── */

Promise<ssize_t> UdpSocket::send_to(Span<const char> buf,
                                     const SocketAddr &addr) {
  struct sockaddr_storage ss;
  socklen_t               len;
  addr.to_sockaddr(&ss, &len);

  ssize_t n = ::sendto(m_fd, buf.data(), buf.size(), 0,
                        reinterpret_cast<struct sockaddr *>(&ss), len);
  if (n >= 0 || errno != EAGAIN)
    return Promise<ssize_t>::resolve(n);

  int     fd  = m_fd;
  auto    sio = m_sio;
  return _::writable(m_sio).then([fd, buf, ss, len]() -> ssize_t {
    return ::sendto(fd, buf.data(), buf.size(), 0,
                    reinterpret_cast<const struct sockaddr *>(&ss), len);
  });
}

Promise<RecvFromResult> UdpSocket::recv_from(Span<char> buf) {
  struct sockaddr_storage from;
  socklen_t               from_len = sizeof(from);

  ssize_t n = ::recvfrom(m_fd, buf.data(), buf.size(), 0,
                          reinterpret_cast<struct sockaddr *>(&from), &from_len);
  if (n >= 0) {
    auto a = SocketAddr::from_sockaddr(reinterpret_cast<struct sockaddr *>(&from), from_len);
    return Promise<RecvFromResult>::resolve(
        RecvFromResult{n, a.unwrap_or(SocketAddr::from(SocketAddrV4::from(Ipv4Addr::UNSPECIFIED, 0)))});
  }
  if (errno != EAGAIN)
    return Promise<RecvFromResult>::resolve(
        RecvFromResult{n, SocketAddr::from(SocketAddrV4::from(Ipv4Addr::UNSPECIFIED, 0))});

  int  fd  = m_fd;
  auto sio = m_sio;
  return _::readable(m_sio).then([fd, buf]() -> RecvFromResult {
    struct sockaddr_storage from2;
    socklen_t               len2 = sizeof(from2);
    ssize_t n2 = ::recvfrom(fd, buf.data(), buf.size(), 0,
                             reinterpret_cast<struct sockaddr *>(&from2), &len2);
    SocketAddr a = SocketAddr::from_sockaddr(
        reinterpret_cast<struct sockaddr *>(&from2), len2)
        .unwrap_or(SocketAddr::from(SocketAddrV4::from(Ipv4Addr::UNSPECIFIED, 0)));
    return RecvFromResult{n2, a};
  });
}

/* ── One-to-one ────────────────────────────────────────────────────── */

Promise<ssize_t> UdpSocket::send(Span<const char> buf) {
  ssize_t n = ::send(m_fd, buf.data(), buf.size(), 0);
  if (n >= 0 || errno != EAGAIN)
    return Promise<ssize_t>::resolve(n);

  int  fd  = m_fd;
  auto sio = m_sio;
  return _::writable(m_sio).then([fd, buf]() -> ssize_t {
    return ::send(fd, buf.data(), buf.size(), 0);
  });
}

Promise<ssize_t> UdpSocket::recv(Span<char> buf) {
  ssize_t n = ::recv(m_fd, buf.data(), buf.size(), 0);
  if (n >= 0 || errno != EAGAIN)
    return Promise<ssize_t>::resolve(n);

  int  fd  = m_fd;
  auto sio = m_sio;
  return _::readable(m_sio).then([fd, buf]() -> ssize_t {
    return ::recv(fd, buf.data(), buf.size(), 0);
  });
}

} // namespace net
} // namespace xpp
