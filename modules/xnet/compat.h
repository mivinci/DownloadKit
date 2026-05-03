/*
 * Copyright 2025 The xKit Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * compat.h - Platform abstraction for xnet (Windows / POSIX)
 */

#ifndef XNET_COMPAT_H
#define XNET_COMPAT_H

#include <xbase/uio.h>

/* ═══════════════════════════════════════════════════════════════════
 *  System headers
 * ═══════════════════════════════════════════════════════════════════ */

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

/* ═══════════════════════════════════════════════════════════════════
 *  close() / closesocket()
 * ═══════════════════════════════════════════════════════════════════ */

#ifdef _WIN32
#define xnet_close(fd) closesocket(fd)
#else
#define xnet_close(fd) close(fd)
#endif

/* ═══════════════════════════════════════════════════════════════════
 *  read() / write() → recv() / send()
 * ═══════════════════════════════════════════════════════════════════ */

#ifdef _WIN32
#define xnet_read(fd, buf, len)  recv(fd, buf, (int)(len), 0)
#define xnet_write(fd, buf, len) send(fd, buf, (int)(len), 0)
#else
#define xnet_read(fd, buf, len)  read(fd, buf, len)
#define xnet_write(fd, buf, len) write(fd, buf, len)
#endif

/* ═══════════════════════════════════════════════════════════════════
 *  Error codes
 * ═══════════════════════════════════════════════════════════════════ */

#ifdef _WIN32
#define xnet_errno()     WSAGetLastError()
#define XNET_EAGAIN      WSAEWOULDBLOCK
#define XNET_EWOULDBLOCK WSAEWOULDBLOCK
#define XNET_EINPROGRESS WSAEWOULDBLOCK
#define XNET_EMFILE      WSAEMFILE
#define XNET_ENFILE      WSAEMFILE /* Windows has no ENFILE; map to EMFILE */
#define XNET_EINTR       WSAEINTR
#else
#define xnet_errno()     errno
#define XNET_EAGAIN      EAGAIN
#define XNET_EWOULDBLOCK EWOULDBLOCK
#define XNET_EINPROGRESS EINPROGRESS
#define XNET_EMFILE      EMFILE
#define XNET_ENFILE      ENFILE
#define XNET_EINTR       EINTR
#endif

/* ═══════════════════════════════════════════════════════════════════
 *  iovec / writev / readv — use xbase/uio.h types
 * ═══════════════════════════════════════════════════════════════════ */

typedef xiovec xnet_iovec;
#define xnet_writev(fd, iov, iovcnt) xwritev(fd, iov, iovcnt)

/* ═══════════════════════════════════════════════════════════════════
 *  setsockopt optval type
 * ═══════════════════════════════════════════════════════════════════ */

#ifdef _WIN32
#define xnet_setsockopt(fd, level, optname, optval, optlen) \
  setsockopt(fd, level, optname, (const char *)(optval), optlen)
#define xnet_getsockopt(fd, level, optname, optval, optlen) \
  getsockopt(fd, level, optname, (char *)(optval), optlen)
#else
#define xnet_setsockopt(fd, level, optname, optval, optlen) \
  setsockopt(fd, level, optname, optval, optlen)
#define xnet_getsockopt(fd, level, optname, optval, optlen) \
  getsockopt(fd, level, optname, optval, optlen)
#endif

/* ═══════════════════════════════════════════════════════════════════
 *  Winsock initialization
 * ═══════════════════════════════════════════════════════════════════ */

#ifdef _WIN32
/**
 * Initialize Winsock. Safe to call multiple times; returns 0 on success.
 * Call once at program startup before any socket operations.
 */
static inline int xnet_init(void) {
  static int initialized = 0;
  if (!initialized) {
    WSADATA d;
    if (WSAStartup(MAKEWORD(2, 2), &d) != 0) return -1;
    initialized = 1;
  }
  return 0;
}

static inline void xnet_cleanup(void) {
  WSACleanup();
}
#else
/* No-op on POSIX */
static inline int xnet_init(void) {
  return 0;
}
static inline void xnet_cleanup(void) {}
#endif

#endif /* XNET_COMPAT_H */
