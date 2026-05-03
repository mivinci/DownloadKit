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
#define NOMINMAX
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
 *  Socket type and invalid socket constant
 * ═══════════════════════════════════════════════════════════════════ */

#ifdef _WIN32
typedef SOCKET xnet_socket_t;
#define XNET_INVALID_SOCKET INVALID_SOCKET
#else
typedef int xnet_socket_t;
#define XNET_INVALID_SOCKET (-1)
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
 *  Type aliases (socklen_t, ssize_t)
 * ═══════════════════════════════════════════════════════════════════ */

#ifdef _WIN32
typedef int     xnet_socklen_t;
typedef SSIZE_T xnet_ssize_t;
#else
typedef socklen_t xnet_socklen_t;
typedef ssize_t   xnet_ssize_t;
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

/* ═══════════════════════════════════════════════════════════════════
 *  Case-insensitive string comparison
 * ═══════════════════════════════════════════════════════════════════ */

#ifdef _WIN32
#define xnet_strcasecmp(s1, s2)     _stricmp(s1, s2)
#define xnet_strncasecmp(s1, s2, n) _strnicmp(s1, s2, n)
#else
#define xnet_strcasecmp(s1, s2)     strcasecmp(s1, s2)
#define xnet_strncasecmp(s1, s2, n) strncasecmp(s1, s2, n)
#endif

/* ═══════════════════════════════════════════════════════════════════
 *  Secure random bytes
 * ═══════════════════════════════════════════════════════════════════ */

#ifdef _WIN32
#include <bcrypt.h>
static inline void xnet_random_bytes(void *buf, size_t len) {
  BCryptGenRandom(NULL, (PUCHAR)buf, (ULONG)len,
                  BCRYPT_USE_SYSTEM_PREFERRED_RNG);
}
#elif defined(__linux__)
#include <sys/random.h>
static inline void xnet_random_bytes(void *buf, size_t len) {
  (void)getrandom(buf, len, 0);
}
#else
/* macOS / BSD */
static inline void xnet_random_bytes(void *buf, size_t len) {
  arc4random_buf(buf, len);
}
#endif

/* ═══════════════════════════════════════════════════════════════════
 *  SIGPIPE suppression
 * ═══════════════════════════════════════════════════════════════════ */

#ifdef _WIN32
/* Windows has no SIGPIPE; no-op. */
#define xnet_ignore_sigpipe() ((void)0)
#else
#include <signal.h>
#define xnet_ignore_sigpipe() signal(SIGPIPE, SIG_IGN)
#endif

/* ═══════════════════════════════════════════════════════════════════
 *  poll() abstraction
 * ═══════════════════════════════════════════════════════════════════ */

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
/* Windows uses WSAPoll which has the same signature as poll() */
#define xnet_poll(fds, nfds, timeout) WSAPoll((fds), (nfds), (timeout))
#else
#include <poll.h>
#define xnet_poll(fds, nfds, timeout) poll((fds), (nfds), (timeout))
#endif

/* ═══════════════════════════════════════════════════════════════════
 *  unlink() abstraction
 * ═══════════════════════════════════════════════════════════════════ */

#ifdef _WIN32
#include <io.h>
#define xnet_unlink(path) _unlink(path)
#else
#include <unistd.h>
#define xnet_unlink(path) unlink(path)
#endif

/* ═══════════════════════════════════════════════════════════════════
 *  strndup() — not available on MSVC
 * ═══════════════════════════════════════════════════════════════════ */

#ifdef _WIN32
#include <stdlib.h>
#include <string.h>
static inline char *xnet_strndup(const char *s, size_t n) {
  size_t len = strnlen(s, n);
  char  *dup = (char *)malloc(len + 1);
  if (dup) {
    memcpy(dup, s, len);
    dup[len] = '\0';
  }
  return dup;
}
#else
#define xnet_strndup(s, n) strndup(s, n)
#endif

/* ═══════════════════════════════════════════════════════════════════
 *  getpid() — MSVC uses _getpid()
 * ═══════════════════════════════════════════════════════════════════ */

#ifdef _WIN32
#include <process.h>
#define xnet_getpid() ((int)_getpid())
#else
#include <unistd.h>
#define xnet_getpid() ((int)getpid())
#endif

/* ═══════════════════════════════════════════════════════════════════
 *  stderr redirect for shell commands — "2>/dev/null" vs "2>NUL"
 * ═══════════════════════════════════════════════════════════════════ */

#ifdef _WIN32
#define XNET_DEVNULL "NUL"
#else
#define XNET_DEVNULL "/dev/null"
#endif

/* ═══════════════════════════════════════════════════════════════════
 *  Temporary directory path
 * ═══════════════════════════════════════════════════════════════════ */

#ifdef _WIN32
static inline const char *xnet_tempdir(void) {
  static char buf[MAX_PATH];
  if (GetTempPathA(MAX_PATH, buf) > 0) return buf;
  return "C:\\Temp";
}
#else
static inline const char *xnet_tempdir(void) {
  return "/tmp";
}
#endif

#endif /* XNET_COMPAT_H */
