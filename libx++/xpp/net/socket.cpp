/*
 * Copyright 2025 The libx++ Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * socket.cpp - Shared socket primitives.
 */

#include <xpp/net/socket.h>

#include <cerrno>
#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>

namespace xpp {
namespace net {

int create_socket(int family, int type) {
  int fd;
#ifdef __linux__
  fd = ::socket(family, type | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
  if (fd >= 0) return fd;
  // Kernel might not support SOCK_CLOEXEC (very old); fall through.
#endif
  fd = ::socket(family, type, 0);
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
