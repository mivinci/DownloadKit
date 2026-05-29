/*
 * Copyright 2025 The libx++ Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * tcp.cpp - Async TCP listener and stream implementation.
 */

#include <xpp/net/tcp.h>

#include <xpp/net/dns.h>

#include <cerrno>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

extern "C" {
#include <x/net/transport_private.h>
}

namespace xpp {
namespace net {

/* ── Helpers ───────────────────────────────────────────────────────── */

namespace {

/// Set non-blocking + CLOEXEC on an already-open fd (e.g. from accept).
/// Returns 0 on success, -1 on failure.
int set_nonblocking(int fd) {
  int flags = fcntl(fd, F_GETFL, 0);
  if (flags < 0) return -1;
  if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) return -1;
  int fdflags = fcntl(fd, F_GETFD, 0);
  if (fdflags < 0) return -1;
  return fcntl(fd, F_SETFD, fdflags | FD_CLOEXEC);
}

/// Recursively try to connect to each address in @p addrs starting at
/// @p idx, returning a Promise that resolves to the first successful
/// TcpStream or Err(ConnectFailed) once all addresses are exhausted.
///
/// The Vec is wrapped in an Arc so each .then() lambda can extend its
/// lifetime by copy — we can't move-capture into a recursive Promise
/// chain.  Arc is preferred over std::shared_ptr to keep this codebase
/// in the Rust-style refcount family.
Promise<Result<TcpStream, SocketError>> connect_each(Arc<Vec<SocketAddr>> addrs, size_t idx) {
  if (idx >= addrs->len()) {
    return Promise<Result<TcpStream, SocketError>>::resolve(
      Result<TcpStream, SocketError>(err, SocketError::ConnectFailed));
  }
  return TcpStream::connect_addr((*addrs)[idx])
    .then([addrs, idx](Result<TcpStream, SocketError> r)
            -> Promise<Result<TcpStream, SocketError>> {
      if (r.is_ok()) {
        return Promise<Result<TcpStream, SocketError>>::resolve(std::move(r));
      }
      return connect_each(addrs, idx + 1);
    });
}

} // anonymous namespace

/* ── TcpListener ───────────────────────────────────────────────────── */

TcpListener::TcpListener(int fd, Arc<ScheduledIo> sio) : m_fd(fd), m_sio(std::move(sio)) {}

TcpListener::TcpListener(TcpListener &&o) noexcept : m_fd(o.m_fd), m_sio(std::move(o.m_sio)) {
  o.m_fd = -1;
}

TcpListener &TcpListener::operator=(TcpListener &&o) noexcept {
  if (this != &o) {
    close();
    m_fd   = o.m_fd;
    m_sio  = std::move(o.m_sio);
    o.m_fd = -1;
  }
  return *this;
}

TcpListener::~TcpListener() {
  close();
}

Result<TcpListener, SocketError> TcpListener::bind_addr(const SocketAddr &addr, int backlog) {
  int family = addr.is_ipv4() ? AF_INET : AF_INET6;
  int fd     = create_socket(family, SOCK_STREAM);
  if (fd < 0) return Result<TcpListener, SocketError>(err, SocketError::CreateFailed);

  int opt = 1;
  setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

  struct sockaddr_storage ss;
  socklen_t               len;
  addr.to_sockaddr(&ss, &len);

  if (::bind(fd, reinterpret_cast<struct sockaddr *>(&ss), len) < 0) {
    ::close(fd);
    return Result<TcpListener, SocketError>(err, SocketError::BindFailed);
  }

  if (::listen(fd, backlog) < 0) {
    ::close(fd);
    return Result<TcpListener, SocketError>(err, SocketError::BindFailed);
  }

  auto sio = Arc<ScheduledIo>::make(fd);
  return Result<TcpListener, SocketError>(ok, TcpListener(fd, sio));
}

Promise<Result<TcpListener, SocketError>> TcpListener::bind(String addr, int backlog) {
  return lookup_host(std::move(addr))
    .then([backlog](Result<Vec<SocketAddr>, SocketError> r) -> Result<TcpListener, SocketError> {
      if (r.is_err()) {
        return Result<TcpListener, SocketError>(err, std::move(r).unwrap_err());
      }
      Vec<SocketAddr> addrs = std::move(r).unwrap();
      // Try each resolved address in order; first successful bind wins.
      for (size_t i = 0; i < addrs.len(); ++i) {
        auto out = TcpListener::bind_addr(addrs[i], backlog);
        if (out.is_ok()) return out;
      }
      return Result<TcpListener, SocketError>(err, SocketError::BindFailed);
    });
}

Promise<Result<TcpStream, SocketError>> TcpListener::accept() {
  if (m_fd < 0) {
    return Promise<Result<TcpStream, SocketError>>::resolve(
      Result<TcpStream, SocketError>(err, SocketError::Closed));
  }

  int                     fd = m_fd;
  struct sockaddr_storage addr;
  socklen_t               len = sizeof(addr);

  int client_fd = ::accept(fd, reinterpret_cast<struct sockaddr *>(&addr), &len);
  if (client_fd >= 0) {
    if (set_nonblocking(client_fd) < 0) {
      ::close(client_fd);
      return Promise<Result<TcpStream, SocketError>>::resolve(
        Result<TcpStream, SocketError>(err, SocketError::AcceptFailed));
    }
    auto peer = SocketAddr::from_sockaddr(reinterpret_cast<struct sockaddr *>(&addr), len);
    return Promise<Result<TcpStream, SocketError>>::resolve(
      Result<TcpStream, SocketError>(
        ok, TcpStream::from_fd(client_fd, peer.unwrap_or(SocketAddr::unspecified()))));
  }
  if (errno != EAGAIN && errno != EWOULDBLOCK) {
    return Promise<Result<TcpStream, SocketError>>::resolve(
      Result<TcpStream, SocketError>(err, SocketError::AcceptFailed));
  }

  auto sio = m_sio;
  return _::readable(m_sio).then([fd, sio]() -> Result<TcpStream, SocketError> {
    struct sockaddr_storage addr2;
    socklen_t               len2 = sizeof(addr2);
    int client_fd                = ::accept(fd, reinterpret_cast<struct sockaddr *>(&addr2), &len2);
    if (client_fd < 0) {
      return Result<TcpStream, SocketError>(err, SocketError::AcceptFailed);
    }
    if (set_nonblocking(client_fd) < 0) {
      ::close(client_fd);
      return Result<TcpStream, SocketError>(err, SocketError::AcceptFailed);
    }
    auto peer = SocketAddr::from_sockaddr(reinterpret_cast<struct sockaddr *>(&addr2), len2);
    return Result<TcpStream, SocketError>(
      ok, TcpStream::from_fd(client_fd, peer.unwrap_or(SocketAddr::unspecified())));
  });
}

SocketAddr TcpListener::local_addr() const {
  struct sockaddr_storage ss;
  socklen_t               len = sizeof(ss);
  ::getsockname(m_fd, reinterpret_cast<struct sockaddr *>(&ss), &len);
  return SocketAddr::from_sockaddr(reinterpret_cast<struct sockaddr *>(&ss), len)
    .unwrap_or(SocketAddr::unspecified());
}

void TcpListener::close() {
  if (m_fd >= 0) {
    m_sio->close_fd();
    m_fd = -1;
  }
}

/* ── TcpStream ─────────────────────────────────────────────────────── */

TcpStream::TcpStream(PollEvented *io, xTransport transport, SocketAddr peer)
    : m_io(io), m_transport(transport), m_peer(std::move(peer)) {}

TcpStream::TcpStream(TcpStream &&o) noexcept
    : m_io(o.m_io), m_transport(o.m_transport), m_peer(std::move(o.m_peer)) {
  o.m_io        = nullptr;
  o.m_transport = {};
}

TcpStream &TcpStream::operator=(TcpStream &&o) noexcept {
  if (this != &o) {
    close();
    m_io          = o.m_io;
    m_transport   = o.m_transport;
    m_peer        = std::move(o.m_peer);
    o.m_io        = nullptr;
    o.m_transport = {};
  }
  return *this;
}

TcpStream::~TcpStream() {
  close();
}

TcpStream TcpStream::from_fd(int fd, const SocketAddr &peer) {
  // Caller guarantees fd is a valid open socket (success path of
  // accept(), or our connect() success path).
  xTransport transport = {};
  xTransportPlainInit(&transport, fd);
  return TcpStream(new PollEvented(fd), transport, peer);
}

Promise<Result<TcpStream, SocketError>> TcpStream::connect_addr(const SocketAddr &addr) {
  int family = addr.is_ipv4() ? AF_INET : AF_INET6;
  int fd     = create_socket(family, SOCK_STREAM);
  if (fd < 0) {
    return Promise<Result<TcpStream, SocketError>>::resolve(
      Result<TcpStream, SocketError>(err, SocketError::CreateFailed));
  }

  struct sockaddr_storage ss;
  socklen_t               len;
  addr.to_sockaddr(&ss, &len);

  int rc = ::connect(fd, reinterpret_cast<struct sockaddr *>(&ss), len);
  if (rc == 0) {
    // Connected immediately
    return Promise<Result<TcpStream, SocketError>>::resolve(
      Result<TcpStream, SocketError>(ok, from_fd(fd, addr)));
  }
  if (errno != EINPROGRESS) {
    ::close(fd);
    return Promise<Result<TcpStream, SocketError>>::resolve(
      Result<TcpStream, SocketError>(err, SocketError::ConnectFailed));
  }

  // Wait for writable = connect complete.
  auto *io   = new PollEvented(fd);
  auto  peer = addr;
  auto  sio  = io->scheduled_io();
  return _::writable(sio).then([io, peer]() -> Result<TcpStream, SocketError> {
    int       fd   = io->fd();
    int       sk_err = 0;
    socklen_t elen = sizeof(sk_err);
    getsockopt(fd, SOL_SOCKET, SO_ERROR, &sk_err, &elen);
    if (sk_err != 0) {
      delete io;
      return Result<TcpStream, SocketError>(err, SocketError::ConnectFailed);
    }
    // Transfer ownership of the PollEvented into the TcpStream
    xTransport transport = {};
    xTransportPlainInit(&transport, fd);
    return Result<TcpStream, SocketError>(ok, TcpStream(io, transport, peer));
  });
}

Promise<Result<TcpStream, SocketError>> TcpStream::connect(String addr) {
  return lookup_host(std::move(addr))
    .then([](Result<Vec<SocketAddr>, SocketError> r) -> Promise<Result<TcpStream, SocketError>> {
      if (r.is_err()) {
        return Promise<Result<TcpStream, SocketError>>::resolve(
          Result<TcpStream, SocketError>(err, std::move(r).unwrap_err()));
      }
      auto addrs = Arc<Vec<SocketAddr>>::make(std::move(r).unwrap());
      return connect_each(std::move(addrs), 0);
    });
}

Promise<ssize_t> TcpStream::read(Span<char> buf) {
  auto t = m_transport;
  return m_io->async_read_op(
    [t](int fd, void *data, size_t len) -> ssize_t {
      (void)fd;
      return t.read(t.ctx, data, len);
    },
    buf.data(), buf.size());
}

Promise<ssize_t> TcpStream::write(Span<const char> buf) {
  // iovec::iov_base is void* (POSIX); C++ const_cast is needed because C
  // sockets do not have const-qualified iov_base.  The kernel only reads
  // from this buffer so the cast is safe.
  auto t   = m_transport;
  auto iov = iovec{const_cast<char *>(buf.data()), buf.size()};
  return m_io->async_write_op(
    [t](int fd, struct iovec iov2, int iovcnt) -> ssize_t {
      (void)fd;
      return t.writev(t.ctx, &iov2, iovcnt);
    },
    iov, 1);
}

ssize_t TcpStream::try_read(Span<char> buf) {
  return m_transport.read(m_transport.ctx, buf.data(), buf.size());
}

ssize_t TcpStream::try_write(Span<const char> buf) {
  struct iovec iov = {const_cast<char *>(buf.data()), buf.size()};
  return m_transport.writev(m_transport.ctx, &iov, 1);
}

Promise<void> TcpStream::readable() {
  return m_io->readable();
}

Promise<void> TcpStream::writable() {
  return m_io->writable();
}

SocketAddr TcpStream::local_addr() const {
  struct sockaddr_storage ss;
  socklen_t               len = sizeof(ss);
  ::getsockname(m_io->fd(), reinterpret_cast<struct sockaddr *>(&ss), &len);
  return SocketAddr::from_sockaddr(reinterpret_cast<struct sockaddr *>(&ss), len)
    .unwrap_or(SocketAddr::unspecified());
}

SocketAddr TcpStream::peer_addr() const {
  return m_peer;
}

void TcpStream::shutdown(Shutdown how) {
  if (m_io && !m_io->is_closed()) {
    int how_flag = (how == Shutdown::Read)   ? SHUT_RD
                   : (how == Shutdown::Both) ? SHUT_RDWR
                                             : SHUT_WR;
    ::shutdown(m_io->fd(), how_flag);
  }
}

void TcpStream::close() {
  if (m_transport.destroy) m_transport.destroy(m_transport.ctx);
  delete m_io;
  m_io        = nullptr;
  m_transport = {};
}

/* ── Socket options ────────────────────────────────────────────────── */

Result<void, SocketError> TcpStream::set_nodelay(bool on) {
  if (m_io == nullptr) return Result<void, SocketError>(err, SocketError::Closed);
  int flag = on ? 1 : 0;
  if (setsockopt(m_io->fd(), IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag)) < 0)
    return Result<void, SocketError>(err, SocketError::CreateFailed);
  return Result<void, SocketError>(ok);
}

Result<bool, SocketError> TcpStream::nodelay() const {
  if (m_io == nullptr) return Result<bool, SocketError>(err, SocketError::Closed);
  int       flag = 0;
  socklen_t len  = sizeof(flag);
  if (getsockopt(m_io->fd(), IPPROTO_TCP, TCP_NODELAY, &flag, &len) < 0)
    return Result<bool, SocketError>(err, SocketError::CreateFailed);
  return Result<bool, SocketError>(ok, flag != 0);
}

} // namespace net
} // namespace xpp
