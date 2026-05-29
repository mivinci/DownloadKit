/*
 * Copyright 2025 The libx++ Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * socket.cpp - Shared socket type implementations.
 */

#include <xpp/net/socket.h>

#include <cerrno>
#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>

namespace xpp {
namespace net {

const char *socket_error_message(SocketError e) noexcept {
  switch (e) {
  case SocketError::CreateFailed:
    return "failed to create socket";
  case SocketError::BindFailed:
    return "failed to bind socket";
  case SocketError::ConnectFailed:
    return "failed to connect socket";
  case SocketError::AcceptFailed:
    return "failed to accept connection";
  case SocketError::AddrFamilyMismatch:
    return "address family mismatch";
  case SocketError::ResolveFailed:
    return "DNS resolution failed";
  case SocketError::NoAddress:
    return "host resolved to no usable address";
  case SocketError::Closed:
    return "operation attempted on a closed socket";
  default:
    return "unknown socket error";
  }
}

int create_socket(int family, int type) {
#ifdef __linux__
  int fd = ::socket(family, type | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
  if (fd >= 0) return fd;
  // Kernel might not support SOCK_CLOEXEC (very old); fall through.
#endif
  int fd = ::socket(family, type, 0);
  if (fd < 0) return -1;

  int flags = fcntl(fd, F_GETFL, 0);
  if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
  fail:
    int saved = errno;
    ::close(fd);
    errno = saved;
    return -1;
  }
  int fdflags = fcntl(fd, F_GETFD, 0);
  if (fdflags < 0 || fcntl(fd, F_SETFD, fdflags | FD_CLOEXEC) < 0) goto fail;
  return fd;
}

} // namespace net
} // namespace xpp
